#include "hiraco_core.h"

#include <wx/bitmap.h>
#include <wx/bmpbuttn.h>
#include <wx/button.h>
#include <wx/dcbuffer.h>
#include <wx/dnd.h>
#include <wx/dirctrl.h>
#include <wx/filedlg.h>
#include <wx/filepicker.h>
#include <wx/graphics.h>
#include <wx/listctrl.h>
#include <wx/menu.h>
#include <wx/msgdlg.h>
#include <wx/scrolwin.h>
#include <wx/sizer.h>
#include <wx/statbmp.h>
#include <wx/statline.h>
#include <wx/stattext.h>
#include <wx/timer.h>
#include <wx/wx.h>

#include <algorithm>
#include <atomic>
#include <cstring>
#include <filesystem>
#include <future>
#include <memory>
#include <optional>
#include <thread>
#include <vector>

namespace {

wxDECLARE_EVENT(EVT_HIRACO_SELECTION_READY, wxThreadEvent);
wxDECLARE_EVENT(EVT_HIRACO_CROP_READY, wxThreadEvent);
wxDECLARE_EVENT(EVT_HIRACO_CONVERT_PROGRESS, wxThreadEvent);
wxDECLARE_EVENT(EVT_HIRACO_CONVERT_DONE, wxThreadEvent);

wxDEFINE_EVENT(EVT_HIRACO_SELECTION_READY, wxThreadEvent);
wxDEFINE_EVENT(EVT_HIRACO_CROP_READY, wxThreadEvent);
wxDEFINE_EVENT(EVT_HIRACO_CONVERT_PROGRESS, wxThreadEvent);
wxDEFINE_EVENT(EVT_HIRACO_CONVERT_DONE, wxThreadEvent);

struct QueueItem {
  uint64_t id = 0;
  std::string source_path;
  std::filesystem::path target_path;
  std::optional<PreparedSource> prepared;
  wxString state = "Ready";
  wxString message;
};

struct SelectionReadyPayload {
  uint64_t item_id = 0;
  uint64_t request_id = 0;
  bool ok = false;
  PreparedSource prepared;
  std::shared_ptr<PreviewImage> original_preview;
  CropRect crop_rect;
  std::string error;
};

struct CropReadyPayload {
  uint64_t item_id = 0;
  uint64_t request_id = 0;
  bool ok = false;
  CropRect crop_rect;
  std::shared_ptr<PreviewImage> crop_preview;
  std::string error;
};

struct ConvertProgressPayload {
  uint64_t item_id = 0;
  double overall_fraction = 0.0;
  std::string message;
};

struct ConvertDonePayload {
  uint64_t item_id = 0;
  bool ok = false;
  bool skipped = false;
  bool canceled = false;
  std::string message;
};

wxString ProcessedMarkerForItem(const QueueItem& item) {
  if (item.state == "Done") {
    return wxString::FromUTF8("✓");
  }
  if (item.state == "Skipped") {
    return "-";
  }
  if (item.state == "Loading" || item.state == "Converting") {
    return wxString::FromUTF8("…");
  }
  if (item.state == "Failed" || item.state == "Canceled") {
    return wxString::FromUTF8("✗");
  }
  return wxString();
}

wxBitmap MakeBitmapFromPreview(std::shared_ptr<const PreviewImage> preview) {
  if (!preview || preview->width == 0 || preview->height == 0 || preview->colors < 3) {
    return wxBitmap();
  }

  wxImage image(preview->width, preview->height);
  unsigned char* rgb = image.GetData();
  const size_t pixel_count = static_cast<size_t>(preview->width) * preview->height;
  if (preview->colors == 3) {
    std::memcpy(rgb, preview->pixels.data(), pixel_count * 3);
  } else {
    for (uint32_t row = 0; row < preview->height; ++row) {
      for (uint32_t col = 0; col < preview->width; ++col) {
        const size_t src_index = (static_cast<size_t>(row) * preview->width + col) * preview->colors;
        const size_t dst_index = (static_cast<size_t>(row) * preview->width + col) * 3;
        rgb[dst_index + 0] = preview->pixels[src_index + 0];
        rgb[dst_index + 1] = preview->pixels[src_index + 1];
        rgb[dst_index + 2] = preview->pixels[src_index + 2];
      }
    }
  }
  return wxBitmap(image);
}

std::shared_ptr<PreviewImage> CropPreviewImage(std::shared_ptr<const PreviewImage> preview,
                                               const CropRect& crop_rect) {
  if (!preview || preview->width == 0 || preview->height == 0 || preview->colors == 0) {
    return nullptr;
  }

  const CropRect clamped = ClampCropRect(crop_rect, preview->width, preview->height);
  auto cropped = std::make_shared<PreviewImage>();
  cropped->width = clamped.width;
  cropped->height = clamped.height;
  cropped->colors = preview->colors;
  cropped->bits = preview->bits;
  cropped->pixels.resize(static_cast<size_t>(cropped->width) * cropped->height * cropped->colors);

  const size_t row_bytes = static_cast<size_t>(clamped.width) * cropped->colors;
  for (uint32_t row = 0; row < clamped.height; ++row) {
    const size_t src_index =
        (static_cast<size_t>(clamped.y + row) * preview->width + clamped.x) * preview->colors;
    const size_t dst_index = static_cast<size_t>(row) * cropped->width * cropped->colors;
    std::memcpy(cropped->pixels.data() + dst_index,
                preview->pixels.data() + src_index,
                row_bytes);
  }

  return cropped;
}

class FitImagePanel final : public wxPanel {
 public:
  explicit FitImagePanel(wxWindow* parent)
      : wxPanel(parent, wxID_ANY, wxDefaultPosition, wxSize(280, 280), wxBORDER_SIMPLE) {
    SetBackgroundStyle(wxBG_STYLE_PAINT);
    Bind(wxEVT_PAINT, &FitImagePanel::OnPaint, this);
    Bind(wxEVT_SIZE, &FitImagePanel::OnSize, this);
  }

  void SetPreview(std::shared_ptr<const PreviewImage> preview) {
    bitmap_ = MakeBitmapFromPreview(preview);
    Refresh();
  }

 private:
  void OnPaint(wxPaintEvent&) {
    wxAutoBufferedPaintDC dc(this);
    dc.SetBackground(wxBrush(wxColour(20, 20, 20)));
    dc.Clear();

    if (!bitmap_.IsOk()) {
      dc.SetTextForeground(*wxLIGHT_GREY);
      dc.DrawText("Converted crop preview", 16, 16);
      return;
    }

    const wxSize client = GetClientSize();
    const double scale_x = static_cast<double>(client.GetWidth()) / bitmap_.GetWidth();
    const double scale_y = static_cast<double>(client.GetHeight()) / bitmap_.GetHeight();
    const double scale = std::min(scale_x, scale_y);
    const int draw_width = std::max(1, static_cast<int>(std::round(bitmap_.GetWidth() * scale)));
    const int draw_height = std::max(1, static_cast<int>(std::round(bitmap_.GetHeight() * scale)));
    const int offset_x = (client.GetWidth() - draw_width) / 2;
    const int offset_y = (client.GetHeight() - draw_height) / 2;

    std::unique_ptr<wxGraphicsContext> gc(wxGraphicsContext::Create(dc));
    if (gc) {
      gc->DrawBitmap(bitmap_, offset_x, offset_y, draw_width, draw_height);
    } else {
      dc.DrawBitmap(bitmap_.ConvertToImage().Scale(draw_width, draw_height), offset_x, offset_y);
    }
  }

  void OnSize(wxSizeEvent& event) {
    Refresh();
    event.Skip();
  }

  wxBitmap bitmap_;
};

class PreviewCanvas final : public wxScrolledWindow {
 public:
  enum class ZoomMode {
    kFit,
    k25,
    k50,
    k100,
  };

  explicit PreviewCanvas(wxWindow* parent)
      : wxScrolledWindow(parent, wxID_ANY, wxDefaultPosition, wxSize(800, 600), wxBORDER_SIMPLE) {
    SetBackgroundStyle(wxBG_STYLE_PAINT);
    SetScrollRate(1, 1);
    Bind(wxEVT_PAINT, &PreviewCanvas::OnPaint, this);
    Bind(wxEVT_SIZE, &PreviewCanvas::OnSize, this);
    Bind(wxEVT_LEFT_DOWN, &PreviewCanvas::OnLeftDown, this);
    Bind(wxEVT_LEFT_UP, &PreviewCanvas::OnLeftUp, this);
    Bind(wxEVT_MOTION, &PreviewCanvas::OnMotion, this);
  }

  void SetPreview(std::shared_ptr<const PreviewImage> preview) {
    preview_ = std::move(preview);
    bitmap_ = MakeBitmapFromPreview(preview_);
    UpdateVirtualArea();
    if (!preview_) {
      Scroll(0, 0);
    }
    Refresh();
  }

  void SetCropRect(const CropRect& crop_rect) {
    crop_rect_ = crop_rect;
    Refresh();
  }

  void SetZoomMode(ZoomMode mode) {
    zoom_mode_ = mode;
    UpdateVirtualArea();
    if (zoom_mode_ == ZoomMode::kFit) {
      Scroll(0, 0);
    } else {
      CenterCropRectInView();
    }
    Refresh();
  }

  void SetCropChangedHandler(std::function<void(const CropRect&)> handler) {
    crop_changed_handler_ = std::move(handler);
  }

 private:
  double CurrentScale() const {
    if (!preview_ || preview_->width == 0 || preview_->height == 0) {
      return 1.0;
    }

    switch (zoom_mode_) {
      case ZoomMode::k25:
        return 0.25;
      case ZoomMode::k50:
        return 0.5;
      case ZoomMode::k100:
        return 1.0;
      case ZoomMode::kFit: {
        const wxSize client = GetClientSize();
        const double scale_x = static_cast<double>(client.GetWidth()) / preview_->width;
        const double scale_y = static_cast<double>(client.GetHeight()) / preview_->height;
        return std::max(0.05, std::min(scale_x, scale_y));
      }
    }
    return 1.0;
  }

