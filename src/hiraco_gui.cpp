#include "hiraco_core.h"

#include <wx/bitmap.h>
#include <wx/bmpbuttn.h>
#include <wx/button.h>
#include <wx/collpane.h>
#include <wx/config.h>
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
#include <wx/splitter.h>
#include <wx/timer.h>
#include <wx/wx.h>

#include <algorithm>
#include <atomic>
#include <cmath>
#include <cstring>
#include <filesystem>
#include <future>
#include <memory>
#include <optional>
#include <thread>
#include <vector>

namespace {

wxDECLARE_EVENT(EVT_HIRACO_SELECTION_READY, wxThreadEvent);
wxDECLARE_EVENT(EVT_HIRACO_METADATA_READY, wxThreadEvent);
wxDECLARE_EVENT(EVT_HIRACO_CROP_READY, wxThreadEvent);
wxDECLARE_EVENT(EVT_HIRACO_CONVERT_PROGRESS, wxThreadEvent);
wxDECLARE_EVENT(EVT_HIRACO_CONVERT_DONE, wxThreadEvent);

wxDEFINE_EVENT(EVT_HIRACO_SELECTION_READY, wxThreadEvent);
wxDEFINE_EVENT(EVT_HIRACO_METADATA_READY, wxThreadEvent);
wxDEFINE_EVENT(EVT_HIRACO_CROP_READY, wxThreadEvent);
wxDEFINE_EVENT(EVT_HIRACO_CONVERT_PROGRESS, wxThreadEvent);
wxDEFINE_EVENT(EVT_HIRACO_CONVERT_DONE, wxThreadEvent);

struct QueueItem {
  uint64_t id = 0;
  std::string source_path;
  std::filesystem::path target_path;
  std::optional<PreparedSource> prepared;
  StageOverrideSet stage_overrides;
  wxString resolution_label;
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

struct MetadataReadyPayload {
  uint64_t item_id = 0;
  bool ok = false;
  PreparedSource prepared;
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

wxString SettingsMarkerForItem(const QueueItem& item) {
  return item.stage_overrides.HasAnyOverrides() ? "Custom" : "Default";
}

bool Is50MpFrame(const PreparedSource& prepared) {
  const SourceLinearDngMetadata& metadata = prepared.metadata;
  return metadata.default_crop_origin_h == 6 &&
         metadata.default_crop_origin_v == 6 &&
         metadata.default_crop_width == 8160 &&
         metadata.default_crop_height == 6120 &&
         prepared.image_width == 8172 &&
         prepared.image_height == 6132;
}

bool Is80MpFrame(const PreparedSource& prepared) {
  const SourceLinearDngMetadata& metadata = prepared.metadata;
  return metadata.default_crop_origin_h == 8 &&
         metadata.default_crop_origin_v == 8 &&
         metadata.default_crop_width == 10368 &&
         metadata.default_crop_height == 7776 &&
         prepared.image_width == 10386 &&
         prepared.image_height == 7792;
}

bool Is20MpFrame(const PreparedSource& prepared) {
  const SourceLinearDngMetadata& metadata = prepared.metadata;
  return metadata.default_crop_origin_h == 12 &&
         metadata.default_crop_origin_v == 12 &&
         metadata.default_crop_width == 5184 &&
         metadata.default_crop_height == 3888 &&
         prepared.image_width == 5220 &&
         prepared.image_height == 3912;
}

wxString ResolutionLabelForPrepared(const PreparedSource& prepared) {
  if (Is50MpFrame(prepared)) {
    return "50 MP";
  }
  if (Is80MpFrame(prepared)) {
    return "80 MP";
  }
  if (Is20MpFrame(prepared)) {
    return "20 MP";
  }
  if (!prepared.IsValid()) {
    return wxString();
  }
  return wxString::Format("%ux%u", prepared.image_width, prepared.image_height);
}

wxString ResolutionTooltipForPrepared(const PreparedSource& prepared) {
  if (!prepared.IsValid()) {
    return wxString();
  }

  const wxString compact = ResolutionLabelForPrepared(prepared);
  const wxString exact = wxString::Format("%ux%u", prepared.image_width, prepared.image_height);
  if (compact.empty() || compact == exact) {
    return exact;
  }
  return wxString::Format("%s (%s)", compact, exact);
}

std::filesystem::path PathFromWxString(const wxString& value) {
#if defined(_WIN32)
  return std::filesystem::path(value.ToStdWstring());
#else
  return std::filesystem::path(value.ToStdString());
#endif
}

wxString WxStringFromPath(const std::filesystem::path& path) {
#if defined(_WIN32)
  return wxString(path.wstring());
#else
  return wxString::FromUTF8(path.string().c_str());
#endif
}

struct ContinuousRect {
  double left = 0.0;
  double top = 0.0;
  double right = 0.0;
  double bottom = 0.0;
};

ContinuousRect ToContinuousRect(const CropRect& rect) {
  ContinuousRect result;
  result.left = static_cast<double>(rect.x);
  result.top = static_cast<double>(rect.y);
  result.right = static_cast<double>(rect.x + rect.width);
  result.bottom = static_cast<double>(rect.y + rect.height);
  return result;
}

ContinuousRect TransformNativeRectToDisplay(const ContinuousRect& rect,
                                            uint32_t native_width,
                                            uint32_t native_height,
                                            int libraw_flip) {
  ContinuousRect mapped = rect;
  const int normalized_flip = NormalizeLibRawFlip(libraw_flip);
  if ((normalized_flip & 1) != 0) {
    const double flipped_left = static_cast<double>(native_width) - mapped.right;
    const double flipped_right = static_cast<double>(native_width) - mapped.left;
    mapped.left = flipped_left;
    mapped.right = flipped_right;
  }
  if ((normalized_flip & 2) != 0) {
    const double flipped_top = static_cast<double>(native_height) - mapped.bottom;
    const double flipped_bottom = static_cast<double>(native_height) - mapped.top;
    mapped.top = flipped_top;
    mapped.bottom = flipped_bottom;
  }
  if ((normalized_flip & 4) != 0) {
    std::swap(mapped.left, mapped.top);
    std::swap(mapped.right, mapped.bottom);
  }
  return mapped;
}

ContinuousRect TransformDisplayRectToNative(const ContinuousRect& rect,
                                            uint32_t native_width,
                                            uint32_t native_height,
                                            int libraw_flip) {
  ContinuousRect mapped = rect;
  const int normalized_flip = NormalizeLibRawFlip(libraw_flip);
  if ((normalized_flip & 4) != 0) {
    std::swap(mapped.left, mapped.top);
    std::swap(mapped.right, mapped.bottom);
  }
  if ((normalized_flip & 2) != 0) {
    const double flipped_top = static_cast<double>(native_height) - mapped.bottom;
    const double flipped_bottom = static_cast<double>(native_height) - mapped.top;
    mapped.top = flipped_top;
    mapped.bottom = flipped_bottom;
  }
  if ((normalized_flip & 1) != 0) {
    const double flipped_left = static_cast<double>(native_width) - mapped.right;
    const double flipped_right = static_cast<double>(native_width) - mapped.left;
    mapped.left = flipped_left;
    mapped.right = flipped_right;
  }
  return mapped;
}

CropRect PreviewCoverageRectInNative(const PreparedSource& prepared,
                                     uint32_t preview_width,
                                     uint32_t preview_height) {
  CropRect coverage;
  coverage.width = prepared.image_width;
  coverage.height = prepared.image_height;

  const SourceLinearDngMetadata& metadata = prepared.metadata;
  if (!metadata.has_default_crop ||
      metadata.default_crop_width == 0 ||
      metadata.default_crop_height == 0) {
    return coverage;
  }

  const uint32_t oriented_crop_width =
      OrientedImageWidth(metadata.default_crop_width, metadata.default_crop_height, metadata.libraw_flip);
  const uint32_t oriented_crop_height =
      OrientedImageHeight(metadata.default_crop_width, metadata.default_crop_height, metadata.libraw_flip);
  const bool matches_default_crop =
      preview_width == oriented_crop_width && preview_height == oriented_crop_height;
  const bool matches_half_default_crop =
      preview_width * 2 == oriented_crop_width && preview_height * 2 == oriented_crop_height;

  if (!matches_default_crop && !matches_half_default_crop) {
    return coverage;
  }

  coverage.x = metadata.default_crop_origin_h;
  coverage.y = metadata.default_crop_origin_v;
  coverage.width = metadata.default_crop_width;
  coverage.height = metadata.default_crop_height;
  return coverage;
}

CropRect ContinuousRectToCropRect(const ContinuousRect& rect,
                                  uint32_t native_width,
                                  uint32_t native_height) {
  CropRect result;
  if (native_width == 0 || native_height == 0) {
    return result;
  }

  const double max_width = static_cast<double>(native_width);
  const double max_height = static_cast<double>(native_height);
  const double clamped_left = std::clamp(rect.left, 0.0, max_width);
  const double clamped_top = std::clamp(rect.top, 0.0, max_height);
  const double clamped_right = std::clamp(rect.right, 0.0, max_width);
  const double clamped_bottom = std::clamp(rect.bottom, 0.0, max_height);

  const uint32_t x = static_cast<uint32_t>(std::clamp<long long>(
      static_cast<long long>(std::llround(clamped_left)), 0, static_cast<long long>(native_width - 1)));
  const uint32_t y = static_cast<uint32_t>(std::clamp<long long>(
      static_cast<long long>(std::llround(clamped_top)), 0, static_cast<long long>(native_height - 1)));
  const uint32_t right = static_cast<uint32_t>(std::clamp<long long>(
      static_cast<long long>(std::llround(clamped_right)), static_cast<long long>(x + 1), static_cast<long long>(native_width)));
  const uint32_t bottom = static_cast<uint32_t>(std::clamp<long long>(
      static_cast<long long>(std::llround(clamped_bottom)), static_cast<long long>(y + 1), static_cast<long long>(native_height)));

  result.x = x;
  result.y = y;
  result.width = right - x;
  result.height = bottom - y;
  return result;
}

void ApplyStageOverridesToResolvedSettings(const StageOverrideSet& overrides,
                                          ResolvedStageSettings* settings) {
  if (overrides.stage1_psf_sigma.has_value()) {
    settings->stage1_psf_sigma = *overrides.stage1_psf_sigma;
  }
  if (overrides.stage1_nsr.has_value()) {
    settings->stage1_nsr = *overrides.stage1_nsr;
  }
  if (overrides.stage2_denoise.has_value()) {
    settings->stage2_denoise = *overrides.stage2_denoise;
  }
  if (overrides.stage2_gain1.has_value()) {
    settings->stage2_gain1 = *overrides.stage2_gain1;
  }
  if (overrides.stage2_gain2.has_value()) {
    settings->stage2_gain2 = *overrides.stage2_gain2;
  }
  if (overrides.stage2_gain3.has_value()) {
    settings->stage2_gain3 = *overrides.stage2_gain3;
  }
  if (overrides.stage3_radius.has_value()) {
    settings->stage3_radius = *overrides.stage3_radius;
  }
  if (overrides.stage3_gain.has_value()) {
    settings->stage3_gain = *overrides.stage3_gain;
  }
}

ResolvedStageSettings ResolveDisplayStageSettings(const StageOverrideSet& overrides) {
  ResolvedStageSettings settings;
  ApplyStageOverridesToResolvedSettings(overrides, &settings);
  return settings;
}

StageOverrideSet MakeExplicitStageOverrides(const ResolvedStageSettings& settings) {
  StageOverrideSet overrides;
  overrides.stage1_psf_sigma = settings.stage1_psf_sigma;
  overrides.stage1_nsr = settings.stage1_nsr;
  overrides.stage2_denoise = settings.stage2_denoise;
  overrides.stage2_gain1 = settings.stage2_gain1;
  overrides.stage2_gain2 = settings.stage2_gain2;
  overrides.stage2_gain3 = settings.stage2_gain3;
  overrides.stage3_radius = settings.stage3_radius;
  overrides.stage3_gain = settings.stage3_gain;
  return overrides;
}

float ClampStage2UiGain(float value) {
  return std::clamp(value, 0.25f, 4.0f);
}

std::optional<float> MigrateLegacySmallDetailGain(std::optional<float> legacy_fine,
                                                  std::optional<float> legacy_small) {
  if (legacy_fine.has_value() && legacy_small.has_value()) {
    return ClampStage2UiGain(0.35f * *legacy_fine + 0.65f * *legacy_small);
  }
  if (legacy_small.has_value()) {
    return ClampStage2UiGain(*legacy_small);
  }
  if (legacy_fine.has_value()) {
    return ClampStage2UiGain(*legacy_fine);
  }
  return std::nullopt;
}

void SetFloatOverrideRelativeToBase(std::optional<float>* override_value,
                                    float value,
                                    float base_value) {
  if (std::abs(value - base_value) <= 1e-6f) {
    override_value->reset();
    return;
  }
  *override_value = value;
}

void SetIntOverrideRelativeToBase(std::optional<int>* override_value,
                                  int value,
                                  int base_value) {
  if (value == base_value) {
    override_value->reset();
    return;
  }
  *override_value = value;
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

  wxSize CurrentDrawSize() const {
    if (!preview_ || preview_->width == 0 || preview_->height == 0) {
      return wxSize(0, 0);
    }

    const double scale = CurrentScale();
    return wxSize(std::max(1, static_cast<int>(std::round(bitmap_.GetWidth() * scale))),
                  std::max(1, static_cast<int>(std::round(bitmap_.GetHeight() * scale))));
  }

  wxPoint CurrentImageOffset() const {
    if (zoom_mode_ != ZoomMode::kFit || !preview_) {
      return wxPoint(0, 0);
    }

    const wxSize client = GetClientSize();
    const wxSize draw_size = CurrentDrawSize();
    return wxPoint(std::max(0, (client.GetWidth() - draw_size.GetWidth()) / 2),
                   std::max(0, (client.GetHeight() - draw_size.GetHeight()) / 2));
  }

  wxPoint ToImagePoint(const wxPoint& point) const {
    int logical_x = 0;
    int logical_y = 0;
    CalcUnscrolledPosition(point.x, point.y, &logical_x, &logical_y);
    const wxPoint offset = CurrentImageOffset();
    const double scale = CurrentScale();
    return wxPoint(static_cast<int>(std::floor((logical_x - offset.x) / scale)),
                   static_cast<int>(std::floor((logical_y - offset.y) / scale)));
  }

  wxRect CropRectToView() const {
    const double scale = CurrentScale();
    const wxPoint offset = CurrentImageOffset();
    return wxRect(offset.x + static_cast<int>(std::round(crop_rect_.x * scale)),
                  offset.y + static_cast<int>(std::round(crop_rect_.y * scale)),
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

    const wxSize draw_size = CurrentDrawSize();
    const wxPoint offset = CurrentImageOffset();

    std::unique_ptr<wxGraphicsContext> gc(wxGraphicsContext::Create(dc));
    if (gc) {
      gc->DrawBitmap(bitmap_, offset.x, offset.y, draw_size.GetWidth(), draw_size.GetHeight());
      gc->SetPen(wxPen(wxColour(255, 193, 7), 2.0));
      gc->SetBrush(*wxTRANSPARENT_BRUSH);
      const wxRect crop_rect = CropRectToView();
      gc->DrawRectangle(crop_rect.x, crop_rect.y, crop_rect.width, crop_rect.height);
    } else {
      dc.DrawBitmap(bitmap_.ConvertToImage().Scale(draw_size.GetWidth(), draw_size.GetHeight()),
                    offset.x,
                    offset.y);
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

enum class OutputLocationMode {
  kSpecificDirectory,
  kRelativeToOriginal,
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
    LoadAppSettings();
    output_dir_picker_->SetPath(WxStringFromPath(base_output_dir_));
    relative_subdir_ctrl_->ChangeValue(WxStringFromPath(relative_subdir_));
    UpdateOutputLocationControls();
    UpdateCompressionChoice();
    UpdateResolvedSliderValues();
    UpdateButtons();
  }

  void AddFiles(const std::vector<std::string>& paths) {
    std::vector<std::pair<uint64_t, std::string>> metadata_jobs;
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
      item.target_path = ResolveOutputPathForMode(path);
      item.resolution_label = "...";
      metadata_jobs.emplace_back(item.id, item.source_path);
      queue_.push_back(item);
    }
    RefreshQueue();
    for (const auto& job : metadata_jobs) {
      BeginMetadataProbe(job.first, job.second);
    }
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
    const ResolvedStageSettings default_stage_settings;

    auto* root = new wxBoxSizer(wxVERTICAL);
    workspace_splitter_ = new wxSplitterWindow(this,
                           wxID_ANY,
                           wxDefaultPosition,
                           wxDefaultSize,
                           wxSP_LIVE_UPDATE);
    auto* detail_splitter = new wxSplitterWindow(workspace_splitter_,
                           wxID_ANY,
                           wxDefaultPosition,
                           wxDefaultSize,
                           wxSP_LIVE_UPDATE);
    auto make_section_label = [](wxWindow* parent, const wxString& text) {
      auto* label = new wxStaticText(parent, wxID_ANY, text);
      wxFont font = label->GetFont();
      font.MakeBold();
      label->SetFont(font);
      return label;
    };

    left_panel_ = new wxPanel(workspace_splitter_);
    left_panel_->SetMinSize(wxSize(320, -1));
    auto* left_sizer = new wxBoxSizer(wxVERTICAL);
    auto* queue_buttons = new wxBoxSizer(wxHORIZONTAL);
    add_files_button_ = new wxButton(left_panel_, wxID_ANY, "Add Files");
    remove_button_ = new wxButton(left_panel_, wxID_ANY, "Remove Selected");
    clear_button_ = new wxButton(left_panel_, wxID_ANY, "Clear");
    queue_view_toggle_button_ = new wxButton(left_panel_, wxID_ANY, ">");
    queue_view_toggle_button_->SetMinSize(wxSize(36, -1));
    queue_buttons->Add(add_files_button_, 0, wxRIGHT, 8);
    queue_buttons->Add(remove_button_, 0, wxRIGHT, 8);
    queue_buttons->Add(clear_button_, 0);
    queue_buttons->AddStretchSpacer();
    queue_buttons->Add(queue_view_toggle_button_, 0);

    queue_ctrl_ = new wxListCtrl(left_panel_, wxID_ANY, wxDefaultPosition, wxSize(420, -1),
                                 wxLC_REPORT | wxLC_HRULES | wxLC_VRULES);
    ConfigureQueueColumns();

    left_sizer->Add(queue_buttons, 0, wxALL | wxEXPAND, 10);
    left_sizer->Add(queue_ctrl_, 1, wxLEFT | wxRIGHT | wxBOTTOM | wxEXPAND, 10);
    left_panel_->SetSizer(left_sizer);

    auto* center_panel = new wxPanel(detail_splitter);
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

    auto* right_panel = new wxPanel(detail_splitter);
    right_panel->SetMinSize(wxSize(460, -1));
    auto* right_sizer = new wxBoxSizer(wxVERTICAL);

    right_sizer->Add(make_section_label(right_panel, "Converted Crop"), 0, wxLEFT | wxTOP, 10);
    crop_preview_panel_ = new FitImagePanel(right_panel);
    right_sizer->Add(crop_preview_panel_, 0, wxALL | wxEXPAND, 10);
    right_sizer->Add(new wxStaticLine(right_panel), 0, wxLEFT | wxRIGHT | wxBOTTOM | wxEXPAND, 10);

    right_sizer->Add(make_section_label(right_panel, "Output"), 0, wxLEFT, 10);
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
    compression_choice_->SetSelection(1);
    compression_row->Add(compression_choice_, 1);
    output_sizer->Add(compression_row, 0, wxEXPAND);
    specific_directory_radio_ =
      new wxRadioButton(output_panel, wxID_ANY, "Write to specific directory", wxDefaultPosition, wxDefaultSize, wxRB_GROUP);
    output_sizer->Add(specific_directory_radio_, 0, wxTOP, 10);
    output_dir_picker_ = new wxDirPickerCtrl(output_panel, wxID_ANY);
    output_sizer->Add(output_dir_picker_, 0, wxTOP | wxEXPAND, 6);
    relative_to_source_radio_ =
      new wxRadioButton(output_panel, wxID_ANY, "Write relative to original image");
    output_sizer->Add(relative_to_source_radio_, 0, wxTOP, 10);
    relative_subdir_hint_label_ =
      new wxStaticText(output_panel, wxID_ANY, "Leave empty to write next to the original image.");
    relative_subdir_hint_label_->SetForegroundColour(wxColour(110, 110, 110));
    output_sizer->Add(relative_subdir_hint_label_, 0, wxTOP | wxEXPAND, 4);
    relative_subdir_ctrl_ =
      new wxTextCtrl(output_panel, wxID_ANY, wxEmptyString, wxDefaultPosition, wxDefaultSize, wxBORDER_SIMPLE);
    relative_subdir_ctrl_->SetHint("Optional subfolder, e.g. converted");
    output_sizer->Add(relative_subdir_ctrl_, 0, wxTOP | wxEXPAND, 6);
    output_panel->SetSizer(output_sizer);
    right_sizer->Add(output_panel, 0, wxLEFT | wxRIGHT | wxBOTTOM | wxEXPAND, 10);
    right_sizer->Add(new wxStaticLine(right_panel), 0, wxLEFT | wxRIGHT | wxBOTTOM | wxEXPAND, 10);

    right_sizer->Add(make_section_label(right_panel, "Processing"), 0, wxLEFT, 10);
    auto* processing_panel = new wxPanel(right_panel);
    auto* sliders_scroll =
      new wxScrolledWindow(processing_panel, wxID_ANY, wxDefaultPosition, wxDefaultSize, wxVSCROLL);
    sliders_scroll->SetScrollRate(0, 16);
    auto* sliders_sizer = new wxBoxSizer(wxVERTICAL);

    auto* stage1_section = new wxPanel(sliders_scroll, wxID_ANY, wxDefaultPosition, wxDefaultSize, wxBORDER_THEME);
    auto* stage1_sizer = CreateStageSectionSizer(stage1_section,
                           "Detail Recovery",
                           "Recover fine detail from the stacked capture before later refinements.",
                           &stage1_reset_button_);
    stage1_sizer->Add(CreateFloatSlider(stage1_section,
                      "Blur Radius",
                      0.50,
                      4.00,
                      default_stage_settings.stage1_psf_sigma,
                      100,
                      2,
                      &stage1_sigma_),
                      0,
                      wxBOTTOM | wxEXPAND,
                      8);
    stage1_sizer->Add(CreateFloatSlider(stage1_section,
                      "Noise Protection",
                      0.00,
                      0.20,
                      default_stage_settings.stage1_nsr,
                      1000,
                      3,
                      &stage1_nsr_),
                      0,
                      wxEXPAND,
                      0);
    stage1_section->SetSizer(stage1_sizer);
    sliders_sizer->Add(stage1_section, 0, wxLEFT | wxRIGHT | wxTOP | wxEXPAND, 10);

    auto* stage2_section = new wxPanel(sliders_scroll, wxID_ANY, wxDefaultPosition, wxDefaultSize, wxBORDER_THEME);
    auto* stage2_sizer = CreateStageSectionSizer(stage2_section,
                           "Multi-scale Detail",
                           "Balance denoising and sharpening across small to large texture bands.",
                           &stage2_reset_button_);
    stage2_sizer->Add(CreateFloatSlider(stage2_section,
                      "Denoise",
                      0.00,
                      1.00,
                      default_stage_settings.stage2_denoise,
                      100,
                      2,
                      &stage2_denoise_),
                      0,
                      wxBOTTOM | wxEXPAND,
                      8);
    stage2_sizer->Add(CreateFloatSlider(stage2_section,
                      "Small Detail",
                      0.25,
                      4.00,
                      default_stage_settings.stage2_gain1,
                      100,
                      2,
                      &stage2_gain1_),
                      0,
                      wxBOTTOM | wxEXPAND,
                      8);
    stage2_sizer->Add(CreateFloatSlider(stage2_section,
                      "Medium Detail",
                      0.25,
                      4.00,
                      default_stage_settings.stage2_gain2,
                      100,
                      2,
                      &stage2_gain2_),
                      0,
                      wxBOTTOM | wxEXPAND,
                      8);
    stage2_sizer->Add(CreateFloatSlider(stage2_section,
                      "Large Detail",
                      0.25,
                      4.00,
                      default_stage_settings.stage2_gain3,
                      100,
                      2,
                      &stage2_gain3_),
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
    stage3_sizer->Add(CreateIntSlider(stage3_section,
                      "Edge Radius",
                      1,
                      16,
                      default_stage_settings.stage3_radius,
                      &stage3_radius_),
                      0,
                      wxBOTTOM | wxEXPAND,
                      8);
    stage3_sizer->Add(CreateFloatSlider(stage3_section,
                      "Edge Gain",
                      0.00,
                      4.00,
                      default_stage_settings.stage3_gain,
                      100,
                      2,
                      &stage3_gain_),
                      0,
                      wxEXPAND,
                      0);
    stage3_section->SetSizer(stage3_sizer);
    sliders_sizer->Add(stage3_section, 0, wxLEFT | wxRIGHT | wxTOP | wxEXPAND, 10);

    sliders_sizer->AddSpacer(10);
    sliders_scroll->SetSizer(sliders_sizer);
    sliders_scroll->FitInside();
    auto* processing_sizer = new wxBoxSizer(wxVERTICAL);
    processing_sizer->Add(sliders_scroll, 1, wxEXPAND);
    processing_panel->SetSizer(processing_sizer);
    right_sizer->Add(processing_panel, 1, wxLEFT | wxRIGHT | wxBOTTOM | wxEXPAND, 10);
    right_sizer->Add(new wxStaticLine(right_panel), 0, wxLEFT | wxRIGHT | wxBOTTOM | wxEXPAND, 10);

    auto* settings_action_row = new wxBoxSizer(wxHORIZONTAL);
    reset_button_ = new wxButton(right_panel, wxID_ANY, "Reset file");
    save_defaults_button_ = new wxButton(right_panel, wxID_ANY, "Save defaults");
    copy_settings_button_ = new wxButton(right_panel, wxID_ANY, "Copy settings");
    paste_settings_button_ = new wxButton(right_panel, wxID_ANY, "Paste settings");
    reset_button_->SetToolTip("Reset the selected file to the built-in safe processing values.");
    save_defaults_button_->SetToolTip("Save the current non-blur slider values as defaults for future files and launches.");
    copy_settings_button_->SetToolTip("Copy the current file's resolved processing settings.");
    paste_settings_button_->SetToolTip("Paste copied settings to all selected queue items.");
    settings_action_row->Add(reset_button_, 0, wxRIGHT, 8);
    settings_action_row->Add(copy_settings_button_, 0, wxRIGHT, 8);
    settings_action_row->Add(paste_settings_button_, 0, wxRIGHT, 8);
    settings_action_row->Add(save_defaults_button_, 0);
    settings_action_row->AddStretchSpacer();
    right_sizer->Add(settings_action_row, 0, wxLEFT | wxRIGHT | wxTOP | wxEXPAND, 10);

    auto* convert_action_row = new wxBoxSizer(wxHORIZONTAL);
    convert_button_ = new wxButton(right_panel, wxID_ANY, "Convert");
    cancel_button_ = new wxButton(right_panel, wxID_ANY, "Cancel");
    convert_action_row->Add(convert_button_, 1, wxRIGHT, 8);
    convert_action_row->Add(cancel_button_, 1);
    right_sizer->Add(convert_action_row, 0, wxALL | wxEXPAND, 10);

    progress_gauge_ = new wxGauge(right_panel, wxID_ANY, 100);
    status_label_ = new wxStaticText(right_panel, wxID_ANY, "Ready");
    right_sizer->Add(progress_gauge_, 0, wxLEFT | wxRIGHT | wxTOP | wxEXPAND, 10);
    right_sizer->Add(status_label_, 0, wxALL | wxEXPAND, 10);
    right_panel->SetSizer(right_sizer);

    detail_splitter->SetMinimumPaneSize(360);
    detail_splitter->SetSashGravity(1.0);
    detail_splitter->SplitVertically(center_panel, right_panel, -470);
    workspace_splitter_->SetMinimumPaneSize(260);
    workspace_splitter_->SetSashGravity(0.0);
    workspace_splitter_->SplitVertically(left_panel_, detail_splitter, 340);

    root->Add(workspace_splitter_, 1, wxEXPAND);
    SetSizer(root);
    ApplyQueueViewMode();
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
                              double initial_value,
                              int scale,
                              int decimals,
                              SliderControl* out_control) {
    auto* panel = new wxPanel(parent);
    auto* row = new wxBoxSizer(wxVERTICAL);
    auto* header = new wxBoxSizer(wxHORIZONTAL);
    auto* label_ctrl = new wxStaticText(panel, wxID_ANY, label);
    auto* value_ctrl = new wxStaticText(panel, wxID_ANY, "0");
    value_ctrl->SetMinSize(wxSize(44, -1));
    const int initial_raw_value = static_cast<int>(std::round(
      std::clamp(initial_value, min_value, max_value) * scale));
    auto* slider = new wxSlider(panel,
                                wxID_ANY,
                  initial_raw_value,
                                static_cast<int>(min_value * scale),
                                static_cast<int>(max_value * scale));
    header->Add(label_ctrl, 1, wxRIGHT, 8);
    header->Add(value_ctrl, 0, wxALIGN_CENTER_VERTICAL | wxLEFT, 8);
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
                            int initial_value,
                            SliderControl* out_control) {
    auto* panel = new wxPanel(parent);
    auto* row = new wxBoxSizer(wxVERTICAL);
    auto* header = new wxBoxSizer(wxHORIZONTAL);
    auto* label_ctrl = new wxStaticText(panel, wxID_ANY, label);
    auto* value_ctrl = new wxStaticText(panel, wxID_ANY, "0");
    value_ctrl->SetMinSize(wxSize(44, -1));
    auto* slider = new wxSlider(panel,
                  wxID_ANY,
                  std::clamp(initial_value, min_value, max_value),
                  min_value,
                  max_value);
    header->Add(label_ctrl, 1, wxRIGHT, 8);
    header->Add(value_ctrl, 0, wxALIGN_CENTER_VERTICAL | wxLEFT, 8);
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
    queue_view_toggle_button_->Bind(wxEVT_BUTTON, &HiracoMainFrame::OnToggleQueueView, this);
    convert_button_->Bind(wxEVT_BUTTON, &HiracoMainFrame::OnConvert, this);
    cancel_button_->Bind(wxEVT_BUTTON, &HiracoMainFrame::OnCancel, this);
    reset_button_->Bind(wxEVT_BUTTON, &HiracoMainFrame::OnResetDefaults, this);
    save_defaults_button_->Bind(wxEVT_BUTTON, &HiracoMainFrame::OnSaveDefaults, this);
    copy_settings_button_->Bind(wxEVT_BUTTON, &HiracoMainFrame::OnCopySettings, this);
    paste_settings_button_->Bind(wxEVT_BUTTON, &HiracoMainFrame::OnPasteSettings, this);
    stage1_reset_button_->Bind(wxEVT_BUTTON, &HiracoMainFrame::OnResetStage1Defaults, this);
    stage2_reset_button_->Bind(wxEVT_BUTTON, &HiracoMainFrame::OnResetStage2Defaults, this);
    stage3_reset_button_->Bind(wxEVT_BUTTON, &HiracoMainFrame::OnResetStage3Defaults, this);
    output_dir_picker_->Bind(wxEVT_DIRPICKER_CHANGED, &HiracoMainFrame::OnOutputDirChanged, this);
    relative_subdir_ctrl_->Bind(wxEVT_TEXT, &HiracoMainFrame::OnRelativeSubdirChanged, this);
    specific_directory_radio_->Bind(wxEVT_RADIOBUTTON, &HiracoMainFrame::OnOutputModeChanged, this);
    relative_to_source_radio_->Bind(wxEVT_RADIOBUTTON, &HiracoMainFrame::OnOutputModeChanged, this);
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
    Bind(EVT_HIRACO_METADATA_READY, &HiracoMainFrame::OnMetadataReady, this);
    Bind(EVT_HIRACO_CROP_READY, &HiracoMainFrame::OnCropReady, this);
    Bind(EVT_HIRACO_CONVERT_PROGRESS, &HiracoMainFrame::OnConvertProgress, this);
    Bind(EVT_HIRACO_CONVERT_DONE, &HiracoMainFrame::OnConvertDone, this);

    original_canvas_->SetCropChangedHandler([this](const CropRect& crop_rect) {
      current_crop_rect_ = crop_rect;
      ScheduleCropPreview(false);
    });

    BindSlider(stage1_sigma_, [this](double value) {
      UpdateSelectedStageOverrides([value](StageOverrideSet& overrides,
                                          const ResolvedStageSettings& base_settings) {
        SetFloatOverrideRelativeToBase(&overrides.stage1_psf_sigma,
                                       static_cast<float>(value),
                                       base_settings.stage1_psf_sigma);
      });
    });
    BindSlider(stage1_nsr_, [this](double value) {
      UpdateSelectedStageOverrides([value](StageOverrideSet& overrides,
                                          const ResolvedStageSettings& base_settings) {
        SetFloatOverrideRelativeToBase(&overrides.stage1_nsr,
                                       static_cast<float>(value),
                                       base_settings.stage1_nsr);
      });
    });
    BindSlider(stage2_denoise_, [this](double value) {
      UpdateSelectedStageOverrides([value](StageOverrideSet& overrides,
                                          const ResolvedStageSettings& base_settings) {
        SetFloatOverrideRelativeToBase(&overrides.stage2_denoise,
                                       static_cast<float>(value),
                                       base_settings.stage2_denoise);
      });
    });
    BindSlider(stage2_gain1_, [this](double value) {
      UpdateSelectedStageOverrides([value](StageOverrideSet& overrides,
                                          const ResolvedStageSettings& base_settings) {
        SetFloatOverrideRelativeToBase(&overrides.stage2_gain1,
                                       static_cast<float>(value),
                                       base_settings.stage2_gain1);
      });
    });
    BindSlider(stage2_gain2_, [this](double value) {
      UpdateSelectedStageOverrides([value](StageOverrideSet& overrides,
                                          const ResolvedStageSettings& base_settings) {
        SetFloatOverrideRelativeToBase(&overrides.stage2_gain2,
                                       static_cast<float>(value),
                                       base_settings.stage2_gain2);
      });
    });
    BindSlider(stage2_gain3_, [this](double value) {
      UpdateSelectedStageOverrides([value](StageOverrideSet& overrides,
                                          const ResolvedStageSettings& base_settings) {
        SetFloatOverrideRelativeToBase(&overrides.stage2_gain3,
                                       static_cast<float>(value),
                                       base_settings.stage2_gain3);
      });
    });
    BindSlider(stage3_radius_, [this](double value) {
      UpdateSelectedStageOverrides([value](StageOverrideSet& overrides,
                                          const ResolvedStageSettings& base_settings) {
        SetIntOverrideRelativeToBase(&overrides.stage3_radius,
                                     static_cast<int>(std::round(value)),
                                     base_settings.stage3_radius);
      });
    });
    BindSlider(stage3_gain_, [this](double value) {
      UpdateSelectedStageOverrides([value](StageOverrideSet& overrides,
                                          const ResolvedStageSettings& base_settings) {
        SetFloatOverrideRelativeToBase(&overrides.stage3_gain,
                                       static_cast<float>(value),
                                       base_settings.stage3_gain);
      });
    });
  }

  void ApplyStageSettingsToSliders(const ResolvedStageSettings& settings) {
    updating_sliders_ = true;
    SetSliderValue(stage1_sigma_, settings.stage1_psf_sigma);
    SetSliderValue(stage1_nsr_, settings.stage1_nsr);
    SetSliderValue(stage2_denoise_, settings.stage2_denoise);
    SetSliderValue(stage2_gain1_, settings.stage2_gain1);
    SetSliderValue(stage2_gain2_, settings.stage2_gain2);
    SetSliderValue(stage2_gain3_, settings.stage2_gain3);
    SetSliderValue(stage3_radius_, settings.stage3_radius);
    SetSliderValue(stage3_gain_, settings.stage3_gain);
    updating_sliders_ = false;
  }

  ResolvedStageSettings HardcodedSafeStageSettingsForItem(const QueueItem* item) const {
    if (item != nullptr && item->prepared.has_value()) {
      return GetResolvedStageSettings(*item->prepared, StageOverrideSet());
    }
    return ResolvedStageSettings();
  }

  StageOverrideSet HardcodedSafeStageOverridesForItem(const QueueItem* item) const {
    return MakeExplicitStageOverrides(HardcodedSafeStageSettingsForItem(item));
  }

  ResolvedStageSettings BaseStageSettingsForItem(const QueueItem* item) const {
    if (item != nullptr && item->prepared.has_value()) {
      return GetResolvedStageSettings(*item->prepared, app_stage_defaults_);
    }
    return ResolveDisplayStageSettings(app_stage_defaults_);
  }

  void NormalizeStageOverrides(QueueItem* item) {
    if (item == nullptr) {
      return;
    }

    const ResolvedStageSettings base_settings = BaseStageSettingsForItem(item);
    if (item->stage_overrides.stage1_psf_sigma.has_value()) {
      SetFloatOverrideRelativeToBase(&item->stage_overrides.stage1_psf_sigma,
                                     *item->stage_overrides.stage1_psf_sigma,
                                     base_settings.stage1_psf_sigma);
    }
    if (item->stage_overrides.stage1_nsr.has_value()) {
      SetFloatOverrideRelativeToBase(&item->stage_overrides.stage1_nsr,
                                     *item->stage_overrides.stage1_nsr,
                                     base_settings.stage1_nsr);
    }
    if (item->stage_overrides.stage2_denoise.has_value()) {
      SetFloatOverrideRelativeToBase(&item->stage_overrides.stage2_denoise,
                                     *item->stage_overrides.stage2_denoise,
                                     base_settings.stage2_denoise);
    }
    if (item->stage_overrides.stage2_gain1.has_value()) {
      SetFloatOverrideRelativeToBase(&item->stage_overrides.stage2_gain1,
                                     *item->stage_overrides.stage2_gain1,
                                     base_settings.stage2_gain1);
    }
    if (item->stage_overrides.stage2_gain2.has_value()) {
      SetFloatOverrideRelativeToBase(&item->stage_overrides.stage2_gain2,
                                     *item->stage_overrides.stage2_gain2,
                                     base_settings.stage2_gain2);
    }
    if (item->stage_overrides.stage2_gain3.has_value()) {
      SetFloatOverrideRelativeToBase(&item->stage_overrides.stage2_gain3,
                                     *item->stage_overrides.stage2_gain3,
                                     base_settings.stage2_gain3);
    }
    if (item->stage_overrides.stage3_radius.has_value()) {
      SetIntOverrideRelativeToBase(&item->stage_overrides.stage3_radius,
                                   *item->stage_overrides.stage3_radius,
                                   base_settings.stage3_radius);
    }
    if (item->stage_overrides.stage3_gain.has_value()) {
      SetFloatOverrideRelativeToBase(&item->stage_overrides.stage3_gain,
                                     *item->stage_overrides.stage3_gain,
                                     base_settings.stage3_gain);
    }
  }

  void UpdateSelectedStageOverrides(
      const std::function<void(StageOverrideSet&, const ResolvedStageSettings&)>& update) {
    QueueItem* item = SelectedItem();
    if (item == nullptr) {
      return;
    }

    const ResolvedStageSettings base_settings = BaseStageSettingsForItem(item);
    update(item->stage_overrides, base_settings);
    NormalizeStageOverrides(item);
    if (selected_row_ >= 0) {
      RefreshQueueRow(selected_row_);
    }
  }

  void UpdateResolvedSliderValues() {
    const QueueItem* item = SelectedItem();
    if (item == nullptr) {
      ApplyStageSettingsToSliders(ResolveDisplayStageSettings(app_stage_defaults_));
      return;
    }

    if (item->prepared.has_value()) {
      ApplyStageSettingsToSliders(
          GetResolvedStageSettings(*item->prepared, ResolveEffectiveStageOverrides(item->stage_overrides)));
      return;
    }

    ApplyStageSettingsToSliders(ResolveDisplayStageSettings(ResolveEffectiveStageOverrides(item->stage_overrides)));
  }

  QueueItem* SelectedItem() {
    if (selected_row_ < 0 || selected_row_ >= static_cast<int>(queue_.size())) {
      return nullptr;
    }
    return &queue_[selected_row_];
  }

  const QueueItem* SelectedItem() const {
    if (selected_row_ < 0 || selected_row_ >= static_cast<int>(queue_.size())) {
      return nullptr;
    }
    return &queue_[selected_row_];
  }

  StageOverrideSet* SelectedStageOverrides() {
    QueueItem* item = SelectedItem();
    return item == nullptr ? nullptr : &item->stage_overrides;
  }

  std::vector<int> GetSelectedRows() const {
    std::vector<int> rows;
    long row = -1;
    while ((row = queue_ctrl_->GetNextItem(row, wxLIST_NEXT_ALL, wxLIST_STATE_SELECTED)) != -1) {
      rows.push_back(static_cast<int>(row));
    }
    if (rows.empty() && selected_row_ >= 0 && selected_row_ < static_cast<int>(queue_.size())) {
      rows.push_back(selected_row_);
    }
    return rows;
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

    const CropRect coverage = PreviewCoverageRectInNative(prepared, preview_width, preview_height);
    const uint32_t mapped_width =
      OrientedImageWidth(coverage.width, coverage.height, prepared.metadata.libraw_flip);
    const uint32_t mapped_height =
      OrientedImageHeight(coverage.width, coverage.height, prepared.metadata.libraw_flip);

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

    const CropRect clamped_display = ClampCropRect(display_crop, display_preview_width_, display_preview_height_);
    const CropRect coverage = PreviewCoverageRectInNative(prepared,
                                display_preview_width_,
                                display_preview_height_);
    const ContinuousRect display_coverage =
      TransformNativeRectToDisplay(ToContinuousRect(coverage),
                     prepared.image_width,
                     prepared.image_height,
                     prepared.metadata.libraw_flip);

    const double display_coverage_width = std::max(1.0, display_coverage.right - display_coverage.left);
    const double display_coverage_height = std::max(1.0, display_coverage.bottom - display_coverage.top);

    ContinuousRect display_rect;
    display_rect.left = display_coverage.left +
      (static_cast<double>(clamped_display.x) * display_coverage_width) / static_cast<double>(display_preview_width_);
    display_rect.top = display_coverage.top +
      (static_cast<double>(clamped_display.y) * display_coverage_height) / static_cast<double>(display_preview_height_);
    display_rect.right = display_coverage.left +
      (static_cast<double>(clamped_display.x + clamped_display.width) * display_coverage_width) /
        static_cast<double>(display_preview_width_);
    display_rect.bottom = display_coverage.top +
      (static_cast<double>(clamped_display.y + clamped_display.height) * display_coverage_height) /
        static_cast<double>(display_preview_height_);

    const ContinuousRect native_rect =
      TransformDisplayRectToNative(display_rect,
                     prepared.image_width,
                     prepared.image_height,
                     prepared.metadata.libraw_flip);
    return ClampCropRect(ContinuousRectToCropRect(native_rect,
                            prepared.image_width,
                            prepared.image_height),
               prepared.image_width,
               prepared.image_height);
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

  StageOverrideSet ResolveEffectiveStageOverrides(const StageOverrideSet& item_overrides) const {
    StageOverrideSet effective = app_stage_defaults_;
    if (item_overrides.stage1_psf_sigma.has_value()) {
      effective.stage1_psf_sigma = item_overrides.stage1_psf_sigma;
    }
    if (item_overrides.stage1_nsr.has_value()) {
      effective.stage1_nsr = item_overrides.stage1_nsr;
    }
    if (item_overrides.stage2_denoise.has_value()) {
      effective.stage2_denoise = item_overrides.stage2_denoise;
    }
    if (item_overrides.stage2_gain1.has_value()) {
      effective.stage2_gain1 = item_overrides.stage2_gain1;
    }
    if (item_overrides.stage2_gain2.has_value()) {
      effective.stage2_gain2 = item_overrides.stage2_gain2;
    }
    if (item_overrides.stage2_gain3.has_value()) {
      effective.stage2_gain3 = item_overrides.stage2_gain3;
    }
    if (item_overrides.stage3_radius.has_value()) {
      effective.stage3_radius = item_overrides.stage3_radius;
    }
    if (item_overrides.stage3_gain.has_value()) {
      effective.stage3_gain = item_overrides.stage3_gain;
    }
    return effective;
  }

  void LoadAppSettings() {
    wxConfigBase* config = wxConfigBase::Get(false);
    if (config == nullptr) {
      return;
    }

    wxString compression_text;
    if (config->Read("ui/compression", &compression_text)) {
      HiracoCompression loaded = compression_;
      if (ParseCompressionString(compression_text.ToStdString(), &loaded)) {
        compression_ = loaded;
      }
    }

    long output_mode = 0;
    if (config->Read("ui/output_mode", &output_mode)) {
      output_location_mode_ = output_mode == 1
                                  ? OutputLocationMode::kRelativeToOriginal
                                  : OutputLocationMode::kSpecificDirectory;
    }

    wxString output_dir;
    if (config->Read("ui/output_dir", &output_dir) && !output_dir.empty()) {
      base_output_dir_ = PathFromWxString(output_dir);
    }

    wxString relative_subdir;
    if (config->Read("ui/relative_subdir", &relative_subdir)) {
      relative_subdir_ = PathFromWxString(relative_subdir);
    }

    double double_value = 0.0;
    if (config->Read("ui/stage1_nsr", &double_value)) {
      app_stage_defaults_.stage1_nsr = static_cast<float>(double_value);
    }
    if (config->Read("ui/stage2_denoise", &double_value)) {
      app_stage_defaults_.stage2_denoise = static_cast<float>(double_value);
    }
    double legacy_fine_detail = 0.0;
    double legacy_small_detail = 0.0;
    const bool has_legacy_fine_detail = config->Read("ui/stage2_gain0", &legacy_fine_detail);
    const bool has_legacy_small_detail = config->Read("ui/stage2_gain1", &legacy_small_detail);
    if (config->Read("ui/stage2_small_detail", &double_value)) {
      app_stage_defaults_.stage2_gain1 = ClampStage2UiGain(static_cast<float>(double_value));
    } else {
      const std::optional<float> migrated_small = MigrateLegacySmallDetailGain(
          has_legacy_fine_detail ? std::optional<float>(static_cast<float>(legacy_fine_detail)) : std::nullopt,
          has_legacy_small_detail ? std::optional<float>(static_cast<float>(legacy_small_detail)) : std::nullopt);
      if (migrated_small.has_value()) {
        app_stage_defaults_.stage2_gain1 = *migrated_small;
      }
    }
    if (config->Read("ui/stage2_medium_detail", &double_value)) {
      app_stage_defaults_.stage2_gain2 = ClampStage2UiGain(static_cast<float>(double_value));
    } else if (config->Read("ui/stage2_gain2", &double_value)) {
      app_stage_defaults_.stage2_gain2 = ClampStage2UiGain(static_cast<float>(double_value));
    }
    if (config->Read("ui/stage2_large_detail", &double_value)) {
      app_stage_defaults_.stage2_gain3 = ClampStage2UiGain(static_cast<float>(double_value));
    } else if (config->Read("ui/stage2_gain3", &double_value)) {
      app_stage_defaults_.stage2_gain3 = ClampStage2UiGain(static_cast<float>(double_value));
    }
    long int_value = 0;
    if (config->Read("ui/stage3_radius", &int_value)) {
      app_stage_defaults_.stage3_radius = static_cast<int>(int_value);
    }
    if (config->Read("ui/stage3_gain", &double_value)) {
      app_stage_defaults_.stage3_gain = static_cast<float>(double_value);
    }
  }

  void SaveAppSettings() const {
    wxConfigBase* config = wxConfigBase::Get(false);
    if (config == nullptr) {
      return;
    }

    config->Write("ui/compression", wxString::FromUTF8(ToCompressionString(compression_)));
    config->Write("ui/output_mode", output_location_mode_ == OutputLocationMode::kRelativeToOriginal ? 1L : 0L);
    config->Write("ui/output_dir", WxStringFromPath(base_output_dir_));
    config->Write("ui/relative_subdir", WxStringFromPath(relative_subdir_));

    if (app_stage_defaults_.stage1_nsr.has_value()) {
      config->Write("ui/stage1_nsr", static_cast<double>(*app_stage_defaults_.stage1_nsr));
    }
    if (app_stage_defaults_.stage2_denoise.has_value()) {
      config->Write("ui/stage2_denoise", static_cast<double>(*app_stage_defaults_.stage2_denoise));
    }
    if (app_stage_defaults_.stage2_gain1.has_value()) {
      config->Write("ui/stage2_small_detail", static_cast<double>(*app_stage_defaults_.stage2_gain1));
    }
    if (app_stage_defaults_.stage2_gain2.has_value()) {
      config->Write("ui/stage2_medium_detail", static_cast<double>(*app_stage_defaults_.stage2_gain2));
    }
    if (app_stage_defaults_.stage2_gain3.has_value()) {
      config->Write("ui/stage2_large_detail", static_cast<double>(*app_stage_defaults_.stage2_gain3));
    }
    if (app_stage_defaults_.stage3_radius.has_value()) {
      config->Write("ui/stage3_radius", static_cast<long>(*app_stage_defaults_.stage3_radius));
    }
    if (app_stage_defaults_.stage3_gain.has_value()) {
      config->Write("ui/stage3_gain", static_cast<double>(*app_stage_defaults_.stage3_gain));
    }
    config->Flush();
  }

  void SaveAppSettingsFromControls() {
    app_stage_defaults_.stage1_psf_sigma.reset();
    app_stage_defaults_.stage1_nsr = static_cast<float>(SliderValue(stage1_nsr_));
    app_stage_defaults_.stage2_denoise = static_cast<float>(SliderValue(stage2_denoise_));
    app_stage_defaults_.stage2_gain1 = static_cast<float>(SliderValue(stage2_gain1_));
    app_stage_defaults_.stage2_gain2 = static_cast<float>(SliderValue(stage2_gain2_));
    app_stage_defaults_.stage2_gain3 = static_cast<float>(SliderValue(stage2_gain3_));
    app_stage_defaults_.stage3_radius = static_cast<int>(std::round(SliderValue(stage3_radius_)));
    app_stage_defaults_.stage3_gain = static_cast<float>(SliderValue(stage3_gain_));
    SaveAppSettings();
  }

  std::filesystem::path ResolveOutputPathForMode(const std::string& source_path) const {
    const std::filesystem::path source(source_path);
    std::filesystem::path output_directory;
    if (output_location_mode_ == OutputLocationMode::kSpecificDirectory) {
      output_directory = base_output_dir_;
    } else {
      output_directory = source.parent_path();
      if (!relative_subdir_.empty()) {
        output_directory /= relative_subdir_;
      }
    }
    return output_directory / (source.stem().string() + ".dng");
  }

  void UpdateOutputLocationControls() {
    const bool specific_directory_mode = output_location_mode_ == OutputLocationMode::kSpecificDirectory;
    const bool controls_enabled = !conversion_running_ && !close_requested_.load();
    specific_directory_radio_->SetValue(specific_directory_mode);
    relative_to_source_radio_->SetValue(!specific_directory_mode);
    output_dir_picker_->Enable(controls_enabled && specific_directory_mode);
    relative_subdir_ctrl_->Enable(controls_enabled && !specific_directory_mode);
    relative_subdir_hint_label_->Enable(controls_enabled && !specific_directory_mode);
  }

  void ConfigureQueueColumns() {
    if (!queue_ctrl_) {
      return;
    }

    queue_ctrl_->ClearAll();
    queue_ctrl_->AppendColumn("File", wxLIST_FORMAT_LEFT, queue_details_expanded_ ? 180 : 220);
    if (queue_details_expanded_) {
      queue_ctrl_->AppendColumn("Size", wxLIST_FORMAT_CENTER, 72);
      queue_ctrl_->AppendColumn("Settings", wxLIST_FORMAT_CENTER, 82);
      queue_ctrl_->AppendColumn("Target", wxLIST_FORMAT_LEFT, 240);
    }
    queue_ctrl_->AppendColumn("Processed", wxLIST_FORMAT_CENTER, 112);
  }

  void PopulateQueueRow(long row, const QueueItem& item) {
    if (queue_details_expanded_) {
      queue_ctrl_->SetItem(row, 1, item.resolution_label);
      queue_ctrl_->SetItem(row, 2, SettingsMarkerForItem(item));
      queue_ctrl_->SetItem(row, 3, item.target_path.string());
      queue_ctrl_->SetItem(row, 4, ProcessedMarkerForItem(item));
      return;
    }

    queue_ctrl_->SetItem(row, 1, ProcessedMarkerForItem(item));
  }

  void ApplyQueueViewMode() {
    if (!queue_ctrl_) {
      return;
    }

    const int desired_left_width = queue_details_expanded_ ? 690 : 368;
    const int selected_row = selected_row_;

    ConfigureQueueColumns();
    RefreshQueue();
    if (selected_row >= 0 && selected_row < static_cast<int>(queue_.size())) {
      queue_ctrl_->SetItemState(selected_row,
                                wxLIST_STATE_SELECTED | wxLIST_STATE_FOCUSED,
                                wxLIST_STATE_SELECTED | wxLIST_STATE_FOCUSED);
    }

    if (queue_view_toggle_button_) {
      queue_view_toggle_button_->SetLabel(queue_details_expanded_ ? "<" : ">");
      queue_view_toggle_button_->SetToolTip(queue_details_expanded_ ? "Collapse queue details" : "Expand queue details");
    }
    if (left_panel_) {
      left_panel_->SetMinSize(wxSize(desired_left_width, -1));
    }
    queue_ctrl_->SetMinSize(wxSize(desired_left_width - 20, -1));
    if (workspace_splitter_ && workspace_splitter_->IsSplit()) {
      workspace_splitter_->SetSashPosition(desired_left_width);
    }
    Layout();
  }

  void OnToggleQueueView(wxCommandEvent&) {
    queue_details_expanded_ = !queue_details_expanded_;
    ApplyQueueViewMode();
  }

  void UpdateButtons() {
    if (close_requested_.load()) {
      add_files_button_->Enable(false);
      remove_button_->Enable(false);
      clear_button_->Enable(false);
      queue_view_toggle_button_->Enable(false);
      convert_button_->Enable(false);
      cancel_button_->Enable(false);
      reset_button_->Enable(false);
      save_defaults_button_->Enable(false);
      copy_settings_button_->Enable(false);
      paste_settings_button_->Enable(false);
      stage1_reset_button_->Enable(false);
      stage2_reset_button_->Enable(false);
      stage3_reset_button_->Enable(false);
      stage1_sigma_.slider->Enable(false);
      stage1_nsr_.slider->Enable(false);
      stage2_denoise_.slider->Enable(false);
      stage2_gain1_.slider->Enable(false);
      stage2_gain2_.slider->Enable(false);
      stage2_gain3_.slider->Enable(false);
      stage3_radius_.slider->Enable(false);
      stage3_gain_.slider->Enable(false);
      UpdateOutputLocationControls();
      return;
    }

    const bool has_selection = selected_row_ >= 0 && selected_row_ < static_cast<int>(queue_.size());
    const bool has_prepared_selection = has_selection && queue_[selected_row_].prepared.has_value();
    const bool has_clipboard_settings = copied_stage_overrides_.has_value();
    const bool has_selected_rows = !GetSelectedRows().empty();
    remove_button_->Enable(has_selection && !conversion_running_);
    clear_button_->Enable(!queue_.empty() && !conversion_running_);
    add_files_button_->Enable(!conversion_running_);
    queue_view_toggle_button_->Enable(true);
    convert_button_->Enable(!queue_.empty() && !conversion_running_);
    cancel_button_->Enable(conversion_running_);
    reset_button_->Enable(has_selection && !conversion_running_);
    save_defaults_button_->Enable(has_selection && !conversion_running_);
    stage1_reset_button_->Enable(has_selection && !conversion_running_);
    stage2_reset_button_->Enable(has_selection && !conversion_running_);
    stage3_reset_button_->Enable(has_selection && !conversion_running_);
    copy_settings_button_->Enable(has_prepared_selection && !conversion_running_);
    paste_settings_button_->Enable(has_selected_rows && has_clipboard_settings && !conversion_running_);
    stage1_sigma_.slider->Enable(has_selection && !conversion_running_);
    stage1_nsr_.slider->Enable(has_selection && !conversion_running_);
    stage2_denoise_.slider->Enable(has_selection && !conversion_running_);
    stage2_gain1_.slider->Enable(has_selection && !conversion_running_);
    stage2_gain2_.slider->Enable(has_selection && !conversion_running_);
    stage2_gain3_.slider->Enable(has_selection && !conversion_running_);
    stage3_radius_.slider->Enable(has_selection && !conversion_running_);
    stage3_gain_.slider->Enable(has_selection && !conversion_running_);
    UpdateOutputLocationControls();
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
    wxString tooltip;
    if (point.x < file_width) {
      tooltip = item.source_path;
    } else if (!queue_details_expanded_) {
      if (item.state == "Done") {
        tooltip = "Converted";
      } else if (item.state == "Skipped") {
        tooltip = "Skipped";
      } else if (item.state == "Failed" || item.state == "Canceled") {
        tooltip = item.message.empty() ? wxString(item.state) : wxString(item.message);
      } else {
        tooltip = "Not converted yet";
      }
    } else {
      const int size_width = queue_ctrl_->GetColumnWidth(1);
      const int settings_width = queue_ctrl_->GetColumnWidth(2);
      const int target_width = queue_ctrl_->GetColumnWidth(3);
      if (point.x < file_width + size_width) {
        if (item.prepared.has_value()) {
          tooltip = ResolutionTooltipForPrepared(*item.prepared);
        } else if (item.resolution_label == "...") {
          tooltip = "Reading file metadata...";
        } else {
          tooltip = "Resolution unavailable";
        }
      } else if (point.x < file_width + size_width + settings_width) {
        tooltip = item.stage_overrides.HasAnyOverrides()
                      ? wxString("Custom per-file processing settings")
                      : wxString("Using file-specific defaults");
      } else if (point.x < file_width + size_width + settings_width + target_width) {
        tooltip = item.target_path.string();
      } else if (item.state == "Done") {
        tooltip = "Converted";
      } else if (item.state == "Skipped") {
        tooltip = "Skipped";
      } else if (item.state == "Failed" || item.state == "Canceled") {
        tooltip = item.message.empty() ? wxString(item.state) : wxString(item.message);
      } else {
        tooltip = "Not converted yet";
      }
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
      PopulateQueueRow(row, item);
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
    PopulateQueueRow(index, queue_[index]);
  }

  void BeginMetadataProbe(uint64_t item_id, const std::string& source_path) {
    LaunchWorker([this, item_id, source_path]() {
      MetadataReadyPayload payload;
      payload.item_id = item_id;

      std::string error;
      PreparedSource prepared;
      if (!PrepareSource(source_path,
                         &prepared,
                         &error,
                         {},
                         [this]() { return close_requested_.load(); })) {
        payload.ok = false;
        payload.error = error;
      } else {
        payload.ok = true;
        payload.prepared = prepared;
      }

      auto* event = new wxThreadEvent(EVT_HIRACO_METADATA_READY);
      event->SetPayload(payload);
      wxQueueEvent(this, event);
    });
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
        crop_preview_panel_->SetPreview(nullptr);
        queue_[selected_row_].state = "Ready";
        queue_[selected_row_].message = "Original preview ready";
        queue_[selected_row_].resolution_label = ResolutionLabelForPrepared(*item_copy.prepared);
        RefreshQueueRow(selected_row_);
        progress_gauge_->SetValue(0);
        status_label_->SetLabel("Original preview ready. Rendering converted crop...");
        UpdateResolvedSliderValues();
        UpdateButtons();
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
    const StageOverrideSet stage_overrides = ResolveEffectiveStageOverrides(item.stage_overrides);
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
    UpdateButtons();
    StartSelectionLoad();
  }

  void OnOutputDirChanged(wxFileDirPickerEvent&) {
    base_output_dir_ = PathFromWxString(output_dir_picker_->GetPath());
    RebuildTargetPaths();
    SaveAppSettings();
  }

  void OnRelativeSubdirChanged(wxCommandEvent&) {
    relative_subdir_ = PathFromWxString(relative_subdir_ctrl_->GetValue());
    RebuildTargetPaths();
    SaveAppSettings();
  }

  void OnOutputModeChanged(wxCommandEvent&) {
    output_location_mode_ = specific_directory_radio_->GetValue()
                                ? OutputLocationMode::kSpecificDirectory
                                : OutputLocationMode::kRelativeToOriginal;
    UpdateOutputLocationControls();
    RebuildTargetPaths();
    SaveAppSettings();
  }

  void RebuildTargetPaths() {
    for (QueueItem& item : queue_) {
      item.target_path = ResolveOutputPathForMode(item.source_path);
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
        compression_ = HiracoCompression::kDeflate;
        break;
    }
    SaveAppSettings();
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
    if (QueueItem* item = SelectedItem()) {
      item->stage_overrides = HardcodedSafeStageOverridesForItem(item);
      NormalizeStageOverrides(item);
      RefreshQueueRow(selected_row_);
    }
    RefreshCropPreviewIfPossible();
  }

  void OnSaveDefaults(wxCommandEvent&) {
    if (SelectedItem() == nullptr) {
      return;
    }

    SaveAppSettingsFromControls();
    for (QueueItem& item : queue_) {
      NormalizeStageOverrides(&item);
    }
    RefreshQueue();
    UpdateResolvedSliderValues();
    status_label_->SetLabel("Saved current defaults");
  }

  void OnResetStage1Defaults(wxCommandEvent&) {
    QueueItem* item = SelectedItem();
    if (StageOverrideSet* overrides = SelectedStageOverrides()) {
      const ResolvedStageSettings safe = HardcodedSafeStageSettingsForItem(item);
      overrides->stage1_psf_sigma = safe.stage1_psf_sigma;
      overrides->stage1_nsr = safe.stage1_nsr;
      NormalizeStageOverrides(item);
      RefreshQueueRow(selected_row_);
    }
    RefreshCropPreviewIfPossible();
  }

  void OnResetStage2Defaults(wxCommandEvent&) {
    QueueItem* item = SelectedItem();
    if (StageOverrideSet* overrides = SelectedStageOverrides()) {
      const ResolvedStageSettings safe = HardcodedSafeStageSettingsForItem(item);
      overrides->stage2_denoise = safe.stage2_denoise;
      overrides->stage2_gain1 = safe.stage2_gain1;
      overrides->stage2_gain2 = safe.stage2_gain2;
      overrides->stage2_gain3 = safe.stage2_gain3;
      NormalizeStageOverrides(item);
      RefreshQueueRow(selected_row_);
    }
    RefreshCropPreviewIfPossible();
  }

  void OnResetStage3Defaults(wxCommandEvent&) {
    QueueItem* item = SelectedItem();
    if (StageOverrideSet* overrides = SelectedStageOverrides()) {
      const ResolvedStageSettings safe = HardcodedSafeStageSettingsForItem(item);
      overrides->stage3_radius = safe.stage3_radius;
      overrides->stage3_gain = safe.stage3_gain;
      NormalizeStageOverrides(item);
      RefreshQueueRow(selected_row_);
    }
    RefreshCropPreviewIfPossible();
  }

  void OnCopySettings(wxCommandEvent&) {
    QueueItem* item = SelectedItem();
    if (item == nullptr || !item->prepared.has_value()) {
      return;
    }

    copied_stage_overrides_ = MakeExplicitStageOverrides(
        GetResolvedStageSettings(*item->prepared, ResolveEffectiveStageOverrides(item->stage_overrides)));
    status_label_->SetLabel("Copied settings from selected file");
    UpdateButtons();
  }

  void OnPasteSettings(wxCommandEvent&) {
    if (!copied_stage_overrides_.has_value()) {
      return;
    }

    const std::vector<int> selected_rows = GetSelectedRows();
    if (selected_rows.empty()) {
      return;
    }

    bool current_row_updated = false;
    for (const int row : selected_rows) {
      if (row < 0 || row >= static_cast<int>(queue_.size())) {
        continue;
      }
      queue_[row].stage_overrides = *copied_stage_overrides_;
      NormalizeStageOverrides(&queue_[row]);
      RefreshQueueRow(row);
      current_row_updated = current_row_updated || row == selected_row_;
    }

    if (current_row_updated) {
      RefreshCropPreviewIfPossible();
    }

    status_label_->SetLabel(wxString::Format("Pasted settings to %zu file%s",
                                             selected_rows.size(),
                                             selected_rows.size() == 1 ? "" : "s"));
    UpdateButtons();
  }

  void OnMetadataReady(wxThreadEvent& event) {
    if (close_requested_.load()) {
      return;
    }

    const MetadataReadyPayload payload = event.GetPayload<MetadataReadyPayload>();
    const int index = FindItemIndex(payload.item_id);
    if (index < 0) {
      return;
    }

    if (payload.ok) {
      if (!queue_[index].prepared.has_value()) {
        queue_[index].prepared = payload.prepared;
      }
      queue_[index].resolution_label = ResolutionLabelForPrepared(*queue_[index].prepared);
    } else if (queue_[index].resolution_label == "...") {
      queue_[index].resolution_label = "?";
    }

    RefreshQueueRow(index);
    if (index == selected_row_) {
      UpdateResolvedSliderValues();
      UpdateButtons();
    }
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
    queue_[index].resolution_label = ResolutionLabelForPrepared(payload.prepared);
    queue_[index].state = "Ready";
    queue_[index].message = "Original preview ready";
    RefreshQueueRow(index);
    UpdateButtons();

    if (index >= 0 && index == selected_row_) {
      current_crop_rect_ = payload.crop_rect;
      display_preview_width_ = payload.original_preview->width;
      display_preview_height_ = payload.original_preview->height;
      original_canvas_->SetPreview(payload.original_preview);
      original_canvas_->SetCropRect(*current_crop_rect_);
      crop_preview_panel_->SetPreview(nullptr);
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
                     ResolveEffectiveStageOverrides(item.stage_overrides),
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
  wxButton* queue_view_toggle_button_ = nullptr;
  wxButton* reset_button_ = nullptr;
  wxButton* save_defaults_button_ = nullptr;
  wxButton* copy_settings_button_ = nullptr;
  wxButton* paste_settings_button_ = nullptr;
  wxButton* stage1_reset_button_ = nullptr;
  wxButton* stage2_reset_button_ = nullptr;
  wxButton* stage3_reset_button_ = nullptr;
  wxButton* convert_button_ = nullptr;
  wxButton* cancel_button_ = nullptr;
  wxListCtrl* queue_ctrl_ = nullptr;
  wxPanel* left_panel_ = nullptr;
  PreviewCanvas* original_canvas_ = nullptr;
  FitImagePanel* crop_preview_panel_ = nullptr;
  wxSplitterWindow* workspace_splitter_ = nullptr;
  wxChoice* zoom_choice_ = nullptr;
  wxChoice* compression_choice_ = nullptr;
  wxRadioButton* specific_directory_radio_ = nullptr;
  wxRadioButton* relative_to_source_radio_ = nullptr;
  wxDirPickerCtrl* output_dir_picker_ = nullptr;
  wxStaticText* relative_subdir_hint_label_ = nullptr;
  wxTextCtrl* relative_subdir_ctrl_ = nullptr;
  wxGauge* progress_gauge_ = nullptr;
  wxStaticText* status_label_ = nullptr;
  wxTimer shutdown_timer_;
  wxTimer crop_preview_timer_;
  wxString queue_tooltip_text_;
  bool queue_details_expanded_ = false;

  SliderControl stage1_sigma_;
  SliderControl stage1_nsr_;
  SliderControl stage2_denoise_;
  SliderControl stage2_gain1_;
  SliderControl stage2_gain2_;
  SliderControl stage2_gain3_;
  SliderControl stage3_radius_;
  SliderControl stage3_gain_;

  std::vector<QueueItem> queue_;
  std::filesystem::path base_output_dir_;
  std::filesystem::path relative_subdir_;
  StageOverrideSet app_stage_defaults_;
  OutputLocationMode output_location_mode_ = OutputLocationMode::kSpecificDirectory;
  HiracoCompression compression_ = HiracoCompression::kDeflate;
  std::optional<StageOverrideSet> copied_stage_overrides_;
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
    SetVendorName("gorol");
    SetAppName("hiraco");
    wxConfigBase::Set(new wxConfig(GetAppName(), GetVendorName()));
    SetExitOnFrameDelete(true);
    auto* frame = new HiracoMainFrame();
    SetTopWindow(frame);
    frame->Show();
    return true;
  }
};

}  // namespace

wxIMPLEMENT_APP(HiracoGuiApp);