  wxPoint ToImagePoint(const wxPoint& point) const {
    int logical_x = 0;
    int logical_y = 0;
    CalcUnscrolledPosition(point.x, point.y, &logical_x, &logical_y);
    const double scale = CurrentScale();
    return wxPoint(static_cast<int>(std::floor(logical_x / scale)),
                   static_cast<int>(std::floor(logical_y / scale)));
  }

  wxRect CropRectToView() const {
    const double scale = CurrentScale();
    return wxRect(static_cast<int>(std::round(crop_rect_.x * scale)),
                  static_cast<int>(std::round(crop_rect_.y * scale)),
                  static_cast<int>(std::round(crop_rect_.width * scale)),
                  static_cast<int>(std::round(crop_rect_.height * scale)));
  }

  void UpdateVirtualArea() {
    if (!preview_) {
      SetVirtualSize(0, 0);
      Scroll(0, 0);
      return;
    }
    const double scale = CurrentScale();
    SetVirtualSize(static_cast<int>(std::round(preview_->width * scale)),
                   static_cast<int>(std::round(preview_->height * scale)));
  }

  void CenterCropRectInView() {
    if (!preview_ || crop_rect_.width == 0 || crop_rect_.height == 0) {
      return;
    }

    const wxRect crop_rect = CropRectToView();
    const wxSize client = GetClientSize();
    const wxSize virtual_size = GetVirtualSize();
    const int max_x = std::max(0, virtual_size.GetWidth() - client.GetWidth());
    const int max_y = std::max(0, virtual_size.GetHeight() - client.GetHeight());
    const int target_x = std::clamp(crop_rect.x + crop_rect.width / 2 - client.GetWidth() / 2,
                                    0,
                                    max_x);
    const int target_y = std::clamp(crop_rect.y + crop_rect.height / 2 - client.GetHeight() / 2,
                                    0,
                                    max_y);
    Scroll(target_x, target_y);
  }

  void OnPaint(wxPaintEvent&) {
    wxAutoBufferedPaintDC dc(this);
    PrepareDC(dc);
    dc.SetBackground(wxBrush(wxColour(18, 18, 18)));
    dc.Clear();

    if (!bitmap_.IsOk()) {
      dc.SetTextForeground(*wxLIGHT_GREY);
      dc.DrawText("Original preview", 16, 16);
      return;
    }

    const double scale = CurrentScale();
    const int draw_width = std::max(1, static_cast<int>(std::round(bitmap_.GetWidth() * scale)));
    const int draw_height = std::max(1, static_cast<int>(std::round(bitmap_.GetHeight() * scale)));

    std::unique_ptr<wxGraphicsContext> gc(wxGraphicsContext::Create(dc));
    if (gc) {
      gc->DrawBitmap(bitmap_, 0, 0, draw_width, draw_height);
      gc->SetPen(wxPen(wxColour(255, 193, 7), 2.0));
      gc->SetBrush(*wxTRANSPARENT_BRUSH);
      const wxRect crop_rect = CropRectToView();
      gc->DrawRectangle(crop_rect.x, crop_rect.y, crop_rect.width, crop_rect.height);
    } else {
      dc.DrawBitmap(bitmap_.ConvertToImage().Scale(draw_width, draw_height), 0, 0);
      dc.SetPen(wxPen(wxColour(255, 193, 7), 2));
      dc.SetBrush(*wxTRANSPARENT_BRUSH);
      dc.DrawRectangle(CropRectToView());
    }
  }

  void OnSize(wxSizeEvent& event) {
    if (zoom_mode_ == ZoomMode::kFit) {
      UpdateVirtualArea();
    }
    Refresh();
    event.Skip();
  }

  void OnLeftDown(wxMouseEvent& event) {
    if (!bitmap_.IsOk()) {
      return;
    }

    const wxPoint image_point = ToImagePoint(event.GetPosition());
    int logical_x = 0;
    int logical_y = 0;
    CalcUnscrolledPosition(event.GetPosition().x, event.GetPosition().y, &logical_x, &logical_y);
    const wxRect crop_rect = CropRectToView();
    left_down_point_ = event.GetPosition();
    left_down_inside_crop_ = crop_rect.Contains(logical_x, logical_y);
    dragging_crop_ = false;
    panning_ = false;
    if (crop_rect.Contains(logical_x, logical_y)) {
      drag_origin_image_ = image_point;
      drag_start_crop_ = crop_rect_;
    } else {
      last_pan_point_ = event.GetPosition();
    }
    CaptureMouse();
  }

  void OnLeftUp(wxMouseEvent& event) {
    if (!dragging_crop_ && !panning_ && bitmap_.IsOk() && preview_) {
      const wxPoint image_point = ToImagePoint(event.GetPosition());
      CropRect updated = crop_rect_;
      updated.x = static_cast<uint32_t>(
          std::clamp(image_point.x - static_cast<int>(updated.width / 2),
                     0,
                     static_cast<int>(preview_->width - updated.width)));
      updated.y = static_cast<uint32_t>(
          std::clamp(image_point.y - static_cast<int>(updated.height / 2),
                     0,
                     static_cast<int>(preview_->height - updated.height)));
      if (updated.x != crop_rect_.x || updated.y != crop_rect_.y) {
        crop_rect_ = updated;
        Refresh();
        if (crop_changed_handler_) {
          crop_changed_handler_(crop_rect_);
        }
      }
    }

    dragging_crop_ = false;
    panning_ = false;
    left_down_inside_crop_ = false;
    if (HasCapture()) {
      ReleaseMouse();
    }
  }

  void OnMotion(wxMouseEvent& event) {
    if (!event.Dragging() || !event.LeftIsDown() || !preview_ || preview_->width == 0 || preview_->height == 0) {
      return;
    }

    if (left_down_inside_crop_) {
      if (!dragging_crop_) {
        const wxPoint current = event.GetPosition();
        const int travel = std::abs(current.x - left_down_point_.x) + std::abs(current.y - left_down_point_.y);
        if (travel < 4) {
          return;
        }
        dragging_crop_ = true;
      }

      const wxPoint current_image = ToImagePoint(event.GetPosition());
      const int dx = current_image.x - drag_origin_image_.x;
      const int dy = current_image.y - drag_origin_image_.y;

      CropRect updated = drag_start_crop_;
      updated.x = static_cast<uint32_t>(std::max(0, std::min<int>(static_cast<int>(preview_->width - updated.width),
                                                                  static_cast<int>(drag_start_crop_.x) + dx)));
      updated.y = static_cast<uint32_t>(std::max(0, std::min<int>(static_cast<int>(preview_->height - updated.height),
                                                                  static_cast<int>(drag_start_crop_.y) + dy)));
      crop_rect_ = updated;
      Refresh();
      if (crop_changed_handler_) {
        crop_changed_handler_(crop_rect_);
      }
      return;
    }

    if (!left_down_inside_crop_ && zoom_mode_ != ZoomMode::kFit) {
      wxPoint current = event.GetPosition();
      if (!panning_) {
        const int travel = std::abs(current.x - left_down_point_.x) + std::abs(current.y - left_down_point_.y);
        if (travel < 4) {
          return;
        }
        panning_ = true;
        last_pan_point_ = current;
      }

      const int dx = current.x - last_pan_point_.x;
      const int dy = current.y - last_pan_point_.y;
      int view_x = 0;
      int view_y = 0;
      GetViewStart(&view_x, &view_y);
      Scroll(std::max(0, view_x - dx), std::max(0, view_y - dy));
      last_pan_point_ = current;
    }
  }

  std::shared_ptr<const PreviewImage> preview_;
  wxBitmap bitmap_;
  CropRect crop_rect_;
  ZoomMode zoom_mode_ = ZoomMode::kFit;
  bool dragging_crop_ = false;
  bool panning_ = false;
  bool left_down_inside_crop_ = false;
  wxPoint drag_origin_image_;
  wxPoint left_down_point_;
  wxPoint last_pan_point_;
  CropRect drag_start_crop_;
  std::function<void(const CropRect&)> crop_changed_handler_;
};

class OverwriteDialog final : public wxDialog {
 public:
  OverwriteDialog(wxWindow* parent, const std::filesystem::path& path)
      : wxDialog(parent, wxID_ANY, "Target File Exists", wxDefaultPosition, wxDefaultSize,
                 wxDEFAULT_DIALOG_STYLE | wxRESIZE_BORDER) {
    auto* root = new wxBoxSizer(wxVERTICAL);
    root->Add(new wxStaticText(this,
                               wxID_ANY,
                               "The target DNG already exists:\n" + wxString(path.string())),
              0,
              wxALL | wxEXPAND,
              12);

    auto* buttons = new wxBoxSizer(wxHORIZONTAL);
    AddResponseButton(buttons, "Yes", OverwriteResponse::kYes);
    AddResponseButton(buttons, "Yes to All", OverwriteResponse::kYesToAll);
    AddResponseButton(buttons, "No", OverwriteResponse::kNo);
    AddResponseButton(buttons, "No to All", OverwriteResponse::kNoToAll);
    AddResponseButton(buttons, "Cancel", OverwriteResponse::kCancel);
    root->Add(buttons, 0, wxALL | wxALIGN_CENTER_HORIZONTAL, 12);
    SetSizerAndFit(root);
  }

  OverwriteResponse ShowModalAndGetResponse() {
    ShowModal();
    return response_;
  }

 private:
  void AddResponseButton(wxSizer* sizer, const wxString& label, OverwriteResponse response) {
    auto* button = new wxButton(this, wxID_ANY, label);
    button->Bind(wxEVT_BUTTON, [this, response](wxCommandEvent&) {
      response_ = response;
      EndModal(wxID_OK);
    });
    sizer->Add(button, 0, wxRIGHT, 8);
  }

  OverwriteResponse response_ = OverwriteResponse::kCancel;
};

class HiracoMainFrame;

class HiracoDropTarget final : public wxFileDropTarget {
 public:
  explicit HiracoDropTarget(HiracoMainFrame* frame)
      : frame_(frame) {}

  bool OnDropFiles(wxCoord, wxCoord, const wxArrayString& filenames) override;

 private:
  HiracoMainFrame* frame_;
};

struct SliderControl {
  wxSlider* slider = nullptr;
  wxStaticText* value = nullptr;
  double min_value = 0.0;
  double max_value = 1.0;
  int scale = 100;
  int decimals = 2;
};

class HiracoMainFrame final : public wxFrame {
 public:
  HiracoMainFrame()
      : wxFrame(nullptr, wxID_ANY, "hiraco-gui", wxDefaultPosition, wxSize(1600, 980)),
  shutdown_timer_(this),
  crop_preview_timer_(this) {
    SetDropTarget(new HiracoDropTarget(this));
    BuildUi();
    BuildAppMenuBar();
    BindEvents();

    base_output_dir_ = std::filesystem::current_path();
    output_dir_picker_->SetPath(base_output_dir_.string());
    UpdateCompressionChoice();
    UpdateResolvedSliderValues();
    UpdateButtons();
  }

  void AddFiles(const std::vector<std::string>& paths) {
    for (const std::string& path : paths) {
      const auto duplicate = std::find_if(queue_.begin(), queue_.end(), [&](const QueueItem& item) {
        return item.source_path == path;
      });
      if (duplicate != queue_.end()) {
        continue;
      }

      QueueItem item;
      item.id = next_item_id_++;
      item.source_path = path;
      item.target_path = ResolveOutputPath(path, base_output_dir_, relative_subdir_);
      queue_.push_back(item);
    }
    RefreshQueue();
    if (selected_row_ < 0 && !queue_.empty()) {
      SelectRow(0);
    }
  }

 private:
  template <typename Task>
  void LaunchWorker(Task task) {
    active_workers_.fetch_add(1);
    std::thread([this, task]() mutable {
      task();
      active_workers_.fetch_sub(1);
      CallAfter([this]() { MaybeFinishClose(); });
    }).detach();
  }

  void BuildAppMenuBar() {
    auto* file_menu = new wxMenu();
    file_menu->Append(wxID_EXIT, "&Quit\tCtrl-Q");

    auto* menu_bar = new wxMenuBar();
    menu_bar->Append(file_menu, "&File");
    SetMenuBar(menu_bar);
  }

  void BuildUi() {
    auto* root = new wxBoxSizer(wxHORIZONTAL);
    auto make_section_label = [](wxWindow* parent, const wxString& text) {
      auto* label = new wxStaticText(parent, wxID_ANY, text);
      wxFont font = label->GetFont();
      font.MakeBold();
      label->SetFont(font);
      return label;
    };

    auto* left_panel = new wxPanel(this);
    auto* left_sizer = new wxBoxSizer(wxVERTICAL);
    auto* queue_buttons = new wxBoxSizer(wxHORIZONTAL);
    add_files_button_ = new wxButton(left_panel, wxID_ANY, "Add Files");
    remove_button_ = new wxButton(left_panel, wxID_ANY, "Remove Selected");
    clear_button_ = new wxButton(left_panel, wxID_ANY, "Clear");
    queue_buttons->Add(add_files_button_, 0, wxRIGHT, 8);
    queue_buttons->Add(remove_button_, 0, wxRIGHT, 8);
    queue_buttons->Add(clear_button_, 0);

    queue_ctrl_ = new wxListCtrl(left_panel, wxID_ANY, wxDefaultPosition, wxSize(420, -1),
                                 wxLC_REPORT | wxLC_HRULES | wxLC_VRULES);
  queue_ctrl_->AppendColumn("File", wxLIST_FORMAT_LEFT, 120);
  queue_ctrl_->AppendColumn("Target", wxLIST_FORMAT_LEFT, 235);
    queue_ctrl_->AppendColumn("Processed", wxLIST_FORMAT_CENTER, 70);

    left_sizer->Add(queue_buttons, 0, wxALL | wxEXPAND, 10);
    left_sizer->Add(queue_ctrl_, 1, wxLEFT | wxRIGHT | wxBOTTOM | wxEXPAND, 10);
    left_panel->SetSizer(left_sizer);

    auto* center_panel = new wxPanel(this);
    auto* center_sizer = new wxBoxSizer(wxVERTICAL);
    auto* zoom_row = new wxBoxSizer(wxHORIZONTAL);
    zoom_row->Add(new wxStaticText(center_panel, wxID_ANY, "Zoom:"), 0, wxALIGN_CENTER_VERTICAL | wxRIGHT, 8);
    zoom_choice_ = new wxChoice(center_panel, wxID_ANY);
    zoom_choice_->Append("Fit");
    zoom_choice_->Append("25%");
    zoom_choice_->Append("50%");
    zoom_choice_->Append("100%");
    zoom_choice_->SetSelection(0);
    zoom_row->Add(zoom_choice_, 0);
    zoom_row->AddStretchSpacer();
    auto* crop_hint = new wxStaticText(center_panel, wxID_ANY, "Drag or click the yellow box to move crop");
    crop_hint->SetForegroundColour(wxColour(110, 110, 110));
    zoom_row->Add(crop_hint, 0, wxALIGN_CENTER_VERTICAL | wxLEFT, 12);

    original_canvas_ = new PreviewCanvas(center_panel);
    center_sizer->Add(zoom_row, 0, wxALL, 10);
    center_sizer->Add(original_canvas_, 1, wxLEFT | wxRIGHT | wxBOTTOM | wxEXPAND, 10);
    center_panel->SetSizer(center_sizer);

    auto* right_panel = new wxPanel(this);
    right_panel->SetMinSize(wxSize(430, -1));
    auto* right_sizer = new wxBoxSizer(wxVERTICAL);

    right_sizer->Add(make_section_label(right_panel, "Converted Crop"), 0, wxLEFT | wxTOP, 10);
    crop_preview_panel_ = new FitImagePanel(right_panel);
    right_sizer->Add(crop_preview_panel_, 0, wxALL | wxEXPAND, 10);
    right_sizer->Add(new wxStaticLine(right_panel), 0, wxLEFT | wxRIGHT | wxBOTTOM | wxEXPAND, 10);

    right_sizer->Add(make_section_label(right_panel, "Output"), 0, wxLEFT | wxRIGHT | wxBOTTOM, 10);
    auto* output_panel = new wxPanel(right_panel);
    auto* output_sizer = new wxBoxSizer(wxVERTICAL);
    auto* compression_row = new wxBoxSizer(wxHORIZONTAL);
    compression_row->Add(new wxStaticText(output_panel, wxID_ANY, "Compression"),
                         0,
                         wxALIGN_CENTER_VERTICAL | wxRIGHT,
                         8);
    compression_choice_ = new wxChoice(output_panel, wxID_ANY);
    compression_choice_->Append("Uncompressed");
    compression_choice_->Append("Deflate");
    compression_choice_->Append("JPEG XL");
    compression_choice_->SetSelection(0);
    compression_row->Add(compression_choice_, 1);
    output_sizer->Add(compression_row, 0, wxEXPAND);
    output_sizer->Add(new wxStaticText(output_panel, wxID_ANY, "Output Directory"),
              0,
              wxTOP,
              10);
    output_dir_picker_ = new wxDirPickerCtrl(output_panel, wxID_ANY);
    output_sizer->Add(output_dir_picker_, 0, wxTOP | wxEXPAND, 10);
    output_sizer->Add(new wxStaticText(output_panel, wxID_ANY, "Relative Subfolder"),
              0,
              wxTOP,
              10);
    relative_subdir_ctrl_ = new wxTextCtrl(output_panel, wxID_ANY);
    output_sizer->Add(relative_subdir_ctrl_, 0, wxTOP | wxEXPAND, 10);
    output_panel->SetSizer(output_sizer);
    right_sizer->Add(output_panel, 0, wxLEFT | wxRIGHT | wxBOTTOM | wxEXPAND, 10);
    right_sizer->Add(new wxStaticLine(right_panel), 0, wxLEFT | wxRIGHT | wxBOTTOM | wxEXPAND, 10);

    right_sizer->Add(make_section_label(right_panel, "Processing"), 0, wxLEFT | wxRIGHT | wxBOTTOM, 10);
    auto* sliders_scroll =
      new wxScrolledWindow(right_panel, wxID_ANY, wxDefaultPosition, wxDefaultSize, wxVSCROLL);
    sliders_scroll->SetScrollRate(0, 16);
    auto* sliders_sizer = new wxBoxSizer(wxVERTICAL);

    auto* stage1_section = new wxPanel(sliders_scroll, wxID_ANY, wxDefaultPosition, wxDefaultSize, wxBORDER_THEME);
    auto* stage1_sizer = CreateStageSectionSizer(stage1_section,
                           "Detail Recovery",
                           "Recover fine detail from the stacked capture before later refinements.",
                           &stage1_reset_button_);
    stage1_sizer->Add(CreateFloatSlider(stage1_section, "Blur Radius", 0.50, 4.00, 100, 2, &stage1_sigma_),
                      0,
                      wxBOTTOM | wxEXPAND,
                      8);
    stage1_sizer->Add(CreateFloatSlider(stage1_section, "Noise Protection", 0.00, 0.20, 1000, 3, &stage1_nsr_),
                      0,
                      wxEXPAND,
                      0);
    stage1_section->SetSizer(stage1_sizer);
    sliders_sizer->Add(stage1_section, 0, wxLEFT | wxRIGHT | wxTOP | wxEXPAND, 10);

    auto* stage2_section = new wxPanel(sliders_scroll, wxID_ANY, wxDefaultPosition, wxDefaultSize, wxBORDER_THEME);
    auto* stage2_sizer = CreateStageSectionSizer(stage2_section,
                           "Multi-scale Detail",
                           "Balance denoising and sharpening across fine to coarse texture bands.",
                           &stage2_reset_button_);
    stage2_sizer->Add(CreateFloatSlider(stage2_section, "Denoise", 0.00, 1.00, 100, 2, &stage2_denoise_),
                      0,
                      wxBOTTOM | wxEXPAND,
                      8);
    stage2_sizer->Add(CreateFloatSlider(stage2_section, "Fine Detail", 0.50, 3.00, 100, 2, &stage2_gain0_),
                      0,
                      wxBOTTOM | wxEXPAND,
                      8);
    stage2_sizer->Add(CreateFloatSlider(stage2_section, "Small Detail", 0.50, 3.00, 100, 2, &stage2_gain1_),
                      0,
                      wxBOTTOM | wxEXPAND,
                      8);
    stage2_sizer->Add(CreateFloatSlider(stage2_section, "Medium Detail", 0.50, 3.00, 100, 2, &stage2_gain2_),
                      0,
                      wxBOTTOM | wxEXPAND,
                      8);
    stage2_sizer->Add(CreateFloatSlider(stage2_section, "Large Detail", 0.50, 3.00, 100, 2, &stage2_gain3_),
                      0,
                      wxEXPAND,
                      0);
    stage2_section->SetSizer(stage2_sizer);
    sliders_sizer->Add(stage2_section, 0, wxLEFT | wxRIGHT | wxTOP | wxEXPAND, 10);

    auto* stage3_section = new wxPanel(sliders_scroll, wxID_ANY, wxDefaultPosition, wxDefaultSize, wxBORDER_THEME);
    auto* stage3_sizer = CreateStageSectionSizer(stage3_section,
                           "Edge Refinement",
                           "Protect strong edges while adding local contrast and keeping halos under control.",
                           &stage3_reset_button_);
    stage3_sizer->Add(CreateIntSlider(stage3_section, "Edge Radius", 1, 16, &stage3_radius_),
                      0,
                      wxBOTTOM | wxEXPAND,
                      8);
    stage3_sizer->Add(CreateFloatSlider(stage3_section, "Edge Gain", 0.00, 2.00, 100, 2, &stage3_gain_),
                      0,
                      wxEXPAND,
                      0);
    stage3_section->SetSizer(stage3_sizer);
    sliders_sizer->Add(stage3_section, 0, wxLEFT | wxRIGHT | wxTOP | wxEXPAND, 10);

    sliders_sizer->AddSpacer(10);
    sliders_scroll->SetSizer(sliders_sizer);
    sliders_scroll->FitInside();
    right_sizer->Add(sliders_scroll, 1, wxLEFT | wxRIGHT | wxBOTTOM | wxEXPAND, 10);
    right_sizer->Add(new wxStaticLine(right_panel), 0, wxLEFT | wxRIGHT | wxBOTTOM | wxEXPAND, 10);

    auto* action_row = new wxBoxSizer(wxHORIZONTAL);
    reset_button_ = new wxButton(right_panel, wxID_ANY, "Restore All Defaults");
    convert_button_ = new wxButton(right_panel, wxID_ANY, "Convert");
    cancel_button_ = new wxButton(right_panel, wxID_ANY, "Cancel");
    action_row->Add(reset_button_, 1, wxRIGHT, 8);
    action_row->Add(convert_button_, 1, wxRIGHT, 8);
    action_row->Add(cancel_button_, 1);
    right_sizer->Add(action_row, 0, wxALL | wxEXPAND, 10);

    progress_gauge_ = new wxGauge(right_panel, wxID_ANY, 100);
    status_label_ = new wxStaticText(right_panel, wxID_ANY, "Ready");
    right_sizer->Add(progress_gauge_, 0, wxLEFT | wxRIGHT | wxTOP | wxEXPAND, 10);
    right_sizer->Add(status_label_, 0, wxALL | wxEXPAND, 10);
    right_panel->SetSizer(right_sizer);

    root->Add(left_panel, 0, wxEXPAND);
    root->Add(center_panel, 1, wxEXPAND);
    root->Add(right_panel, 0, wxEXPAND);
    SetSizer(root);
  }

  wxBoxSizer* CreateStageSectionSizer(wxWindow* parent,
                                      const wxString& title,
                                      const wxString& description,
                                      wxButton** reset_button) {
    auto* section_sizer = new wxBoxSizer(wxVERTICAL);
    auto* header_row = new wxBoxSizer(wxHORIZONTAL);
    auto* title_label = new wxStaticText(parent, wxID_ANY, title);
    wxFont title_font = title_label->GetFont();
    title_font.MakeBold();
    title_label->SetFont(title_font);
    header_row->Add(title_label, 1, wxALIGN_CENTER_VERTICAL | wxRIGHT, 8);
    *reset_button = new wxButton(parent, wxID_ANY, "Restore");
    header_row->Add(*reset_button, 0);
    section_sizer->Add(header_row, 0, wxALL | wxEXPAND, 10);

    auto* description_label = new wxStaticText(parent, wxID_ANY, description);
    description_label->SetForegroundColour(wxColour(110, 110, 110));
    description_label->Wrap(330);
    section_sizer->Add(description_label, 0, wxLEFT | wxRIGHT | wxBOTTOM | wxEXPAND, 10);
    return section_sizer;
  }

  wxWindow* CreateFloatSlider(wxWindow* parent,
                              const wxString& label,
                              double min_value,
                              double max_value,
                              int scale,
                              int decimals,
                              SliderControl* out_control) {
    auto* panel = new wxPanel(parent);
    auto* row = new wxBoxSizer(wxVERTICAL);
    auto* header = new wxBoxSizer(wxHORIZONTAL);
    auto* label_ctrl = new wxStaticText(panel, wxID_ANY, label);
    auto* value_ctrl = new wxStaticText(panel, wxID_ANY, "0");
    auto* slider = new wxSlider(panel,
                                wxID_ANY,
                                static_cast<int>(min_value * scale),
                                static_cast<int>(min_value * scale),
                                static_cast<int>(max_value * scale));
    header->Add(label_ctrl, 1, wxRIGHT, 8);
    header->Add(value_ctrl, 0);
    row->Add(header, 0, wxBOTTOM | wxEXPAND, 4);
    row->Add(slider, 0, wxEXPAND);
    panel->SetSizer(row);

    out_control->slider = slider;
    out_control->value = value_ctrl;
    out_control->min_value = min_value;
    out_control->max_value = max_value;
    out_control->scale = scale;
    out_control->decimals = decimals;
    UpdateSliderLabel(*out_control);
    return panel;
  }

  wxWindow* CreateIntSlider(wxWindow* parent,
                            const wxString& label,
                            int min_value,
                            int max_value,
                            SliderControl* out_control) {
    auto* panel = new wxPanel(parent);
    auto* row = new wxBoxSizer(wxVERTICAL);
    auto* header = new wxBoxSizer(wxHORIZONTAL);
    auto* label_ctrl = new wxStaticText(panel, wxID_ANY, label);
    auto* value_ctrl = new wxStaticText(panel, wxID_ANY, "0");
    auto* slider = new wxSlider(panel, wxID_ANY, min_value, min_value, max_value);
    header->Add(label_ctrl, 1, wxRIGHT, 8);
    header->Add(value_ctrl, 0);
    row->Add(header, 0, wxBOTTOM | wxEXPAND, 4);
    row->Add(slider, 0, wxEXPAND);
    panel->SetSizer(row);

    out_control->slider = slider;
    out_control->value = value_ctrl;
    out_control->min_value = min_value;
    out_control->max_value = max_value;
    out_control->scale = 1;
    out_control->decimals = 0;
    UpdateSliderLabel(*out_control);
    return panel;
  }

  void BindEvents() {
    add_files_button_->Bind(wxEVT_BUTTON, &HiracoMainFrame::OnAddFiles, this);
    remove_button_->Bind(wxEVT_BUTTON, &HiracoMainFrame::OnRemoveSelected, this);
    clear_button_->Bind(wxEVT_BUTTON, &HiracoMainFrame::OnClearQueue, this);
    convert_button_->Bind(wxEVT_BUTTON, &HiracoMainFrame::OnConvert, this);
    cancel_button_->Bind(wxEVT_BUTTON, &HiracoMainFrame::OnCancel, this);
    reset_button_->Bind(wxEVT_BUTTON, &HiracoMainFrame::OnResetDefaults, this);
    stage1_reset_button_->Bind(wxEVT_BUTTON, &HiracoMainFrame::OnResetStage1Defaults, this);
    stage2_reset_button_->Bind(wxEVT_BUTTON, &HiracoMainFrame::OnResetStage2Defaults, this);
    stage3_reset_button_->Bind(wxEVT_BUTTON, &HiracoMainFrame::OnResetStage3Defaults, this);
    output_dir_picker_->Bind(wxEVT_DIRPICKER_CHANGED, &HiracoMainFrame::OnOutputDirChanged, this);
    relative_subdir_ctrl_->Bind(wxEVT_TEXT, &HiracoMainFrame::OnRelativeSubdirChanged, this);
    compression_choice_->Bind(wxEVT_CHOICE, &HiracoMainFrame::OnCompressionChanged, this);
    zoom_choice_->Bind(wxEVT_CHOICE, &HiracoMainFrame::OnZoomChanged, this);
    queue_ctrl_->Bind(wxEVT_LIST_ITEM_SELECTED, &HiracoMainFrame::OnQueueSelectionChanged, this);
    queue_ctrl_->Bind(wxEVT_MOTION, &HiracoMainFrame::OnQueueMouseMove, this);
    queue_ctrl_->Bind(wxEVT_LEAVE_WINDOW, &HiracoMainFrame::OnQueueMouseLeave, this);
    Bind(wxEVT_MENU, &HiracoMainFrame::OnQuit, this, wxID_EXIT);
    Bind(wxEVT_CLOSE_WINDOW, &HiracoMainFrame::OnCloseWindow, this);
    shutdown_timer_.Bind(wxEVT_TIMER, &HiracoMainFrame::OnShutdownTimer, this);
    crop_preview_timer_.Bind(wxEVT_TIMER, &HiracoMainFrame::OnCropPreviewTimer, this);
    Bind(EVT_HIRACO_SELECTION_READY, &HiracoMainFrame::OnSelectionReady, this);
    Bind(EVT_HIRACO_CROP_READY, &HiracoMainFrame::OnCropReady, this);
    Bind(EVT_HIRACO_CONVERT_PROGRESS, &HiracoMainFrame::OnConvertProgress, this);
    Bind(EVT_HIRACO_CONVERT_DONE, &HiracoMainFrame::OnConvertDone, this);

    original_canvas_->SetCropChangedHandler([this](const CropRect& crop_rect) {
      current_crop_rect_ = crop_rect;
      ScheduleCropPreview(false);
    });

    BindSlider(stage1_sigma_, [this](double value) { stage_overrides_.stage1_psf_sigma = static_cast<float>(value); });
    BindSlider(stage1_nsr_, [this](double value) { stage_overrides_.stage1_nsr = static_cast<float>(value); });
    BindSlider(stage2_denoise_, [this](double value) { stage_overrides_.stage2_denoise = static_cast<float>(value); });
    BindSlider(stage2_gain0_, [this](double value) { stage_overrides_.stage2_gain0 = static_cast<float>(value); });
    BindSlider(stage2_gain1_, [this](double value) { stage_overrides_.stage2_gain1 = static_cast<float>(value); });
    BindSlider(stage2_gain2_, [this](double value) { stage_overrides_.stage2_gain2 = static_cast<float>(value); });
    BindSlider(stage2_gain3_, [this](double value) { stage_overrides_.stage2_gain3 = static_cast<float>(value); });
    BindSlider(stage3_radius_, [this](double value) { stage_overrides_.stage3_radius = static_cast<int>(std::round(value)); });
    BindSlider(stage3_gain_, [this](double value) { stage_overrides_.stage3_gain = static_cast<float>(value); });
  }

  void ApplyStageSettingsToSliders(const ResolvedStageSettings& settings) {
    updating_sliders_ = true;
    SetSliderValue(stage1_sigma_, settings.stage1_psf_sigma);
    SetSliderValue(stage1_nsr_, settings.stage1_nsr);
    SetSliderValue(stage2_denoise_, settings.stage2_denoise);
    SetSliderValue(stage2_gain0_, settings.stage2_gain0);
    SetSliderValue(stage2_gain1_, settings.stage2_gain1);
    SetSliderValue(stage2_gain2_, settings.stage2_gain2);
    SetSliderValue(stage2_gain3_, settings.stage2_gain3);
    SetSliderValue(stage3_radius_, settings.stage3_radius);
    SetSliderValue(stage3_gain_, settings.stage3_gain);
    updating_sliders_ = false;
  }

  void UpdateResolvedSliderValues() {
    if (selected_row_ >= 0 &&
        selected_row_ < static_cast<int>(queue_.size()) &&
        queue_[selected_row_].prepared.has_value()) {
      ApplyStageSettingsToSliders(GetResolvedStageSettings(*queue_[selected_row_].prepared, stage_overrides_));
      return;
    }

    ApplyStageSettingsToSliders(ResolvedStageSettings());
  }

  void RefreshCropPreviewIfPossible() {
    UpdateResolvedSliderValues();
    if (selected_row_ >= 0 &&
        selected_row_ < static_cast<int>(queue_.size()) &&
        queue_[selected_row_].prepared.has_value() &&
        current_crop_rect_.has_value()) {
      ScheduleCropPreview(false);
    }
  }

  void InvalidateSelectionRequests() {
    if (selection_cancel_) {
      selection_cancel_->store(true);
    }
    ++selection_request_id_;
  }

  void InvalidateCropRequests() {
    if (crop_cancel_) {
      crop_cancel_->store(true);
    }
    crop_preview_timer_.Stop();
    ++crop_request_id_;
    crop_render_queued_ = false;
    crop_render_debounced_ = false;
  }

  void ArmCropPreviewTimer() {
    if (close_requested_.load()) {
      return;
    }
    crop_preview_timer_.Stop();
    if (!crop_render_queued_) {
      return;
    }
    crop_preview_timer_.StartOnce(80);
  }

  void StartQueuedCropPreviewIfIdle() {
    if (!crop_render_queued_ || crop_worker_running_) {
      return;
    }
    if (crop_render_debounced_) {
      ArmCropPreviewTimer();
      return;
    }
    crop_preview_timer_.Stop();
    StartCropPreviewWorker();
  }

  static uint32_t ScaleCoordBetweenExtents(uint32_t value,
                                           uint32_t from_extent,
                                           uint32_t to_extent) {
    if (from_extent == 0 || to_extent == 0) {
      return 0;
    }
    return static_cast<uint32_t>(
        std::clamp<uint64_t>((static_cast<uint64_t>(value) * to_extent + from_extent / 2) / from_extent,
                             0,
                             to_extent));
  }

  void ResolveDisplayCropMapping(const PreparedSource& prepared,
                                 uint32_t preview_width,
                                 uint32_t preview_height,
                                 uint32_t* mapped_width,
                                 uint32_t* mapped_height,
                                 uint32_t* offset_x,
                                 uint32_t* offset_y) const {
    *mapped_width = prepared.image_width;
    *mapped_height = prepared.image_height;
    *offset_x = 0;
    *offset_y = 0;

    const bool matches_default_crop =
        preview_width == prepared.metadata.default_crop_width &&
        preview_height == prepared.metadata.default_crop_height;
    const bool matches_half_default_crop =
        preview_width == prepared.metadata.default_crop_width / 2 &&
        preview_height == prepared.metadata.default_crop_height / 2;

    if (prepared.metadata.has_default_crop &&
        prepared.metadata.default_crop_width > 0 &&
        prepared.metadata.default_crop_height > 0 &&
        (matches_default_crop || matches_half_default_crop)) {
      *mapped_width = prepared.metadata.default_crop_width;
      *mapped_height = prepared.metadata.default_crop_height;
      *offset_x = prepared.metadata.default_crop_origin_h;
      *offset_y = prepared.metadata.default_crop_origin_v;
    }
  }

  CropRect MakeInitialDisplayCropRect(const PreparedSource& prepared,
                                      uint32_t preview_width,
                                      uint32_t preview_height) const {
    if (preview_width == 0 || preview_height == 0) {
      return CropRect();
    }

    uint32_t mapped_width = prepared.image_width;
    uint32_t mapped_height = prepared.image_height;
    uint32_t offset_x = 0;
    uint32_t offset_y = 0;
    ResolveDisplayCropMapping(prepared,
                              preview_width,
                              preview_height,
                              &mapped_width,
                              &mapped_height,
                              &offset_x,
                              &offset_y);
    (void) offset_x;
    (void) offset_y;

    const uint32_t display_crop_width =
        std::max(1u, ScaleCoordBetweenExtents(512, mapped_width, preview_width));
    const uint32_t display_crop_height =
        std::max(1u, ScaleCoordBetweenExtents(512, mapped_height, preview_height));
    return CenterCropRect(preview_width, preview_height, display_crop_width, display_crop_height);
  }

  static double NormalizeSelectionProgress(const ProcessingProgress& progress) {
    const double fraction = std::clamp(progress.fraction, 0.0, 1.0);
    if (progress.phase == "prepare") {
      return 0.35 * fraction;
    }
    if (progress.phase == "preview") {
      return 0.35 + 0.65 * fraction;
    }
    return fraction;
  }

  static double NormalizeCropProgress(const ProcessingProgress& progress) {
    const double fraction = std::clamp(progress.fraction, 0.0, 1.0);
    if (progress.phase == "cache") {
      return 0.60 * fraction;
    }
    if (progress.phase == "enhance") {
      return 0.60 + 0.30 * fraction;
    }
    if (progress.phase == "crop") {
      return 0.90 + 0.10 * fraction;
    }
    return fraction;
  }

  static double NormalizeConvertProgress(const ProcessingProgress& progress) {
    const double fraction = std::clamp(progress.fraction, 0.0, 1.0);
    if (progress.phase == "convert") {
      return fraction;
    }
    if (progress.phase == "enhance") {
      return 0.10 + 0.80 * fraction;
    }
    if (progress.phase == "write") {
      return 0.92 + 0.07 * fraction;
    }
    return fraction;
  }

  CropRect MapDisplayCropToProcessingCrop(const CropRect& display_crop,
                                          const PreparedSource& prepared) const {
    if (display_preview_width_ == 0 || display_preview_height_ == 0) {
      return ClampCropRect(display_crop, prepared.image_width, prepared.image_height);
    }

    uint32_t mapped_width = prepared.image_width;
    uint32_t mapped_height = prepared.image_height;
    uint32_t offset_x = 0;
    uint32_t offset_y = 0;
    ResolveDisplayCropMapping(prepared,
                              display_preview_width_,
                              display_preview_height_,
                              &mapped_width,
                              &mapped_height,
                              &offset_x,
                              &offset_y);

    CropRect mapped;
    mapped.x = offset_x + ScaleCoordBetweenExtents(display_crop.x, display_preview_width_, mapped_width);
    mapped.y = offset_y + ScaleCoordBetweenExtents(display_crop.y, display_preview_height_, mapped_height);
    mapped.width = std::max(1u, ScaleCoordBetweenExtents(display_crop.width, display_preview_width_, mapped_width));
    mapped.height = std::max(1u, ScaleCoordBetweenExtents(display_crop.height, display_preview_height_, mapped_height));
    return ClampCropRect(mapped, prepared.image_width, prepared.image_height);
  }

  void BindSlider(SliderControl& control, std::function<void(double)> on_change) {
    auto apply_change = [this, &control, on_change](bool debounce) {
      const double value = SliderValue(control);
      UpdateSliderLabel(control);
      if (!updating_sliders_) {
        on_change(value);
        ScheduleCropPreview(debounce);
      }
    };

    control.slider->Bind(wxEVT_SLIDER, [apply_change](wxCommandEvent&) { apply_change(false); });
    control.slider->Bind(wxEVT_SCROLL_THUMBTRACK,
                         [apply_change](wxScrollEvent&) { apply_change(true); });
    control.slider->Bind(wxEVT_SCROLL_THUMBRELEASE,
                         [apply_change](wxScrollEvent&) { apply_change(false); });
    control.slider->Bind(wxEVT_SCROLL_CHANGED,
                         [apply_change](wxScrollEvent&) { apply_change(false); });
    control.slider->Bind(wxEVT_SCROLL_LINEUP,
                         [apply_change](wxScrollEvent&) { apply_change(false); });
    control.slider->Bind(wxEVT_SCROLL_LINEDOWN,
                         [apply_change](wxScrollEvent&) { apply_change(false); });
    control.slider->Bind(wxEVT_SCROLL_PAGEUP,
                         [apply_change](wxScrollEvent&) { apply_change(false); });
    control.slider->Bind(wxEVT_SCROLL_PAGEDOWN,
                         [apply_change](wxScrollEvent&) { apply_change(false); });
    control.slider->Bind(wxEVT_SCROLL_TOP,
                         [apply_change](wxScrollEvent&) { apply_change(false); });
    control.slider->Bind(wxEVT_SCROLL_BOTTOM,
                         [apply_change](wxScrollEvent&) { apply_change(false); });
  }

  double SliderValue(const SliderControl& control) const {
    return static_cast<double>(control.slider->GetValue()) / control.scale;
  }

  void SetSliderValue(SliderControl& control, double value) {
    const int raw_value = static_cast<int>(std::round(value * control.scale));
    control.slider->SetValue(raw_value);
    UpdateSliderLabel(control);
  }

  void UpdateSliderLabel(SliderControl& control) {
    control.value->SetLabel(wxString::Format("%.*f", control.decimals, SliderValue(control)));
  }

  void UpdateCompressionChoice() {
    switch (compression_) {
      case HiracoCompression::kUncompressed:
        compression_choice_->SetSelection(0);
        break;
      case HiracoCompression::kDeflate:
        compression_choice_->SetSelection(1);
        break;
      case HiracoCompression::kJpegXl:
        compression_choice_->SetSelection(2);
        break;
    }
  }

  void UpdateButtons() {
    if (close_requested_.load()) {
      add_files_button_->Enable(false);
      remove_button_->Enable(false);
      clear_button_->Enable(false);
      convert_button_->Enable(false);
      cancel_button_->Enable(false);
      return;
    }

    const bool has_selection = selected_row_ >= 0 && selected_row_ < static_cast<int>(queue_.size());
    remove_button_->Enable(has_selection && !conversion_running_);
    clear_button_->Enable(!queue_.empty() && !conversion_running_);
    add_files_button_->Enable(!conversion_running_);
    convert_button_->Enable(!queue_.empty() && !conversion_running_);
    cancel_button_->Enable(conversion_running_);
  }

  void UpdateQueueTooltip(const wxString& tooltip) {
    if (queue_tooltip_text_ == tooltip) {
      return;
    }
    queue_tooltip_text_ = tooltip;
    if (queue_tooltip_text_.empty()) {
      queue_ctrl_->UnsetToolTip();
    } else {
      queue_ctrl_->SetToolTip(queue_tooltip_text_);
    }
  }

  void OnQueueMouseMove(wxMouseEvent& event) {
    int flags = 0;
    const wxPoint point = event.GetPosition();
    const long row = queue_ctrl_->HitTest(point, flags);
    if (row == wxNOT_FOUND || row < 0 || row >= static_cast<long>(queue_.size())) {
      UpdateQueueTooltip(wxString());
      event.Skip();
      return;
    }

    const QueueItem& item = queue_[static_cast<size_t>(row)];
    const int file_width = queue_ctrl_->GetColumnWidth(0);
    const int target_width = queue_ctrl_->GetColumnWidth(1);
    wxString tooltip;
    if (point.x < file_width) {
      tooltip = item.source_path;
    } else if (point.x < file_width + target_width) {
      tooltip = item.target_path.string();
    } else if (item.state == "Done") {
      tooltip = "Converted";
    } else if (item.state == "Skipped") {
      tooltip = "Skipped";
    } else if (item.state == "Loading" || item.state == "Converting") {
      tooltip = item.message.empty() ? wxString("In progress") : wxString(item.message);
    } else if (item.state == "Failed" || item.state == "Canceled") {
      tooltip = item.message.empty() ? item.state : wxString(item.message);
    } else {
      tooltip = "Not converted yet";
    }

    UpdateQueueTooltip(tooltip);
    event.Skip();
  }

  void OnQueueMouseLeave(wxMouseEvent& event) {
    UpdateQueueTooltip(wxString());
    event.Skip();
  }

  void RefreshQueue() {
    queue_ctrl_->Freeze();
    queue_ctrl_->DeleteAllItems();
    for (size_t index = 0; index < queue_.size(); ++index) {
      const QueueItem& item = queue_[index];
      const long row = queue_ctrl_->InsertItem(index, std::filesystem::path(item.source_path).filename().string());
      queue_ctrl_->SetItem(row, 1, item.target_path.string());
      queue_ctrl_->SetItem(row, 2, ProcessedMarkerForItem(item));
    }
    queue_ctrl_->Thaw();
    UpdateButtons();
  }

  int FindItemIndex(uint64_t id) const {
    for (size_t index = 0; index < queue_.size(); ++index) {
      if (queue_[index].id == id) {
        return static_cast<int>(index);
      }
    }
    return -1;
  }

  void RefreshQueueRow(int index) {
    if (index < 0 || index >= static_cast<int>(queue_.size())) {
      return;
    }
    queue_ctrl_->SetItem(index, 1, queue_[index].target_path.string());
    queue_ctrl_->SetItem(index, 2, ProcessedMarkerForItem(queue_[index]));
  }

  void SelectRow(int row) {
    if (row < 0 || row >= static_cast<int>(queue_.size())) {
      selected_row_ = -1;
      return;
    }
    selected_row_ = row;
    queue_ctrl_->SetItemState(row,
                              wxLIST_STATE_SELECTED | wxLIST_STATE_FOCUSED,
                              wxLIST_STATE_SELECTED | wxLIST_STATE_FOCUSED);
    StartSelectionLoad();
  }

  void StartSelectionLoad() {
    if (close_requested_.load() ||
        selected_row_ < 0 || selected_row_ >= static_cast<int>(queue_.size())) {
      return;
    }

    const QueueItem item_copy = queue_[selected_row_];
    const uint64_t item_id = item_copy.id;
    ++selection_request_id_;
    if (selection_cancel_) {
      selection_cancel_->store(true);
    }
    InvalidateCropRequests();
    current_crop_rect_.reset();
    display_preview_width_ = 0;
    display_preview_height_ = 0;

    if (item_copy.prepared.has_value()) {
      auto cached_preview = std::make_shared<PreviewImage>();
      if (TryGetCachedOriginalPreview(*item_copy.prepared, {}, cached_preview)) {
        current_crop_rect_ = MakeInitialDisplayCropRect(*item_copy.prepared,
                                                        cached_preview->width,
                                                        cached_preview->height);
        display_preview_width_ = cached_preview->width;
        display_preview_height_ = cached_preview->height;
        original_canvas_->SetPreview(cached_preview);
        original_canvas_->SetCropRect(*current_crop_rect_);
        crop_preview_panel_->SetPreview(CropPreviewImage(cached_preview, *current_crop_rect_));
        queue_[selected_row_].state = "Ready";
        queue_[selected_row_].message = "Original preview ready";
        RefreshQueueRow(selected_row_);
        progress_gauge_->SetValue(0);
        status_label_->SetLabel("Original preview ready. Rendering converted crop...");
        UpdateResolvedSliderValues();
        ScheduleCropPreview(false);
        return;
      }
    }

    queue_[selected_row_].state = "Loading";
    queue_[selected_row_].message = "Rendering original preview";
    RefreshQueueRow(selected_row_);

    const uint64_t request_id = selection_request_id_;
    selection_cancel_ = std::make_shared<std::atomic_bool>(false);
    original_canvas_->SetPreview(nullptr);
    crop_preview_panel_->SetPreview(nullptr);
    progress_gauge_->SetValue(0);
    status_label_->SetLabel("Loading original preview...");

    LaunchWorker([this, item_copy, item_id, request_id, cancel_token = selection_cancel_]() {
      SelectionReadyPayload payload;
      payload.item_id = item_id;
      payload.request_id = request_id;

      auto report_progress = [this, item_id, request_id](const ProcessingProgress& progress) {
        const double normalized = NormalizeSelectionProgress(progress);
        const std::string message = progress.message;
        CallAfter([this, item_id, request_id, normalized, message]() {
          if (close_requested_.load() || conversion_running_ || request_id != selection_request_id_) {
            return;
          }
          progress_gauge_->SetValue(static_cast<int>(std::round(std::clamp(normalized, 0.0, 1.0) * 100.0)));
          status_label_->SetLabel(message);
          const int index = FindItemIndex(item_id);
          if (index >= 0 && queue_[index].state == "Loading") {
            queue_[index].message = message;
            RefreshQueueRow(index);
          }
        });
      };

      PreparedSource prepared;
      if (item_copy.prepared.has_value()) {
        prepared = *item_copy.prepared;
      } else {
        std::string prepare_error;
        if (!PrepareSource(item_copy.source_path,
                           &prepared,
                           &prepare_error,
                           report_progress,
                           [cancel_token]() { return cancel_token->load(); })) {
          payload.error = prepare_error;
          auto* event = new wxThreadEvent(EVT_HIRACO_SELECTION_READY);
          event->SetPayload(payload);
          wxQueueEvent(this, event);
          return;
        }
      }

      payload.prepared = prepared;

      auto original = std::make_shared<PreviewImage>();
      std::string preview_error;
      if (!RenderOriginalPreview(&prepared,
                                 original,
                                 {},
                                 report_progress,
                                 [cancel_token]() { return cancel_token->load(); },
                                 &preview_error)) {
        payload.error = preview_error;
        auto* event = new wxThreadEvent(EVT_HIRACO_SELECTION_READY);
        event->SetPayload(payload);
        wxQueueEvent(this, event);
        return;
      }

      payload.ok = true;
      payload.original_preview = original;
  payload.crop_rect = MakeInitialDisplayCropRect(prepared, original->width, original->height);
      auto* event = new wxThreadEvent(EVT_HIRACO_SELECTION_READY);
      event->SetPayload(payload);
      wxQueueEvent(this, event);
    });
  }

  void ScheduleCropPreview(bool debounce) {
    if (close_requested_.load() ||
        selected_row_ < 0 || selected_row_ >= static_cast<int>(queue_.size())) {
      return;
    }
    ++crop_request_id_;
    crop_render_queued_ = true;
    crop_render_debounced_ = debounce;
    if (crop_worker_running_ && crop_cancel_) {
      crop_cancel_->store(true);
    }
    StartQueuedCropPreviewIfIdle();
  }

  void StartCropPreviewWorker() {
    if (close_requested_.load() ||
        selected_row_ < 0 || selected_row_ >= static_cast<int>(queue_.size())) {
      return;
    }
    QueueItem& item = queue_[selected_row_];
    if (!item.prepared.has_value() || !current_crop_rect_.has_value()) {
      return;
    }

    crop_render_queued_ = false;
    crop_worker_running_ = true;
    const uint64_t request_id = crop_request_id_;
    active_crop_request_id_ = request_id;
    if (crop_cancel_) {
      crop_cancel_->store(true);
    }
    crop_cancel_ = std::make_shared<std::atomic_bool>(false);
    const uint64_t item_id = item.id;
    PreparedSource prepared = *item.prepared;
    const CropRect display_crop_rect = *current_crop_rect_;
    const CropRect crop_rect = MapDisplayCropToProcessingCrop(display_crop_rect, prepared);
    const StageOverrideSet stage_overrides = stage_overrides_;
    item.message = "Rendering crop preview";
    RefreshQueueRow(selected_row_);
    progress_gauge_->SetValue(0);
    status_label_->SetLabel("Rendering converted crop...");

    LaunchWorker([this, item_id, request_id, display_crop_rect, prepared, crop_rect, stage_overrides, cancel_token = crop_cancel_]() mutable {
      CropReadyPayload payload;
      payload.item_id = item_id;
      payload.request_id = request_id;
      payload.crop_rect = display_crop_rect;
      payload.crop_preview = std::make_shared<PreviewImage>();

      auto report_progress = [this, item_id, request_id](const ProcessingProgress& progress) {
        const double normalized = NormalizeCropProgress(progress);
        const std::string message = progress.message;
        CallAfter([this, item_id, request_id, normalized, message]() {
          if (close_requested_.load() || conversion_running_ || request_id != crop_request_id_) {
            return;
          }
          progress_gauge_->SetValue(static_cast<int>(std::round(std::clamp(normalized, 0.0, 1.0) * 100.0)));
          status_label_->SetLabel(message);
          const int index = FindItemIndex(item_id);
          if (index >= 0 && queue_[index].state == "Ready") {
            queue_[index].message = message;
            RefreshQueueRow(index);
          }
        });
      };

      std::string error;
      if (!RenderConvertedCrop(&prepared,
                               crop_rect,
                               stage_overrides,
                               payload.crop_preview,
                               {},
                               report_progress,
                               [cancel_token]() { return cancel_token->load(); },
                               &error)) {
        payload.ok = false;
        payload.error = error;
      } else {
        payload.ok = true;
      }

      auto* event = new wxThreadEvent(EVT_HIRACO_CROP_READY);
      event->SetPayload(payload);
      wxQueueEvent(this, event);
    });
  }

  OverwriteResponse PromptOverwriteOnUi(const std::filesystem::path& path) {
    if (close_requested_.load()) {
      return OverwriteResponse::kCancel;
    }

    auto promise = std::make_shared<std::promise<OverwriteResponse>>();
    std::future<OverwriteResponse> future = promise->get_future();
    CallAfter([this, path, promise]() {
      if (close_requested_.load()) {
        promise->set_value(OverwriteResponse::kCancel);
        return;
      }
      OverwriteDialog dialog(this, path);
      promise->set_value(dialog.ShowModalAndGetResponse());
    });
    return future.get();
  }

  void RequestBackgroundCancel() {
    conversion_cancel_.store(true);
    if (selection_cancel_) {
      selection_cancel_->store(true);
    }
    if (crop_cancel_) {
      crop_cancel_->store(true);
    }
  }

  void BeginCloseRequest() {
    if (close_requested_.exchange(true)) {
      return;
    }

    RequestBackgroundCancel();
    shutdown_timer_.Start(100);
    status_label_->SetLabel("Closing...");
    Disable();
    Hide();
    UpdateButtons();
  }

  void MaybeFinishClose() {
    if (!close_requested_.load()) {
      return;
    }
    if (active_workers_.load() != 0 || conversion_running_) {
      return;
    }

    shutdown_timer_.Stop();
    Destroy();
  }

  void OnAddFiles(wxCommandEvent&) {
    wxFileDialog dialog(this,
                        "Add source files",
                        wxEmptyString,
                        wxEmptyString,
                        "Raw files (*.orf;*.ORF;*.ori;*.ORI)|*.orf;*.ORF;*.ori;*.ORI|All files (*.*)|*.*",
                        wxFD_OPEN | wxFD_FILE_MUST_EXIST | wxFD_MULTIPLE);
    if (dialog.ShowModal() != wxID_OK) {
      return;
    }

    wxArrayString selected_files;
    dialog.GetPaths(selected_files);
    std::vector<std::string> paths;
    for (const wxString& file : selected_files) {
      paths.push_back(file.ToStdString());
    }
    AddFiles(paths);
  }

  void OnRemoveSelected(wxCommandEvent&) {
    InvalidateCropRequests();
    std::vector<long> selected_rows;
    long row = -1;
    while ((row = queue_ctrl_->GetNextItem(row, wxLIST_NEXT_ALL, wxLIST_STATE_SELECTED)) != -1) {
      selected_rows.push_back(row);
    }
    if (selected_rows.empty()) {
      return;
    }

    std::sort(selected_rows.rbegin(), selected_rows.rend());
    for (long selected : selected_rows) {
      queue_.erase(queue_.begin() + selected);
    }
    selected_row_ = -1;
    current_crop_rect_.reset();
    RefreshQueue();
    if (!queue_.empty()) {
      SelectRow(0);
    } else {
      display_preview_width_ = 0;
      display_preview_height_ = 0;
      original_canvas_->SetPreview(nullptr);
      crop_preview_panel_->SetPreview(nullptr);
      UpdateResolvedSliderValues();
      status_label_->SetLabel("Ready");
    }
  }

  void OnClearQueue(wxCommandEvent&) {
    InvalidateCropRequests();
    queue_.clear();
    selected_row_ = -1;
    current_crop_rect_.reset();
    display_preview_width_ = 0;
    display_preview_height_ = 0;
    original_canvas_->SetPreview(nullptr);
    crop_preview_panel_->SetPreview(nullptr);
    UpdateResolvedSliderValues();
    status_label_->SetLabel("Ready");
    RefreshQueue();
  }

  void OnQueueSelectionChanged(wxListEvent& event) {
    selected_row_ = static_cast<int>(event.GetIndex());
    StartSelectionLoad();
  }

  void OnOutputDirChanged(wxFileDirPickerEvent&) {
    base_output_dir_ = output_dir_picker_->GetPath().ToStdString();
    RebuildTargetPaths();
  }

  void OnRelativeSubdirChanged(wxCommandEvent&) {
    relative_subdir_ = relative_subdir_ctrl_->GetValue().ToStdString();
    RebuildTargetPaths();
  }

  void RebuildTargetPaths() {
    for (QueueItem& item : queue_) {
      item.target_path = ResolveOutputPath(item.source_path, base_output_dir_, relative_subdir_);
    }
    RefreshQueue();
  }

  void OnCompressionChanged(wxCommandEvent&) {
    switch (compression_choice_->GetSelection()) {
      case 0:
        compression_ = HiracoCompression::kUncompressed;
        break;
      case 1:
        compression_ = HiracoCompression::kDeflate;
        break;
      case 2:
        compression_ = HiracoCompression::kJpegXl;
        break;
      default:
        compression_ = HiracoCompression::kUncompressed;
        break;
    }
  }

  void OnZoomChanged(wxCommandEvent&) {
    switch (zoom_choice_->GetSelection()) {
      case 1:
        original_canvas_->SetZoomMode(PreviewCanvas::ZoomMode::k25);
        break;
      case 2:
        original_canvas_->SetZoomMode(PreviewCanvas::ZoomMode::k50);
        break;
      case 3:
        original_canvas_->SetZoomMode(PreviewCanvas::ZoomMode::k100);
        break;
      case 0:
      default:
        original_canvas_->SetZoomMode(PreviewCanvas::ZoomMode::kFit);
        break;
    }
  }

  void OnResetDefaults(wxCommandEvent&) {
    stage_overrides_ = StageOverrideSet();
    RefreshCropPreviewIfPossible();
  }

  void OnResetStage1Defaults(wxCommandEvent&) {
    stage_overrides_.stage1_psf_sigma.reset();
    stage_overrides_.stage1_nsr.reset();
    RefreshCropPreviewIfPossible();
  }

  void OnResetStage2Defaults(wxCommandEvent&) {
    stage_overrides_.stage2_denoise.reset();
    stage_overrides_.stage2_gain0.reset();
    stage_overrides_.stage2_gain1.reset();
    stage_overrides_.stage2_gain2.reset();
    stage_overrides_.stage2_gain3.reset();
    RefreshCropPreviewIfPossible();
  }

  void OnResetStage3Defaults(wxCommandEvent&) {
    stage_overrides_.stage3_radius.reset();
    stage_overrides_.stage3_gain.reset();
    RefreshCropPreviewIfPossible();
  }

  void OnSelectionReady(wxThreadEvent& event) {
    if (close_requested_.load()) {
      return;
    }

    if (conversion_running_) {
      return;
    }

    const SelectionReadyPayload payload = event.GetPayload<SelectionReadyPayload>();
    if (payload.request_id != selection_request_id_) {
      return;
    }

    const int index = FindItemIndex(payload.item_id);
    if (index < 0) {
      return;
    }

    if (!payload.ok) {
      queue_[index].state = "Failed";
      queue_[index].message = payload.error;
      RefreshQueueRow(index);
      status_label_->SetLabel(payload.error);
      return;
    }

    queue_[index].prepared = payload.prepared;
    queue_[index].state = "Ready";
    queue_[index].message = "Original preview ready";
    RefreshQueueRow(index);

    if (index >= 0 && index == selected_row_) {
      current_crop_rect_ = payload.crop_rect;
      display_preview_width_ = payload.original_preview->width;
      display_preview_height_ = payload.original_preview->height;
      original_canvas_->SetPreview(payload.original_preview);
      original_canvas_->SetCropRect(*current_crop_rect_);
      crop_preview_panel_->SetPreview(CropPreviewImage(payload.original_preview, *current_crop_rect_));
      status_label_->SetLabel("Original preview ready. Rendering converted crop...");
      UpdateResolvedSliderValues();
      ScheduleCropPreview(false);
    }
  }

  void OnCropReady(wxThreadEvent& event) {
    const CropReadyPayload payload = event.GetPayload<CropReadyPayload>();
    if (payload.request_id == active_crop_request_id_) {
      crop_worker_running_ = false;
      active_crop_request_id_ = 0;
    }

    if (close_requested_.load()) {
      return;
    }

    if (conversion_running_) {
      return;
    }

    if (payload.request_id != crop_request_id_) {
      StartQueuedCropPreviewIfIdle();
      return;
    }

    if (!payload.ok) {
      const int index = FindItemIndex(payload.item_id);
      if (index >= 0) {
        queue_[index].message = payload.error;
        RefreshQueueRow(index);
      }
      status_label_->SetLabel(payload.error);
      StartQueuedCropPreviewIfIdle();
      return;
    }

    const int index = FindItemIndex(payload.item_id);
    if (index >= 0 && index == selected_row_) {
      crop_preview_panel_->SetPreview(payload.crop_preview);
      status_label_->SetLabel("Converted crop updated");
    }
    if (index >= 0 && queue_[index].state == "Ready") {
      queue_[index].message = "Preview ready";
      RefreshQueueRow(index);
    }
    StartQueuedCropPreviewIfIdle();
  }

  void OnCropPreviewTimer(wxTimerEvent&) {
    if (close_requested_.load() || conversion_running_ || crop_worker_running_ || !crop_render_queued_) {
      return;
    }
    StartCropPreviewWorker();
  }

  void OnConvert(wxCommandEvent&) {
    if (close_requested_.load() || conversion_running_ || queue_.empty()) {
      return;
    }

    InvalidateSelectionRequests();
    InvalidateCropRequests();
    conversion_running_ = true;
    conversion_cancel_.store(false);
    overwrite_policy_ = OverwritePolicy::kAsk;
    progress_gauge_->SetValue(0);
    status_label_->SetLabel("Converting...");
    UpdateButtons();

    const std::vector<QueueItem> queue_snapshot = queue_;

    LaunchWorker([this, queue_snapshot]() {
      for (size_t item_index = 0; item_index < queue_snapshot.size(); ++item_index) {
        if (conversion_cancel_.load()) {
          break;
        }

        const QueueItem& item = queue_snapshot[item_index];
        auto* progress_event = new wxThreadEvent(EVT_HIRACO_CONVERT_PROGRESS);
        progress_event->SetPayload(ConvertProgressPayload{item.id,
                                                          static_cast<double>(item_index) / std::max<size_t>(1, queue_snapshot.size()),
                                                          "Preparing source"});
        wxQueueEvent(this, progress_event);

        PreparedSource prepared;
        if (item.prepared.has_value()) {
          prepared = *item.prepared;
        } else {
          std::string error;
          if (!PrepareSource(item.source_path,
                             &prepared,
                             &error,
                             {},
                             [this]() { return conversion_cancel_.load(); })) {
            auto* done_event = new wxThreadEvent(EVT_HIRACO_CONVERT_DONE);
            done_event->SetPayload(ConvertDonePayload{item.id, false, false, false, error});
            wxQueueEvent(this, done_event);
            continue;
          }
        }

        const bool target_exists = std::filesystem::exists(item.target_path);
        if (target_exists) {
          OverwriteResponse response = OverwriteResponse::kCancel;
          if (overwrite_policy_ == OverwritePolicy::kAsk) {
            response = PromptOverwriteOnUi(item.target_path);
          }
          OverwriteDecision decision = ResolveOverwriteDecision(overwrite_policy_, target_exists, response);
          overwrite_policy_ = decision.next_policy;
          if (decision.canceled) {
            conversion_cancel_.store(true);
            auto* done_event = new wxThreadEvent(EVT_HIRACO_CONVERT_DONE);
            done_event->SetPayload(ConvertDonePayload{item.id, false, false, true, "Conversion canceled"});
            wxQueueEvent(this, done_event);
            break;
          }
          if (!decision.should_write) {
            auto* done_event = new wxThreadEvent(EVT_HIRACO_CONVERT_DONE);
            done_event->SetPayload(ConvertDonePayload{item.id, true, true, false, "Skipped existing target"});
            wxQueueEvent(this, done_event);
            continue;
          }
        }

        DngWriteResult result = ConvertToDng(prepared,
                                             item.target_path,
                                             compression_,
                                             stage_overrides_,
                                             {},
                                             [this, item_index, total = queue_snapshot.size(), item_id = item.id](const ProcessingProgress& progress) {
                                               auto* event = new wxThreadEvent(EVT_HIRACO_CONVERT_PROGRESS);
                                               const double overall =
                                                   (static_cast<double>(item_index) + NormalizeConvertProgress(progress)) /
                                                   std::max<size_t>(1, total);
                                               event->SetPayload(ConvertProgressPayload{item_id, overall, progress.message});
                                               wxQueueEvent(this, event);
                                             },
                                             [this]() { return conversion_cancel_.load(); });

        auto* done_event = new wxThreadEvent(EVT_HIRACO_CONVERT_DONE);
        done_event->SetPayload(ConvertDonePayload{item.id,
                                                  result.ok,
                                                  false,
                                                  !result.ok && result.message == "operation canceled",
                                                  result.message});
        wxQueueEvent(this, done_event);
      }

      CallAfter([this]() {
        conversion_running_ = false;
        UpdateButtons();
        if (conversion_cancel_.load()) {
          status_label_->SetLabel("Conversion canceled");
        } else {
          status_label_->SetLabel("Batch conversion finished");
          progress_gauge_->SetValue(100);
        }
        MaybeFinishClose();
      });
    });
  }

  void OnCancel(wxCommandEvent&) {
    RequestBackgroundCancel();
    status_label_->SetLabel("Cancel requested...");
  }

  void OnQuit(wxCommandEvent&) {
    Close(true);
  }

  void OnCloseWindow(wxCloseEvent& event) {
    BeginCloseRequest();
    if (active_workers_.load() != 0 || conversion_running_) {
      if (event.CanVeto()) {
        event.Veto();
      }
      return;
    }
    Destroy();
  }

  void OnShutdownTimer(wxTimerEvent&) {
    MaybeFinishClose();
  }

  void OnConvertProgress(wxThreadEvent& event) {
    if (close_requested_.load()) {
      return;
    }

    const ConvertProgressPayload payload = event.GetPayload<ConvertProgressPayload>();
    progress_gauge_->SetValue(static_cast<int>(std::round(payload.overall_fraction * 100.0)));
    status_label_->SetLabel(payload.message);

    const int index = FindItemIndex(payload.item_id);
    if (index >= 0) {
      queue_[index].state = "Converting";
      queue_[index].message = payload.message;
      RefreshQueueRow(index);
    }
  }

  void OnConvertDone(wxThreadEvent& event) {
    if (close_requested_.load()) {
      return;
    }

    const ConvertDonePayload payload = event.GetPayload<ConvertDonePayload>();
    const int index = FindItemIndex(payload.item_id);
    if (index < 0) {
      return;
    }

    if (payload.canceled) {
      queue_[index].state = "Canceled";
    } else if (payload.skipped) {
      queue_[index].state = "Skipped";
    } else if (payload.ok) {
      queue_[index].state = "Done";
    } else {
      queue_[index].state = "Failed";
    }
    queue_[index].message = payload.message;
    RefreshQueueRow(index);
  }

  wxButton* add_files_button_ = nullptr;
  wxButton* remove_button_ = nullptr;
  wxButton* clear_button_ = nullptr;
  wxButton* reset_button_ = nullptr;
  wxButton* stage1_reset_button_ = nullptr;
  wxButton* stage2_reset_button_ = nullptr;
  wxButton* stage3_reset_button_ = nullptr;
  wxButton* convert_button_ = nullptr;
  wxButton* cancel_button_ = nullptr;
  wxListCtrl* queue_ctrl_ = nullptr;
  PreviewCanvas* original_canvas_ = nullptr;
  FitImagePanel* crop_preview_panel_ = nullptr;
  wxChoice* zoom_choice_ = nullptr;
  wxChoice* compression_choice_ = nullptr;
  wxDirPickerCtrl* output_dir_picker_ = nullptr;
  wxTextCtrl* relative_subdir_ctrl_ = nullptr;
  wxGauge* progress_gauge_ = nullptr;
  wxStaticText* status_label_ = nullptr;
  wxTimer shutdown_timer_;
  wxTimer crop_preview_timer_;
  wxString queue_tooltip_text_;

  SliderControl stage1_sigma_;
  SliderControl stage1_nsr_;
  SliderControl stage2_denoise_;
  SliderControl stage2_gain0_;
  SliderControl stage2_gain1_;
  SliderControl stage2_gain2_;
  SliderControl stage2_gain3_;
  SliderControl stage3_radius_;
  SliderControl stage3_gain_;

  std::vector<QueueItem> queue_;
  std::filesystem::path base_output_dir_;
  std::filesystem::path relative_subdir_;
  HiracoCompression compression_ = HiracoCompression::kUncompressed;
  StageOverrideSet stage_overrides_;
  std::optional<CropRect> current_crop_rect_;
  uint32_t display_preview_width_ = 0;
  uint32_t display_preview_height_ = 0;
  int selected_row_ = -1;
  uint64_t next_item_id_ = 1;
  uint64_t selection_request_id_ = 0;
  uint64_t crop_request_id_ = 0;
  uint64_t active_crop_request_id_ = 0;
  bool crop_worker_running_ = false;
  bool crop_render_queued_ = false;
  bool crop_render_debounced_ = false;
  bool updating_sliders_ = false;
  bool conversion_running_ = false;
  std::atomic_int active_workers_ = 0;
  std::atomic_bool close_requested_ = false;
  OverwritePolicy overwrite_policy_ = OverwritePolicy::kAsk;
  std::shared_ptr<std::atomic_bool> selection_cancel_;
  std::shared_ptr<std::atomic_bool> crop_cancel_;
  std::atomic_bool conversion_cancel_ = false;
};

bool HiracoDropTarget::OnDropFiles(wxCoord, wxCoord, const wxArrayString& filenames) {
  std::vector<std::string> paths;
  paths.reserve(filenames.size());
  for (const wxString& filename : filenames) {
    paths.push_back(filename.ToStdString());
  }
  frame_->AddFiles(paths);
  return true;
}

class HiracoGuiApp final : public wxApp {
 public:
  bool OnInit() override {
    SetExitOnFrameDelete(true);
    auto* frame = new HiracoMainFrame();
    SetTopWindow(frame);
    frame->Show();
    return true;
  }
};

}  // namespace

wxIMPLEMENT_APP(HiracoGuiApp);
