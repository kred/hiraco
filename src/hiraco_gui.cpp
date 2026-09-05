#include "hiraco_core.h"

#include <wx/bitmap.h>
#include <wx/activityindicator.h>
#include <wx/artprov.h>
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
#include <condition_variable>
#include <cstring>
#include <deque>
#include <filesystem>
#include <functional>
#include <future>
#include <memory>
#include <optional>
#include <set>
#include <thread>
#include <vector>

namespace {

wxDECLARE_EVENT(EVT_HIRACO_SELECTION_READY, wxThreadEvent);
wxDECLARE_EVENT(EVT_HIRACO_METADATA_READY, wxThreadEvent);
wxDECLARE_EVENT(EVT_HIRACO_CONVERTED_PREVIEW_READY, wxThreadEvent);
wxDECLARE_EVENT(EVT_HIRACO_CONVERT_PROGRESS, wxThreadEvent);

wxDEFINE_EVENT(EVT_HIRACO_SELECTION_READY, wxThreadEvent);
wxDEFINE_EVENT(EVT_HIRACO_METADATA_READY, wxThreadEvent);
wxDEFINE_EVENT(EVT_HIRACO_CONVERTED_PREVIEW_READY, wxThreadEvent);
wxDEFINE_EVENT(EVT_HIRACO_CONVERT_PROGRESS, wxThreadEvent);

constexpr uint32_t kFullPreviewMaxDimension = 2560;
// The processing algorithm temporarily allocates several full-frame working
// planes. Retain up to four 80 MP source caches while reserving most of a
// 10 GB working-set allowance for those transient buffers.
constexpr size_t kPreviewCacheBudgetBytes = 2ull * 1024ull * 1024ull * 1024ull;

const wxColour kControlSurface(235, 236, 238);
const wxColour kControlBorder(213, 215, 218);
const wxColour kControlSelected(76, 81, 87);
const wxColour kControlSelectedHover(63, 68, 73);
const wxColour kControlText(42, 45, 48);
const wxColour kControlMutedText(102, 106, 110);
const wxColour kQueueSurface(248, 248, 248);
const wxColour kQueueSelectedSurface(230, 234, 238);

enum class ProcessingPreset {
  kNone,
  kSmall,
  kMedium,
  kStrong,
  kCustom,
};

struct QueueItem {
  uint64_t id = 0;
  std::string source_path;
  std::filesystem::path target_path;
  std::optional<PreparedSource> prepared;
  bool enable_highlight_recovery = false;
  StageOverrideSet stage_overrides;
  // This is deliberately separate from the highlighted preset.  A slider
  // adjustment may mean no button is highlighted, but Restore should still
  // return to the last preset the user chose.
  ProcessingPreset last_preset = ProcessingPreset::kSmall;
  wxString resolution_label;
  uint64_t preview_cache_access_sequence = 0;
};

struct SelectionReadyPayload {
  uint64_t item_id = 0;
  uint64_t request_id = 0;
  bool ok = false;
  PreparedSource prepared;
  std::shared_ptr<PreviewImage> original_preview;
  std::string error;
};

struct MetadataReadyPayload {
  uint64_t item_id = 0;
  bool ok = false;
  PreparedSource prepared;
  std::string error;
};

struct ConvertedPreviewReadyPayload {
  uint64_t item_id = 0;
  uint64_t request_id = 0;
  bool ok = false;
  std::shared_ptr<PreviewImage> converted_preview;
  std::string error;
};

struct ConvertProgressPayload {
  double overall_fraction = 0.0;
  std::string message;
};

enum class WorkerPriority {
  kInteractive,
  kBackground,
};

// Image processing already uses OpenMP, FFTW and Halide worker pools. Running
// several full-image jobs simultaneously oversubscribes those pools and makes
// the UI slower, not faster. One dispatcher keeps work bounded while allowing
// the latest interactive request to jump ahead of queued metadata work.
class ProcessingTaskQueue {
 public:
  ProcessingTaskQueue() : worker_([this]() { Run(); }) {}

  ProcessingTaskQueue(const ProcessingTaskQueue&) = delete;
  ProcessingTaskQueue& operator=(const ProcessingTaskQueue&) = delete;

  ~ProcessingTaskQueue() {
    Stop();
  }

  bool Enqueue(WorkerPriority priority, std::function<void()> task) {
    std::lock_guard<std::mutex> lock(mutex_);
    if (stopping_) {
      return false;
    }
    if (priority == WorkerPriority::kInteractive) {
      interactive_tasks_.push_back(std::move(task));
    } else {
      background_tasks_.push_back(std::move(task));
    }
    ready_.notify_one();
    return true;
  }

  size_t DiscardPending() {
    std::lock_guard<std::mutex> lock(mutex_);
    const size_t count = interactive_tasks_.size() + background_tasks_.size();
    interactive_tasks_.clear();
    background_tasks_.clear();
    return count;
  }

  void Stop() {
    {
      std::lock_guard<std::mutex> lock(mutex_);
      if (stopping_) {
        return;
      }
      stopping_ = true;
      interactive_tasks_.clear();
      background_tasks_.clear();
    }
    ready_.notify_one();
    if (worker_.joinable()) {
      worker_.join();
    }
  }

 private:
  void Run() {
    while (true) {
      std::function<void()> task;
      {
        std::unique_lock<std::mutex> lock(mutex_);
        ready_.wait(lock, [this]() {
          return stopping_ || !interactive_tasks_.empty() || !background_tasks_.empty();
        });
        if (stopping_) {
          return;
        }
        if (!interactive_tasks_.empty()) {
          task = std::move(interactive_tasks_.front());
          interactive_tasks_.pop_front();
        } else {
          task = std::move(background_tasks_.front());
          background_tasks_.pop_front();
        }
      }
      task();
    }
  }

  std::mutex mutex_;
  std::condition_variable ready_;
  std::deque<std::function<void()>> interactive_tasks_;
  std::deque<std::function<void()>> background_tasks_;
  bool stopping_ = false;
  std::thread worker_;
};

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

wxBitmap MakeThumbnailBitmap(std::shared_ptr<const PreviewImage> preview,
                             int max_width,
                             int max_height) {
  if (!preview || preview->width == 0 || preview->height == 0 || preview->colors < 3 ||
      max_width <= 0 || max_height <= 0) {
    return wxBitmap();
  }

  const double scale = std::min(static_cast<double>(max_width) / preview->width,
                                static_cast<double>(max_height) / preview->height);
  const int width = std::max(1, static_cast<int>(std::round(preview->width * scale)));
  const int height = std::max(1, static_cast<int>(std::round(preview->height * scale)));
  wxImage image(width, height);
  unsigned char* rgb = image.GetData();
  for (int row = 0; row < height; ++row) {
    const uint32_t source_row = std::min<uint32_t>(
        preview->height - 1,
        static_cast<uint32_t>((static_cast<uint64_t>(row) * preview->height) / height));
    for (int col = 0; col < width; ++col) {
      const uint32_t source_col = std::min<uint32_t>(
          preview->width - 1,
          static_cast<uint32_t>((static_cast<uint64_t>(col) * preview->width) / width));
      const size_t source_index =
          (static_cast<size_t>(source_row) * preview->width + source_col) * preview->colors;
      const size_t target_index = (static_cast<size_t>(row) * width + col) * 3;
      rgb[target_index + 0] = preview->pixels[source_index + 0];
      rgb[target_index + 1] = preview->pixels[source_index + 1];
      rgb[target_index + 2] = preview->pixels[source_index + 2];
    }
  }
  return wxBitmap(image);
}

std::shared_ptr<PreviewImage> ExtractPreviewRegion(std::shared_ptr<const PreviewImage> preview,
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

std::shared_ptr<PreviewImage> ScalePreviewImage(std::shared_ptr<const PreviewImage> preview,
                                                uint32_t max_dimension) {
  if (!preview || preview->width == 0 || preview->height == 0 || preview->colors == 0 ||
      max_dimension == 0) {
    return nullptr;
  }

  const uint32_t longest_edge = std::max(preview->width, preview->height);
  if (longest_edge <= max_dimension) {
    return std::make_shared<PreviewImage>(*preview);
  }

  const double scale = static_cast<double>(max_dimension) / longest_edge;
  auto scaled = std::make_shared<PreviewImage>();
  scaled->width = std::max(1u, static_cast<uint32_t>(std::lround(preview->width * scale)));
  scaled->height = std::max(1u, static_cast<uint32_t>(std::lround(preview->height * scale)));
  scaled->colors = preview->colors;
  scaled->bits = preview->bits;
  scaled->pixels.resize(static_cast<size_t>(scaled->width) * scaled->height * scaled->colors);

  for (uint32_t row = 0; row < scaled->height; ++row) {
    const uint32_t source_row = std::min<uint32_t>(
        preview->height - 1,
        static_cast<uint32_t>((static_cast<uint64_t>(row) * preview->height) / scaled->height));
    for (uint32_t column = 0; column < scaled->width; ++column) {
      const uint32_t source_column = std::min<uint32_t>(
          preview->width - 1,
          static_cast<uint32_t>((static_cast<uint64_t>(column) * preview->width) / scaled->width));
      const size_t source_index =
          (static_cast<size_t>(source_row) * preview->width + source_column) * preview->colors;
      const size_t target_index =
          (static_cast<size_t>(row) * scaled->width + column) * scaled->colors;
      std::memcpy(scaled->pixels.data() + target_index,
                  preview->pixels.data() + source_index,
                  scaled->colors);
    }
  }
  return scaled;
}

std::shared_ptr<PreviewImage> MakeComparisonOriginalPreview(
    std::shared_ptr<const PreviewImage> preview,
    const PreparedSource& prepared) {
  if (!preview || preview->width == 0 || preview->height == 0) {
    return nullptr;
  }

  std::shared_ptr<PreviewImage> comparison_preview = std::make_shared<PreviewImage>(*preview);
  const SourceLinearDngMetadata& metadata = prepared.metadata;
  if (metadata.has_default_crop && metadata.default_crop_width > 0 &&
      metadata.default_crop_height > 0 && prepared.image_width > 0 && prepared.image_height > 0 &&
      metadata.default_crop_origin_h < prepared.image_width &&
      metadata.default_crop_origin_v < prepared.image_height) {
    CropRect native_crop;
    native_crop.x = metadata.default_crop_origin_h;
    native_crop.y = metadata.default_crop_origin_v;
    native_crop.width = std::min(metadata.default_crop_width, prepared.image_width - native_crop.x);
    native_crop.height = std::min(metadata.default_crop_height, prepared.image_height - native_crop.y);
    const ContinuousRect oriented_crop = TransformNativeRectToDisplay(
        ToContinuousRect(native_crop), prepared.image_width, prepared.image_height, metadata.libraw_flip);
    const uint32_t oriented_width =
        OrientedImageWidth(prepared.image_width, prepared.image_height, metadata.libraw_flip);
    const uint32_t oriented_height =
        OrientedImageHeight(prepared.image_width, prepared.image_height, metadata.libraw_flip);
    if (oriented_width > 0 && oriented_height > 0) {
      const auto scale_coordinate = [](double coordinate, uint32_t source_extent, uint32_t target_extent) {
        return static_cast<uint32_t>(std::clamp<long long>(
            std::llround(coordinate * target_extent / source_extent), 0, target_extent));
      };
      const uint32_t left = scale_coordinate(oriented_crop.left, oriented_width, preview->width);
      const uint32_t top = scale_coordinate(oriented_crop.top, oriented_height, preview->height);
      const uint32_t right = scale_coordinate(oriented_crop.right, oriented_width, preview->width);
      const uint32_t bottom = scale_coordinate(oriented_crop.bottom, oriented_height, preview->height);
      if (right > left && bottom > top) {
        comparison_preview = ExtractPreviewRegion(
            comparison_preview, CropRect{left, top, right - left, bottom - top});
      }
    }
  }
  return ScalePreviewImage(comparison_preview, kFullPreviewMaxDimension);
}

class PaletteButton final : public wxControl {
 public:
  PaletteButton(wxWindow* parent, wxWindowID id, const wxString& label)
      : wxControl(parent, id, wxDefaultPosition, wxDefaultSize, wxBORDER_NONE) {
    SetLabel(label);
    SetBackgroundStyle(wxBG_STYLE_PAINT);
    Bind(wxEVT_PAINT, &PaletteButton::OnPaint, this);
    Bind(wxEVT_LEFT_DOWN, &PaletteButton::OnLeftDown, this);
    Bind(wxEVT_LEFT_UP, &PaletteButton::OnLeftUp, this);
    Bind(wxEVT_ENTER_WINDOW, &PaletteButton::OnMouseEnter, this);
    Bind(wxEVT_LEAVE_WINDOW, &PaletteButton::OnMouseLeave, this);
  }

  void SetSelected(bool selected) {
    if (selected_ == selected) {
      return;
    }
    selected_ = selected;
    Refresh();
  }

 protected:
  wxSize DoGetBestSize() const override {
    wxClientDC dc(const_cast<PaletteButton*>(this));
    dc.SetFont(GetFont());
    const wxSize text = dc.GetTextExtent(GetLabel());
    return wxSize(std::max(42, text.GetWidth() + 24), std::max(28, text.GetHeight() + 12));
  }

 private:
  void OnPaint(wxPaintEvent&) {
    wxAutoBufferedPaintDC dc(this);
    dc.SetBackground(wxBrush(GetParent()->GetBackgroundColour()));
    dc.Clear();

    const wxSize size = GetClientSize();
    const bool enabled = IsEnabled();
    wxColour fill = kControlSurface;
    wxColour text_colour = kControlText;
    if (!enabled) {
      fill = wxColour(242, 243, 244);
      text_colour = wxColour(155, 158, 161);
    } else if (selected_) {
      fill = hovered_ ? kControlSelectedHover : kControlSelected;
      text_colour = *wxWHITE;
    } else if (hovered_) {
      fill = wxColour(225, 227, 229);
    }

    std::unique_ptr<wxGraphicsContext> graphics(wxGraphicsContext::Create(dc));
    if (graphics != nullptr) {
      graphics->SetBrush(wxBrush(fill));
      graphics->SetPen(wxPen(selected_ ? fill : kControlBorder, 1.0));
      graphics->DrawRoundedRectangle(0.5, 0.5,
                                     std::max(0, size.GetWidth() - 1),
                                     std::max(0, size.GetHeight() - 1),
                                     7.0);
      graphics->SetFont(GetFont(), text_colour);
      wxDouble width = 0.0;
      wxDouble height = 0.0;
      graphics->GetTextExtent(GetLabel(), &width, &height);
      graphics->DrawText(GetLabel(), (size.GetWidth() - width) / 2.0,
                         (size.GetHeight() - height) / 2.0);
      return;
    }

    dc.SetBrush(wxBrush(fill));
    dc.SetPen(wxPen(selected_ ? fill : kControlBorder));
    dc.DrawRoundedRectangle(0, 0, size.GetWidth(), size.GetHeight(), 7);
    dc.SetTextForeground(text_colour);
    const wxSize text = dc.GetTextExtent(GetLabel());
    dc.DrawText(GetLabel(), (size.GetWidth() - text.GetWidth()) / 2,
                (size.GetHeight() - text.GetHeight()) / 2);
  }

  void OnLeftDown(wxMouseEvent& event) {
    if (IsEnabled()) {
      pressed_ = true;
      CaptureMouse();
    }
    event.Skip();
  }

  void OnLeftUp(wxMouseEvent& event) {
    const bool activate = pressed_ && GetClientRect().Contains(event.GetPosition());
    pressed_ = false;
    if (HasCapture()) {
      ReleaseMouse();
    }
    if (activate) {
      wxCommandEvent command(wxEVT_BUTTON, GetId());
      command.SetEventObject(this);
      ProcessWindowEvent(command);
    }
    event.Skip();
  }

  void OnMouseEnter(wxMouseEvent& event) {
    hovered_ = true;
    Refresh();
    event.Skip();
  }

  void OnMouseLeave(wxMouseEvent& event) {
    hovered_ = false;
    Refresh();
    event.Skip();
  }

  bool selected_ = false;
  bool hovered_ = false;
  bool pressed_ = false;
};

class CompareCanvas final : public wxScrolledWindow {
 public:
  enum class ComparisonMode {
    kOriginal,
    kConverted,
    kSideBySide,
  };

  enum class ZoomMode {
    kFit,
    k50,
    k100,
  };

  explicit CompareCanvas(wxWindow* parent)
      : wxScrolledWindow(parent, wxID_ANY, wxDefaultPosition, wxSize(800, 600), wxBORDER_SIMPLE) {
    SetBackgroundStyle(wxBG_STYLE_PAINT);
    SetScrollRate(1, 1);
    Bind(wxEVT_PAINT, &CompareCanvas::OnPaint, this);
    Bind(wxEVT_SIZE, &CompareCanvas::OnSize, this);
    Bind(wxEVT_LEFT_DOWN, &CompareCanvas::OnLeftDown, this);
    Bind(wxEVT_LEFT_UP, &CompareCanvas::OnLeftUp, this);
    Bind(wxEVT_MOTION, &CompareCanvas::OnMotion, this);
    Bind(wxEVT_LEAVE_WINDOW, &CompareCanvas::OnMouseLeave, this);
  }

  void SetOriginalPreview(std::shared_ptr<const PreviewImage> preview) {
    original_preview_ = std::move(preview);
    original_bitmap_ = MakeBitmapFromPreview(original_preview_);
    UpdateVirtualArea();
    Refresh();
  }

  void SetConvertedPreview(std::shared_ptr<const PreviewImage> preview) {
    converted_preview_ = std::move(preview);
    converted_bitmap_ = MakeBitmapFromPreview(converted_preview_);
    UpdateVirtualArea();
    Refresh();
  }

  void SetComparisonMode(ComparisonMode mode) {
    int view_x = 0;
    int view_y = 0;
    GetViewStart(&view_x, &view_y);
    const bool was_fit = zoom_mode_ == ZoomMode::kFit;
    const double previous_scale = CurrentScale();
    const ComparisonMode previous_mode = comparison_mode_;
    if (mode == ComparisonMode::kSideBySide && previous_mode != ComparisonMode::kSideBySide) {
      non_side_by_side_zoom_mode_ = zoom_mode_;
    } else if (mode != ComparisonMode::kSideBySide && previous_mode == ComparisonMode::kSideBySide) {
      zoom_mode_ = non_side_by_side_zoom_mode_;
    }
    comparison_mode_ = mode;
    // Side by side is a pixel-level comparison.  Its shared view is always
    // 100%, so the same source coordinate is visible in both panes.
    if (comparison_mode_ == ComparisonMode::kSideBySide) {
      zoom_mode_ = ZoomMode::k100;
    }
    UpdateVirtualArea();
    if (zoom_mode_ == ZoomMode::kFit) {
      Scroll(0, 0);
    } else if (comparison_mode_ == ComparisonMode::kSideBySide && was_fit) {
      CenterView();
    } else {
      const double current_scale = CurrentScale();
      RestoreViewPosition(static_cast<int>(std::round(view_x * current_scale / previous_scale)),
                          static_cast<int>(std::round(view_y * current_scale / previous_scale)));
    }
    Refresh();
  }

  ComparisonMode GetComparisonMode() const {
    return comparison_mode_;
  }

  void SetZoomMode(ZoomMode mode) {
    if (comparison_mode_ == ComparisonMode::kSideBySide) {
      mode = ZoomMode::k100;
    } else {
      non_side_by_side_zoom_mode_ = mode;
    }
    zoom_mode_ = mode;
    UpdateVirtualArea();
    if (zoom_mode_ == ZoomMode::kFit) {
      Scroll(0, 0);
    } else {
      CenterView();
    }
    Refresh();
  }

  ZoomMode GetZoomMode() const {
    return zoom_mode_;
  }

  void ZoomIn() {
    if (comparison_mode_ == ComparisonMode::kSideBySide) {
      return;
    }
    if (zoom_mode_ == ZoomMode::kFit) {
      SetZoomMode(ZoomMode::k50);
    } else {
      SetZoomMode(ZoomMode::k100);
    }
  }

  void ZoomOut() {
    if (comparison_mode_ == ComparisonMode::kSideBySide) {
      return;
    }
    if (zoom_mode_ == ZoomMode::k100) {
      SetZoomMode(ZoomMode::k50);
    } else {
      SetZoomMode(ZoomMode::kFit);
    }
  }

  bool HasOriginalPreview() const {
    return original_bitmap_.IsOk();
  }

  bool HasConvertedPreview() const {
    return converted_bitmap_.IsOk();
  }

 private:
  const wxBitmap* PrimaryBitmap() const {
    if (original_bitmap_.IsOk()) {
      return &original_bitmap_;
    }
    if (converted_bitmap_.IsOk()) {
      return &converted_bitmap_;
    }
    return nullptr;
  }

  double CurrentScale() const {
    const wxBitmap* bitmap = PrimaryBitmap();
    if (bitmap == nullptr) {
      return 1.0;
    }

    switch (zoom_mode_) {
      case ZoomMode::k50:
        return 0.5;
      case ZoomMode::k100:
        return 1.0;
      case ZoomMode::kFit: {
        const wxSize client = GetClientSize();
        const int available_width = comparison_mode_ == ComparisonMode::kSideBySide
            ? std::max(1, client.GetWidth() / 2)
            : std::max(1, client.GetWidth());
        const double scale_x = static_cast<double>(available_width) / bitmap->GetWidth();
        const double scale_y = static_cast<double>(std::max(1, client.GetHeight())) / bitmap->GetHeight();
        return std::max(0.05, std::min(scale_x, scale_y));
      }
    }
    return 1.0;
  }

  wxSize SingleDrawSize() const {
    const wxBitmap* bitmap = PrimaryBitmap();
    if (bitmap == nullptr) {
      return wxSize(0, 0);
    }
    const double scale = CurrentScale();
    return wxSize(std::max(1, static_cast<int>(std::round(bitmap->GetWidth() * scale))),
                  std::max(1, static_cast<int>(std::round(bitmap->GetHeight() * scale))));
  }

  wxSize CurrentDrawSize() const {
    wxSize size = SingleDrawSize();
    if (comparison_mode_ == ComparisonMode::kSideBySide) {
      // The two panes share one pan position.  The additional pane width gives
      // the scrolled window enough horizontal range to reveal either edge of
      // both images while they remain visible together.
      size.SetWidth(size.GetWidth() + std::max(1, GetClientSize().GetWidth() / 2));
    }
    return size;
  }

  wxPoint CurrentImageOffset() const {
    if (zoom_mode_ != ZoomMode::kFit || PrimaryBitmap() == nullptr) {
      return wxPoint(0, 0);
    }
    const wxSize client = GetClientSize();
    const wxSize single = SingleDrawSize();
    const int available_width = comparison_mode_ == ComparisonMode::kSideBySide
        ? std::max(1, client.GetWidth() / 2)
        : std::max(1, client.GetWidth());
    return wxPoint(std::max(0, (available_width - single.GetWidth()) / 2),
                   std::max(0, (client.GetHeight() - single.GetHeight()) / 2));
  }

  void UpdateVirtualArea() {
    if (PrimaryBitmap() == nullptr) {
      SetVirtualSize(0, 0);
      Scroll(0, 0);
      return;
    }
    const wxSize draw = CurrentDrawSize();
    SetVirtualSize(draw.GetWidth(), draw.GetHeight());
  }

  void CenterView() {
    const wxSize virtual_size = GetVirtualSize();
    const wxSize client = GetClientSize();
    Scroll(std::max(0, (virtual_size.GetWidth() - client.GetWidth()) / 2),
           std::max(0, (virtual_size.GetHeight() - client.GetHeight()) / 2));
  }

  void RestoreViewPosition(int view_x, int view_y) {
    const wxSize virtual_size = GetVirtualSize();
    const wxSize client = GetClientSize();
    Scroll(std::clamp(view_x, 0, std::max(0, virtual_size.GetWidth() - client.GetWidth())),
           std::clamp(view_y, 0, std::max(0, virtual_size.GetHeight() - client.GetHeight())));
  }

  void DrawBitmap(wxGraphicsContext* graphics,
                  wxDC& dc,
                  const wxBitmap& bitmap,
                  int x,
                  int y,
                  int width,
                  int height) const {
    if (!bitmap.IsOk() || width <= 0 || height <= 0) {
      return;
    }
    if (graphics != nullptr) {
      graphics->DrawBitmap(bitmap, x, y, width, height);
      return;
    }
    dc.DrawBitmap(bitmap.ConvertToImage().Scale(width, height), x, y);
  }

  void DrawLabel(wxGraphicsContext* graphics, wxDC& dc, const wxString& label, int x, int y) const {
    if (graphics != nullptr) {
      graphics->SetFont(GetFont(), wxColour(245, 245, 245));
      graphics->SetBrush(wxBrush(wxColour(20, 20, 20, 210)));
      graphics->SetPen(*wxTRANSPARENT_PEN);
      wxDouble width = 0.0;
      wxDouble height = 0.0;
      graphics->GetTextExtent(label, &width, &height);
      graphics->DrawRoundedRectangle(x, y, width + 18, 28, 6);
      graphics->DrawText(label, x + 9, y + 6);
      return;
    }
    dc.SetTextForeground(*wxWHITE);
    dc.SetBrush(wxBrush(wxColour(20, 20, 20)));
    dc.SetPen(*wxTRANSPARENT_PEN);
    const wxSize text_size = dc.GetTextExtent(label);
    dc.DrawRoundedRectangle(x, y, text_size.GetWidth() + 18, 28, 6);
    dc.DrawText(label, x + 9, y + 6);
  }

  void OnPaint(wxPaintEvent&) {
    wxAutoBufferedPaintDC dc(this);
    PrepareDC(dc);
    dc.SetBackground(wxBrush(wxColour(18, 18, 18)));
    dc.Clear();

    const wxBitmap* primary = PrimaryBitmap();
    if (primary == nullptr) {
      dc.SetTextForeground(*wxLIGHT_GREY);
      dc.DrawText("Select an image to preview", 16, 16);
      return;
    }

    const wxSize single_size = SingleDrawSize();
    const wxPoint offset = CurrentImageOffset();
    std::unique_ptr<wxGraphicsContext> graphics(wxGraphicsContext::Create(dc));
    wxGraphicsContext* gc = graphics.get();
    const wxBitmap& original = original_bitmap_.IsOk() ? original_bitmap_ : *primary;
    const wxBitmap& converted = converted_bitmap_.IsOk() ? converted_bitmap_ : original;

    if (comparison_mode_ == ComparisonMode::kSideBySide) {
      const wxSize client = GetClientSize();
      const int left_pane_width = std::max(1, client.GetWidth() / 2);
      const int right_pane_width = std::max(1, client.GetWidth() - left_pane_width);
      int view_x = 0;
      int view_y = 0;
      GetViewStart(&view_x, &view_y);
      const int left_pane_x = view_x;
      const int right_pane_x = view_x + left_pane_width;
      const auto draw_pane = [&](const wxBitmap& bitmap, int pane_x, int pane_width, int image_x) {
        if (gc != nullptr) {
          gc->PushState();
          gc->Clip(pane_x, view_y, pane_width, client.GetHeight());
          DrawBitmap(gc,
                     dc,
                     bitmap,
                     image_x,
                     offset.y,
                     single_size.GetWidth(),
                     single_size.GetHeight());
          gc->PopState();
          return;
        }
        dc.SetClippingRegion(pane_x, view_y, pane_width, client.GetHeight());
        DrawBitmap(nullptr,
                   dc,
                   bitmap,
                   image_x,
                   offset.y,
                   single_size.GetWidth(),
                   single_size.GetHeight());
        dc.DestroyClippingRegion();
      };
      draw_pane(original, left_pane_x, left_pane_width, offset.x);
      draw_pane(converted, right_pane_x, right_pane_width, left_pane_width + offset.x);
      DrawLabel(gc, dc, "Original", left_pane_x + 12, view_y + 12);
      DrawLabel(gc, dc, "Converted", right_pane_x + 12, view_y + 12);
      if (gc != nullptr) {
        gc->SetPen(wxPen(wxColour(210, 210, 210), 1.0));
        gc->StrokeLine(right_pane_x, view_y, right_pane_x, view_y + client.GetHeight());
      } else {
        dc.SetPen(wxPen(wxColour(210, 210, 210), 1));
        dc.DrawLine(right_pane_x, view_y, right_pane_x, view_y + client.GetHeight());
      }
      return;
    }

    if (comparison_mode_ == ComparisonMode::kConverted) {
      DrawBitmap(gc, dc, converted, offset.x, offset.y, single_size.GetWidth(), single_size.GetHeight());
      int view_x = 0;
      int view_y = 0;
      GetViewStart(&view_x, &view_y);
      DrawLabel(gc, dc, "Converted", view_x + 12, view_y + 12);
      return;
    }

    DrawBitmap(gc, dc, original, offset.x, offset.y, single_size.GetWidth(), single_size.GetHeight());
    int view_x = 0;
    int view_y = 0;
    GetViewStart(&view_x, &view_y);
    DrawLabel(gc, dc, "Original", view_x + 12, view_y + 12);
  }

  void OnSize(wxSizeEvent& event) {
    if (zoom_mode_ == ZoomMode::kFit) {
      UpdateVirtualArea();
    }
    Refresh();
    event.Skip();
  }

  void OnLeftDown(wxMouseEvent& event) {
    if (PrimaryBitmap() == nullptr) {
      return;
    }
    panning_ = false;
    last_pan_point_ = event.GetPosition();
    CaptureMouse();
  }

  void OnLeftUp(wxMouseEvent&) {
    panning_ = false;
    if (HasCapture()) {
      ReleaseMouse();
    }
  }

  void OnMotion(wxMouseEvent& event) {
    if (!event.Dragging() || !event.LeftIsDown() || PrimaryBitmap() == nullptr) {
      if (zoom_mode_ != ZoomMode::kFit) {
        SetCursor(wxCursor(wxCURSOR_HAND));
      } else {
        SetCursor(wxNullCursor);
      }
      return;
    }

    if (zoom_mode_ != ZoomMode::kFit) {
      const wxPoint current = event.GetPosition();
      const int dx = current.x - last_pan_point_.x;
      const int dy = current.y - last_pan_point_.y;
      int view_x = 0;
      int view_y = 0;
      GetViewStart(&view_x, &view_y);
      Scroll(std::max(0, view_x - dx), std::max(0, view_y - dy));
      last_pan_point_ = current;
      panning_ = true;
    }
  }

  void OnMouseLeave(wxMouseEvent& event) {
    if (!panning_) {
      SetCursor(wxNullCursor);
    }
    event.Skip();
  }

  std::shared_ptr<const PreviewImage> original_preview_;
  std::shared_ptr<const PreviewImage> converted_preview_;
  wxBitmap original_bitmap_;
  wxBitmap converted_bitmap_;
  ComparisonMode comparison_mode_ = ComparisonMode::kConverted;
  ZoomMode zoom_mode_ = ZoomMode::kFit;
  ZoomMode non_side_by_side_zoom_mode_ = ZoomMode::kFit;
  bool panning_ = false;
  wxPoint last_pan_point_;
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
  // Slider notifications are emitted continuously while a thumb is dragged.
  // Keep that interaction visual-only; the release submits the one preview
  // request for the final value.
  bool dragging = false;
  std::optional<int> last_preview_request_value;
};

enum class OutputLocationMode {
  kSpecificDirectory,
  kNextToOriginal,
  kSubfolderUnderOriginal,
};

class HiracoMainFrame final : public wxFrame {
 public:
  HiracoMainFrame()
      : wxFrame(nullptr, wxID_ANY, "hiraco-gui", wxDefaultPosition, wxSize(1600, 980)),
  shutdown_timer_(this) {
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
    UpdateCompareButtons();
    UpdateZoomButtons();
    UpdateButtons();
    FinishStatusActivity("Ready");
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

    const bool should_select_first = selected_row_ < 0 && !queue_.empty();
    const uint64_t selected_item_id = should_select_first ? queue_.front().id : 0;
    RefreshQueue();

    // Queue the first image's interactive preview before opportunistic queue
    // metadata.  Otherwise a just-added file can be prepared once by the
    // background probe and a second time by the selection worker.
    if (should_select_first) {
      SelectRow(0);
    }
    for (const auto& job : metadata_jobs) {
      if (job.first == selected_item_id) {
        continue;
      }
      BeginMetadataProbe(job.first, job.second);
    }
  }

 private:
  template <typename Task>
  void LaunchWorker(WorkerPriority priority, Task task) {
    active_workers_.fetch_add(1);
    const bool queued = processing_tasks_.Enqueue(priority, [this, task = std::move(task)]() mutable {
      task();
      CallAfter([this]() {
        active_workers_.fetch_sub(1);
        MaybeFinishClose();
      });
    });
    if (!queued) {
      active_workers_.fetch_sub(1);
    }
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
    clear_button_ = new wxButton(left_panel_, wxID_ANY, "Clear all");
    queue_buttons->Add(add_files_button_, 0, wxRIGHT, 8);
    queue_buttons->Add(clear_button_, 0);
    queue_buttons->AddStretchSpacer();

    queue_scroll_ = new wxScrolledWindow(left_panel_, wxID_ANY, wxDefaultPosition, wxSize(320, -1), wxVSCROLL);
    queue_scroll_->SetScrollRate(0, 12);
    queue_sizer_ = new wxBoxSizer(wxVERTICAL);
    queue_scroll_->SetSizer(queue_sizer_);

    left_sizer->Add(queue_buttons, 0, wxALL | wxEXPAND, 10);
    left_sizer->Add(queue_scroll_, 1, wxLEFT | wxRIGHT | wxBOTTOM | wxEXPAND, 10);
    left_panel_->SetSizer(left_sizer);

    auto* center_panel = new wxPanel(detail_splitter);
    auto* center_sizer = new wxBoxSizer(wxVERTICAL);
    auto* preview_toolbar = new wxBoxSizer(wxHORIZONTAL);
    auto* compare_toolbar = new wxBoxSizer(wxHORIZONTAL);
    compare_toolbar->Add(new wxStaticText(center_panel, wxID_ANY, "Compare:"),
                         0,
                         wxALIGN_CENTER_VERTICAL | wxRIGHT,
                         8);
    original_mode_button_ = new PaletteButton(center_panel, wxID_ANY, "Original");
    converted_mode_button_ = new PaletteButton(center_panel, wxID_ANY, "Converted");
    side_by_side_mode_button_ = new PaletteButton(center_panel, wxID_ANY, "Side by side");
    compare_toolbar->Add(original_mode_button_, 0, wxRIGHT, 4);
    compare_toolbar->Add(converted_mode_button_, 0, wxRIGHT, 4);
    compare_toolbar->Add(side_by_side_mode_button_, 0);
    auto* zoom_toolbar = new wxBoxSizer(wxHORIZONTAL);
    zoom_toolbar->Add(new wxStaticText(center_panel, wxID_ANY, "Zoom:"),
                         0,
                         wxALIGN_CENTER_VERTICAL | wxRIGHT,
                         8);
    zoom_out_button_ = new PaletteButton(center_panel, wxID_ANY, "−");
    zoom_fit_button_ = new PaletteButton(center_panel, wxID_ANY, "Fit");
    zoom_50_button_ = new PaletteButton(center_panel, wxID_ANY, "50%");
    zoom_100_button_ = new PaletteButton(center_panel, wxID_ANY, "100%");
    zoom_in_button_ = new PaletteButton(center_panel, wxID_ANY, "+");
    zoom_out_button_->SetMinSize(wxSize(32, -1));
    zoom_in_button_->SetMinSize(wxSize(32, -1));
    zoom_toolbar->Add(zoom_out_button_, 0, wxRIGHT, 4);
    zoom_toolbar->Add(zoom_fit_button_, 0, wxRIGHT, 4);
    zoom_toolbar->Add(zoom_50_button_, 0, wxRIGHT, 4);
    zoom_toolbar->Add(zoom_100_button_, 0, wxRIGHT, 4);
    zoom_toolbar->Add(zoom_in_button_, 0);
    preview_toolbar->Add(compare_toolbar, 0, wxALIGN_CENTER_VERTICAL);
    preview_toolbar->AddStretchSpacer();
    preview_toolbar->Add(zoom_toolbar, 0, wxALIGN_CENTER_VERTICAL);

    compare_canvas_ = new CompareCanvas(center_panel);
    center_sizer->Add(preview_toolbar, 0, wxALL | wxEXPAND, 10);
    center_sizer->Add(compare_canvas_, 1, wxLEFT | wxRIGHT | wxBOTTOM | wxEXPAND, 10);
    center_panel->SetSizer(center_sizer);

    inspector_panel_ = new wxPanel(detail_splitter);
    auto* right_panel = inspector_panel_;
    right_panel->SetMinSize(wxSize(460, -1));
    auto* right_sizer = new wxBoxSizer(wxVERTICAL);

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
    output_options_pane_ = new wxCollapsiblePane(output_panel, wxID_ANY, "Output Options");
    auto* options_panel = output_options_pane_->GetPane();
    auto* options_sizer = new wxBoxSizer(wxVERTICAL);
    specific_directory_radio_ =
      new wxRadioButton(options_panel, wxID_ANY, "Specific folder", wxDefaultPosition, wxDefaultSize, wxRB_GROUP);
    options_sizer->Add(specific_directory_radio_, 0, wxTOP | wxEXPAND, 6);
    output_dir_picker_ = new wxDirPickerCtrl(options_panel, wxID_ANY);
    options_sizer->Add(output_dir_picker_, 0, wxTOP | wxEXPAND, 4);
    next_to_source_radio_ =
      new wxRadioButton(options_panel, wxID_ANY, "Next to original ORF");
    options_sizer->Add(next_to_source_radio_, 0, wxTOP | wxEXPAND, 8);
    relative_subdir_radio_ =
      new wxRadioButton(options_panel, wxID_ANY, "Subfolder under original ORF");
    options_sizer->Add(relative_subdir_radio_, 0, wxTOP | wxEXPAND, 8);
    relative_subdir_ctrl_ =
      new wxTextCtrl(options_panel, wxID_ANY, wxEmptyString, wxDefaultPosition, wxDefaultSize, wxBORDER_SIMPLE);
    relative_subdir_ctrl_->SetHint("Subfolder, e.g. converted");
    options_sizer->Add(relative_subdir_ctrl_, 0, wxTOP | wxEXPAND, 4);
    options_panel->SetSizer(options_sizer);
    options_panel->Fit();
    output_options_pane_->Collapse(false);
    output_sizer->Add(output_options_pane_, 0, wxTOP | wxEXPAND, 8);
    output_panel->SetSizer(output_sizer);
    right_sizer->Add(output_panel, 0, wxLEFT | wxRIGHT | wxBOTTOM | wxEXPAND, 10);
    right_sizer->Add(new wxStaticLine(right_panel), 0, wxLEFT | wxRIGHT | wxBOTTOM | wxEXPAND, 10);

    right_sizer->Add(make_section_label(right_panel, "Presets"), 0, wxLEFT | wxBOTTOM, 10);
    auto* presets_row = new wxBoxSizer(wxHORIZONTAL);
    small_preset_button_ = new PaletteButton(right_panel, wxID_ANY, "Small");
    medium_preset_button_ = new PaletteButton(right_panel, wxID_ANY, "Medium");
    strong_preset_button_ = new PaletteButton(right_panel, wxID_ANY, "Strong");
    custom_preset_button_ = new PaletteButton(right_panel, wxID_ANY, "Custom");
    presets_row->Add(small_preset_button_, 1, wxRIGHT, 6);
    presets_row->Add(medium_preset_button_, 1, wxRIGHT, 6);
    presets_row->Add(strong_preset_button_, 1, wxRIGHT, 6);
    presets_row->Add(custom_preset_button_, 1);
    right_sizer->Add(presets_row, 0, wxLEFT | wxRIGHT | wxBOTTOM | wxEXPAND, 10);

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
    auto* convert_action_row = new wxBoxSizer(wxHORIZONTAL);
    convert_button_ = new wxButton(right_panel, wxID_ANY, "Convert");
    cancel_button_ = new wxButton(right_panel, wxID_ANY, "Cancel");
    convert_action_row->Add(convert_button_, 1, wxRIGHT, 8);
    convert_action_row->Add(cancel_button_, 1);
    right_sizer->Add(convert_action_row, 0, wxALL | wxEXPAND, 10);

    right_panel->SetSizer(right_sizer);

    detail_splitter->SetMinimumPaneSize(360);
    detail_splitter->SetSashGravity(1.0);
    detail_splitter->SplitVertically(center_panel, right_panel, -470);
    workspace_splitter_->SetMinimumPaneSize(260);
    workspace_splitter_->SetSashGravity(0.0);
    workspace_splitter_->SplitVertically(left_panel_, detail_splitter, 340);

    root->Add(workspace_splitter_, 1, wxEXPAND);
    auto* status_panel = new wxPanel(this, wxID_ANY, wxDefaultPosition, wxDefaultSize, wxBORDER_THEME);
    auto* status_sizer = new wxBoxSizer(wxHORIZONTAL);
    activity_indicator_ = new wxActivityIndicator(status_panel, wxID_ANY);
    status_label_ = new wxStaticText(status_panel, wxID_ANY, "Ready");
    progress_gauge_ = new wxGauge(status_panel, wxID_ANY, 100, wxDefaultPosition, wxSize(180, -1));
    activity_indicator_->SetMinSize(wxSize(22, 22));
    status_sizer->Add(activity_indicator_, 0, wxALIGN_CENTER_VERTICAL | wxLEFT | wxRIGHT, 8);
    status_sizer->Add(status_label_, 1, wxALIGN_CENTER_VERTICAL | wxRIGHT, 10);
    status_sizer->Add(progress_gauge_, 0, wxALIGN_CENTER_VERTICAL | wxTOP | wxBOTTOM | wxRIGHT, 5);
    status_panel->SetSizer(status_sizer);
    status_panel->SetMinSize(wxSize(-1, 32));
    root->Add(status_panel, 0, wxEXPAND);
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
    (*reset_button)->SetToolTip("Restore this section from the last selected preset.");
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
    clear_button_->Bind(wxEVT_BUTTON, &HiracoMainFrame::OnClearQueue, this);
    convert_button_->Bind(wxEVT_BUTTON, &HiracoMainFrame::OnConvert, this);
    cancel_button_->Bind(wxEVT_BUTTON, &HiracoMainFrame::OnCancel, this);
    stage1_reset_button_->Bind(wxEVT_BUTTON, &HiracoMainFrame::OnResetStage1Defaults, this);
    stage2_reset_button_->Bind(wxEVT_BUTTON, &HiracoMainFrame::OnResetStage2Defaults, this);
    stage3_reset_button_->Bind(wxEVT_BUTTON, &HiracoMainFrame::OnResetStage3Defaults, this);
    small_preset_button_->Bind(wxEVT_BUTTON,
                               [this](wxCommandEvent&) { ApplyProcessingPreset(ProcessingPreset::kSmall); });
    medium_preset_button_->Bind(wxEVT_BUTTON,
                                [this](wxCommandEvent&) { ApplyProcessingPreset(ProcessingPreset::kMedium); });
    strong_preset_button_->Bind(wxEVT_BUTTON,
                                [this](wxCommandEvent&) { ApplyProcessingPreset(ProcessingPreset::kStrong); });
    custom_preset_button_->Bind(wxEVT_BUTTON,
                                [this](wxCommandEvent&) { ApplyProcessingPreset(ProcessingPreset::kCustom); });
    output_dir_picker_->Bind(wxEVT_DIRPICKER_CHANGED, &HiracoMainFrame::OnOutputDirChanged, this);
    relative_subdir_ctrl_->Bind(wxEVT_TEXT, &HiracoMainFrame::OnRelativeSubdirChanged, this);
    specific_directory_radio_->Bind(wxEVT_RADIOBUTTON, &HiracoMainFrame::OnOutputModeChanged, this);
    next_to_source_radio_->Bind(wxEVT_RADIOBUTTON, &HiracoMainFrame::OnOutputModeChanged, this);
    relative_subdir_radio_->Bind(wxEVT_RADIOBUTTON, &HiracoMainFrame::OnOutputModeChanged, this);
    output_options_pane_->Bind(wxEVT_COLLAPSIBLEPANE_CHANGED,
                               [this](wxCollapsiblePaneEvent& event) {
      event.Skip();
      CallAfter([this]() {
        if (close_requested_.load()) {
          return;
        }
        if (inspector_panel_ != nullptr) {
          inspector_panel_->Layout();
        }
        if (workspace_splitter_ != nullptr) {
          workspace_splitter_->Layout();
        }
        Layout();
        SendSizeEvent();
      });
    });
    compression_choice_->Bind(wxEVT_CHOICE, &HiracoMainFrame::OnCompressionChanged, this);
    original_mode_button_->Bind(wxEVT_BUTTON, [this](wxCommandEvent&) {
      compare_canvas_->SetComparisonMode(CompareCanvas::ComparisonMode::kOriginal);
      UpdateCompareButtons();
      UpdateZoomButtons();
    });
    converted_mode_button_->Bind(wxEVT_BUTTON, [this](wxCommandEvent&) {
      compare_canvas_->SetComparisonMode(CompareCanvas::ComparisonMode::kConverted);
      UpdateCompareButtons();
      UpdateZoomButtons();
    });
    side_by_side_mode_button_->Bind(wxEVT_BUTTON, [this](wxCommandEvent&) {
      compare_canvas_->SetComparisonMode(CompareCanvas::ComparisonMode::kSideBySide);
      UpdateCompareButtons();
      UpdateZoomButtons();
    });
    zoom_out_button_->Bind(wxEVT_BUTTON, [this](wxCommandEvent&) {
      compare_canvas_->ZoomOut();
      UpdateZoomButtons();
    });
    zoom_fit_button_->Bind(wxEVT_BUTTON, [this](wxCommandEvent&) {
      compare_canvas_->SetZoomMode(CompareCanvas::ZoomMode::kFit);
      UpdateZoomButtons();
    });
    zoom_50_button_->Bind(wxEVT_BUTTON, [this](wxCommandEvent&) {
      compare_canvas_->SetZoomMode(CompareCanvas::ZoomMode::k50);
      UpdateZoomButtons();
    });
    zoom_100_button_->Bind(wxEVT_BUTTON, [this](wxCommandEvent&) {
      compare_canvas_->SetZoomMode(CompareCanvas::ZoomMode::k100);
      UpdateZoomButtons();
    });
    zoom_in_button_->Bind(wxEVT_BUTTON, [this](wxCommandEvent&) {
      compare_canvas_->ZoomIn();
      UpdateZoomButtons();
    });
    Bind(wxEVT_MENU, &HiracoMainFrame::OnQuit, this, wxID_EXIT);
    Bind(wxEVT_CLOSE_WINDOW, &HiracoMainFrame::OnCloseWindow, this);
    shutdown_timer_.Bind(wxEVT_TIMER, &HiracoMainFrame::OnShutdownTimer, this);
    Bind(EVT_HIRACO_SELECTION_READY, &HiracoMainFrame::OnSelectionReady, this);
    Bind(EVT_HIRACO_METADATA_READY, &HiracoMainFrame::OnMetadataReady, this);
    Bind(EVT_HIRACO_CONVERTED_PREVIEW_READY, &HiracoMainFrame::OnConvertedPreviewReady, this);
    Bind(EVT_HIRACO_CONVERT_PROGRESS, &HiracoMainFrame::OnConvertProgress, this);

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

  void UpdateCompareButtons() {
    const CompareCanvas::ComparisonMode mode = compare_canvas_->GetComparisonMode();
    auto update = [mode](PaletteButton* button, CompareCanvas::ComparisonMode button_mode) {
      button->SetSelected(mode == button_mode);
    };
    update(original_mode_button_, CompareCanvas::ComparisonMode::kOriginal);
    update(converted_mode_button_, CompareCanvas::ComparisonMode::kConverted);
    update(side_by_side_mode_button_, CompareCanvas::ComparisonMode::kSideBySide);
  }

  void UpdateZoomButtons() {
    const CompareCanvas::ZoomMode mode = compare_canvas_->GetZoomMode();
    const bool side_by_side =
        compare_canvas_->GetComparisonMode() == CompareCanvas::ComparisonMode::kSideBySide;
    auto update = [mode](PaletteButton* button, CompareCanvas::ZoomMode button_mode) {
      button->SetSelected(mode == button_mode);
    };
    update(zoom_fit_button_, CompareCanvas::ZoomMode::kFit);
    update(zoom_50_button_, CompareCanvas::ZoomMode::k50);
    update(zoom_100_button_, CompareCanvas::ZoomMode::k100);
    zoom_out_button_->Enable(!side_by_side);
    zoom_fit_button_->Enable(!side_by_side);
    zoom_50_button_->Enable(!side_by_side);
    zoom_100_button_->Enable(true);
    zoom_in_button_->Enable(!side_by_side);
  }

  void BeginStatusActivity(const wxString& message) {
    if (!activity_indicator_->IsShown()) {
      activity_indicator_->Show();
      activity_indicator_->GetParent()->Layout();
    }
    status_label_->SetLabel(message);
    progress_gauge_->SetValue(0);
    activity_indicator_->Start();
  }

  void FinishStatusActivity(const wxString& message, int progress = 0) {
    activity_indicator_->Stop();
    activity_indicator_->Hide();
    activity_indicator_->GetParent()->Layout();
    progress_gauge_->SetValue(std::clamp(progress, 0, 100));
    status_label_->SetLabel(message);
  }

  void UpdatePresetButtons() {
    const QueueItem* item = SelectedItem();
    const bool has_selection = item != nullptr;
    const auto update = [this, item](PaletteButton* button, ProcessingPreset preset) {
      button->SetSelected(item != nullptr && SettingsMatchPreset(*item, preset));
    };
    update(small_preset_button_, ProcessingPreset::kSmall);
    update(medium_preset_button_, ProcessingPreset::kMedium);
    update(strong_preset_button_, ProcessingPreset::kStrong);
    update(custom_preset_button_, ProcessingPreset::kCustom);
    custom_preset_button_->Enable(has_selection && custom_preset_settings_.has_value() &&
                                  !conversion_running_ && !close_requested_.load());
  }

  bool SettingsEqual(const ResolvedStageSettings& left, const ResolvedStageSettings& right) const {
    // Match the precision the sliders can actually represent.  Preset math
    // can have more precision than the control (for example 0.0972 versus
    // the displayed 0.097), so a raw float comparison would look wrong.
    const auto equal = [](float a, float b, int scale) {
      return std::lround(a * scale) == std::lround(b * scale);
    };
    return equal(left.stage1_psf_sigma, right.stage1_psf_sigma, 100) &&
           equal(left.stage1_nsr, right.stage1_nsr, 1000) &&
           equal(left.stage2_denoise, right.stage2_denoise, 100) &&
           equal(left.stage2_gain1, right.stage2_gain1, 100) &&
           equal(left.stage2_gain2, right.stage2_gain2, 100) &&
           equal(left.stage2_gain3, right.stage2_gain3, 100) &&
           left.stage3_radius == right.stage3_radius &&
           equal(left.stage3_gain, right.stage3_gain, 100);
  }

  ResolvedStageSettings BuiltInPresetSettings(const QueueItem* item, float strength) const {
    const ResolvedStageSettings base = HardcodedSafeStageSettingsForItem(item);
    ResolvedStageSettings settings = base;
    const auto scale_detail = [strength](float value) {
      return std::clamp(1.0f + (value - 1.0f) * strength, 0.25f, 4.0f);
    };
    settings.stage1_nsr = std::clamp(base.stage1_nsr * (1.20f - 0.20f * strength), 0.0f, 0.20f);
    settings.stage2_denoise = std::clamp(base.stage2_denoise + (1.0f - strength) * 0.20f,
                                         0.0f,
                                         1.0f);
    settings.stage2_gain1 = scale_detail(base.stage2_gain1);
    settings.stage2_gain2 = scale_detail(base.stage2_gain2);
    settings.stage2_gain3 = scale_detail(base.stage2_gain3);
    settings.stage3_gain = std::clamp(base.stage3_gain * strength, 0.0f, 4.0f);
    return settings;
  }

  std::optional<ResolvedStageSettings> SettingsForPreset(const QueueItem* item,
                                                          ProcessingPreset preset) const {
    switch (preset) {
      case ProcessingPreset::kSmall:
        return BuiltInPresetSettings(item, 0.60f);
      case ProcessingPreset::kMedium:
        return BuiltInPresetSettings(item, 1.00f);
      case ProcessingPreset::kStrong:
        return BuiltInPresetSettings(item, 1.45f);
      case ProcessingPreset::kCustom:
        return custom_preset_settings_;
      case ProcessingPreset::kNone:
        return std::nullopt;
    }
    return std::nullopt;
  }

  ResolvedStageSettings ResolvedSettingsForItem(const QueueItem& item) const {
    if (item.prepared.has_value()) {
      return GetResolvedStageSettings(*item.prepared, ResolveEffectiveStageOverrides(item.stage_overrides));
    }
    return ResolveDisplayStageSettings(ResolveEffectiveStageOverrides(item.stage_overrides));
  }

  bool SettingsMatchPreset(const QueueItem& item, ProcessingPreset preset) const {
    const std::optional<ResolvedStageSettings> preset_settings = SettingsForPreset(&item, preset);
    return preset_settings.has_value() && SettingsEqual(ResolvedSettingsForItem(item), *preset_settings);
  }

  bool SettingsMatchAnyBuiltInPreset(const QueueItem& item) const {
    return SettingsMatchPreset(item, ProcessingPreset::kSmall) ||
           SettingsMatchPreset(item, ProcessingPreset::kMedium) ||
           SettingsMatchPreset(item, ProcessingPreset::kStrong);
  }

  ResolvedStageSettings LastPresetSettings(const QueueItem* item) const {
    if (item != nullptr) {
      const std::optional<ResolvedStageSettings> selected = SettingsForPreset(item, item->last_preset);
      if (selected.has_value()) {
        return *selected;
      }
    }
    return BuiltInPresetSettings(item, 0.60f);
  }

  void ApplyProcessingPresetToRow(int row, ProcessingPreset preset, const wxString& label) {
    if (row < 0 || row >= static_cast<int>(queue_.size())) {
      return;
    }
    QueueItem* item = &queue_[row];
    const std::optional<ResolvedStageSettings> settings = SettingsForPreset(item, preset);
    if (!settings.has_value()) {
      return;
    }
    item->stage_overrides = MakeExplicitStageOverrides(*settings);
    item->last_preset = preset;
    NormalizeStageOverrides(item);
    UpdatePresetButtons();
    RefreshQueueRow(row);
    if (row == selected_row_) {
      RefreshConvertedPreviewIfPossible();
    }
    status_label_->SetLabel("Applied " + label + " preset");
  }

  void ApplyProcessingPreset(ProcessingPreset preset) {
    const wxString label = preset == ProcessingPreset::kSmall ? "Small" :
                           preset == ProcessingPreset::kMedium ? "Medium" :
                           preset == ProcessingPreset::kStrong ? "Strong" : "Custom";
    ApplyProcessingPresetToRow(selected_row_, preset, label);
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
    UpdatePresetButtons();
  }

  void UpdateResolvedSliderValues() {
    const QueueItem* item = SelectedItem();
    UpdatePresetButtons();
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
    std::vector<int> rows(selected_rows_.begin(), selected_rows_.end());
    if (rows.empty() && selected_row_ >= 0 && selected_row_ < static_cast<int>(queue_.size())) {
      rows.push_back(selected_row_);
    }
    return rows;
  }

  void RefreshConvertedPreviewIfPossible() {
    UpdateResolvedSliderValues();
    if (selected_row_ >= 0 &&
        selected_row_ < static_cast<int>(queue_.size()) &&
        queue_[selected_row_].prepared.has_value()) {
      ScheduleConvertedPreview();
    }
  }

  void InvalidateSelectionRequests() {
    if (selection_cancel_) {
      selection_cancel_->store(true);
    }
    ++selection_request_id_;
  }

  void InvalidateConvertedPreviewRequests() {
    if (converted_preview_cancel_) {
      converted_preview_cancel_->store(true);
    }
    ++converted_preview_request_id_;
    converted_preview_queued_ = false;
  }

  void StartQueuedConvertedPreviewIfIdle() {
    if (!converted_preview_queued_ || converted_preview_worker_running_) {
      return;
    }
    StartConvertedPreviewWorker();
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

  static double NormalizeConvertedPreviewProgress(const ProcessingProgress& progress) {
    const double fraction = std::clamp(progress.fraction, 0.0, 1.0);
    if (progress.phase == "cache") {
      return 0.60 * fraction;
    }
    if (progress.phase == "enhance") {
      return 0.60 + 0.30 * fraction;
    }
    if (progress.phase == "preview") {
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

  void BindSlider(SliderControl& control, std::function<void(double)> on_change) {
    auto apply_change = [this, &control, on_change](bool request_preview) {
      const double value = SliderValue(control);
      UpdateSliderLabel(control);
      if (!updating_sliders_) {
        on_change(value);
        if (request_preview &&
            control.last_preview_request_value != control.slider->GetValue()) {
          control.last_preview_request_value = control.slider->GetValue();
          ScheduleConvertedPreview();
        }
      }
    };

    auto begin_drag = [this, &control]() {
      if (control.dragging) {
        return;
      }
      control.dragging = true;
      // Do not spend CPU finishing a preview for a value the user is actively
      // changing.  The final thumb position will submit a replacement.
      control.last_preview_request_value.reset();
      InvalidateConvertedPreviewRequests();
      FinishStatusActivity("Adjust settings, then release to update preview");
    };
    auto finish_drag = [&control, apply_change]() {
      if (!control.dragging) {
        return;
      }
      control.dragging = false;
      apply_change(true);
    };
    auto apply_non_drag_change = [&control, apply_change]() {
      apply_change(!control.dragging);
    };

    // wxEVT_SLIDER is emitted for every value change, including every
    // thumb-track update.  Keep it as a value/settings notification only;
    // preview scheduling is driven by the semantic scroll events below.
    control.slider->Bind(wxEVT_SLIDER,
                         [apply_change](wxCommandEvent&) { apply_change(false); });
    control.slider->Bind(wxEVT_SCROLL_THUMBTRACK,
                         [begin_drag, apply_change](wxScrollEvent&) {
                           begin_drag();
                           apply_change(false);
                         });
    control.slider->Bind(wxEVT_SCROLL_THUMBRELEASE,
                         [finish_drag](wxScrollEvent&) { finish_drag(); });
    control.slider->Bind(wxEVT_SCROLL_CHANGED,
                         [apply_non_drag_change](wxScrollEvent&) { apply_non_drag_change(); });
    control.slider->Bind(wxEVT_SCROLL_LINEUP,
                         [apply_non_drag_change](wxScrollEvent&) { apply_non_drag_change(); });
    control.slider->Bind(wxEVT_SCROLL_LINEDOWN,
                         [apply_non_drag_change](wxScrollEvent&) { apply_non_drag_change(); });
    control.slider->Bind(wxEVT_SCROLL_PAGEUP,
                         [apply_non_drag_change](wxScrollEvent&) { apply_non_drag_change(); });
    control.slider->Bind(wxEVT_SCROLL_PAGEDOWN,
                         [apply_non_drag_change](wxScrollEvent&) { apply_non_drag_change(); });
    control.slider->Bind(wxEVT_SCROLL_TOP,
                         [apply_non_drag_change](wxScrollEvent&) { apply_non_drag_change(); });
    control.slider->Bind(wxEVT_SCROLL_BOTTOM,
                         [apply_non_drag_change](wxScrollEvent&) { apply_non_drag_change(); });
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
    config->Read("ui/output_mode", &output_mode);
    long output_mode_version = 0;
    config->Read("ui/output_mode_version", &output_mode_version);

    wxString output_dir;
    if (config->Read("ui/output_dir", &output_dir) && !output_dir.empty()) {
      base_output_dir_ = PathFromWxString(output_dir);
    }

    wxString relative_subdir;
    if (config->Read("ui/relative_subdir", &relative_subdir)) {
      relative_subdir_ = PathFromWxString(relative_subdir);
    }

    if (output_mode_version >= 2 && output_mode == 1) {
      output_location_mode_ = OutputLocationMode::kNextToOriginal;
    } else if (output_mode == 2) {
      output_location_mode_ = OutputLocationMode::kSubfolderUnderOriginal;
    } else if (output_mode == 1) {
      // Version-one settings represented both source-relative modes with one
      // radio button and an optional subfolder field.
      output_location_mode_ = relative_subdir_.empty()
          ? OutputLocationMode::kNextToOriginal
          : OutputLocationMode::kSubfolderUnderOriginal;
    } else {
      output_location_mode_ = OutputLocationMode::kSpecificDirectory;
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
    long output_mode = 0;
    if (output_location_mode_ == OutputLocationMode::kNextToOriginal) {
      output_mode = 1;
    } else if (output_location_mode_ == OutputLocationMode::kSubfolderUnderOriginal) {
      output_mode = 2;
    }
    config->Write("ui/output_mode", output_mode);
    config->Write("ui/output_mode_version", 2L);
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

  std::filesystem::path ResolveOutputPathForMode(const std::string& source_path) const {
    const std::filesystem::path source(source_path);
    std::filesystem::path output_directory;
    if (output_location_mode_ == OutputLocationMode::kSpecificDirectory) {
      output_directory = base_output_dir_;
    } else if (output_location_mode_ == OutputLocationMode::kNextToOriginal) {
      output_directory = source.parent_path();
    } else {
      output_directory = source.parent_path() / relative_subdir_;
    }
    return output_directory / (source.stem().string() + ".dng");
  }

  void UpdateOutputLocationControls() {
    const bool specific_directory_mode = output_location_mode_ == OutputLocationMode::kSpecificDirectory;
    const bool subfolder_mode = output_location_mode_ == OutputLocationMode::kSubfolderUnderOriginal;
    const bool controls_enabled = !conversion_running_ && !close_requested_.load();
    specific_directory_radio_->SetValue(specific_directory_mode);
    next_to_source_radio_->SetValue(output_location_mode_ == OutputLocationMode::kNextToOriginal);
    relative_subdir_radio_->SetValue(subfolder_mode);
    output_dir_picker_->Enable(controls_enabled && specific_directory_mode);
    relative_subdir_ctrl_->Enable(controls_enabled && subfolder_mode);
  }

  void UpdateButtons() {
    const bool disabled = close_requested_.load();
    const bool has_selection = selected_row_ >= 0 && selected_row_ < static_cast<int>(queue_.size());
    const bool enable_file_settings = has_selection && !conversion_running_ && !disabled;

    add_files_button_->Enable(!conversion_running_ && !disabled);
    clear_button_->Enable(!queue_.empty() && !conversion_running_ && !disabled);
    convert_button_->Enable(!queue_.empty() && !conversion_running_ && !disabled);
    cancel_button_->Enable(conversion_running_ && !disabled);
    stage1_reset_button_->Enable(enable_file_settings);
    stage2_reset_button_->Enable(enable_file_settings);
    stage3_reset_button_->Enable(enable_file_settings);
    for (wxButton* button : thumbnail_reset_buttons_) {
      button->Enable(!conversion_running_ && !disabled);
    }
    small_preset_button_->Enable(enable_file_settings);
    medium_preset_button_->Enable(enable_file_settings);
    strong_preset_button_->Enable(enable_file_settings);
    stage1_sigma_.slider->Enable(enable_file_settings);
    stage1_nsr_.slider->Enable(enable_file_settings);
    stage2_denoise_.slider->Enable(enable_file_settings);
    stage2_gain1_.slider->Enable(enable_file_settings);
    stage2_gain2_.slider->Enable(enable_file_settings);
    stage2_gain3_.slider->Enable(enable_file_settings);
    stage3_radius_.slider->Enable(enable_file_settings);
    stage3_gain_.slider->Enable(enable_file_settings);
    UpdateOutputLocationControls();
    UpdatePresetButtons();
  }

  void RefreshQueue() {
    if (queue_scroll_ == nullptr || queue_sizer_ == nullptr) {
      return;
    }

    queue_scroll_->Freeze();
    queue_sizer_->Clear(true);
    thumbnail_reset_buttons_.clear();
    for (size_t index = 0; index < queue_.size(); ++index) {
      const QueueItem& item = queue_[index];
      auto* card = new wxPanel(queue_scroll_, wxID_ANY, wxDefaultPosition, wxDefaultSize, wxBORDER_SIMPLE);
      const bool selected = selected_rows_.count(static_cast<int>(index)) != 0;
      card->SetBackgroundColour(selected ? kQueueSelectedSurface : kQueueSurface);
      card->SetToolTip(item.source_path);
      auto* card_sizer = new wxBoxSizer(wxVERTICAL);

      auto* header = new wxBoxSizer(wxHORIZONTAL);
      auto* name = new wxStaticText(card,
                                    wxID_ANY,
                                    std::filesystem::path(item.source_path).filename().string());
      wxFont name_font = name->GetFont();
      name_font.MakeBold();
      name->SetFont(name_font);
      header->Add(name, 1, wxALIGN_CENTER_VERTICAL | wxRIGHT, 6);
      auto* remove = new wxBitmapButton(card,
                                        wxID_ANY,
                                        wxArtProvider::GetBitmap(wxART_DELETE, wxART_BUTTON, wxSize(16, 16)));
      remove->SetToolTip("Remove this image");
      remove->SetMinSize(wxSize(28, 28));
      auto* reset_settings = new wxButton(card, wxID_ANY, "Reset settings");
      reset_settings->SetToolTip("Reset this image to the Small preset.");
      thumbnail_reset_buttons_.push_back(reset_settings);
      header->Add(reset_settings, 0, wxRIGHT, 5);
      header->Add(remove, 0);
      card_sizer->Add(header, 0, wxALL | wxEXPAND, 7);

      std::shared_ptr<const PreviewImage> cached_preview;
      if (item.prepared.has_value()) {
        if (!TryGetCachedOriginalPreview(*item.prepared, {}, &cached_preview)) {
          cached_preview.reset();
        }
      }
      const wxBitmap thumbnail = MakeThumbnailBitmap(cached_preview, 280, 150);
      wxWindow* preview_window = nullptr;
      if (thumbnail.IsOk()) {
        auto* image = new wxStaticBitmap(card, wxID_ANY, thumbnail);
        card_sizer->Add(image, 0, wxLEFT | wxRIGHT | wxBOTTOM | wxALIGN_CENTER_HORIZONTAL, 7);
        preview_window = image;
      } else {
        auto* placeholder = new wxPanel(card, wxID_ANY, wxDefaultPosition, wxSize(280, 118), wxBORDER_SIMPLE);
        placeholder->SetBackgroundColour(kControlSurface);
        auto* placeholder_sizer = new wxBoxSizer(wxVERTICAL);
        auto* placeholder_text = new wxStaticText(
            placeholder,
            wxID_ANY,
            "Preview will appear here");
        placeholder_text->SetForegroundColour(kControlMutedText);
        placeholder_sizer->AddStretchSpacer();
        placeholder_sizer->Add(placeholder_text, 0, wxALIGN_CENTER | wxALL, 4);
        placeholder_sizer->AddStretchSpacer();
        placeholder->SetSizer(placeholder_sizer);
        card_sizer->Add(placeholder, 0, wxLEFT | wxRIGHT | wxBOTTOM | wxEXPAND, 7);
        preview_window = placeholder;
      }

      wxString detail = item.resolution_label == "..." ? wxString() : item.resolution_label;
      if (item.prepared.has_value() && item.prepared->HasHighlightRecoverySource()) {
        detail += item.enable_highlight_recovery ? "  ·  HL Rec On" : "  ·  HL Rec Off";
      }
      wxStaticText* detail_label = nullptr;
      if (!detail.empty()) {
        detail_label = new wxStaticText(card, wxID_ANY, detail);
        detail_label->SetForegroundColour(kControlMutedText);
        card_sizer->Add(detail_label, 0, wxLEFT | wxRIGHT | wxBOTTOM | wxEXPAND, 7);
      }
      wxCheckBox* highlight_recovery = nullptr;
      if (item.prepared.has_value() && item.prepared->HasHighlightRecoverySource()) {
        highlight_recovery = new wxCheckBox(card, wxID_ANY, "ORI highlight recovery");
        highlight_recovery->SetValue(item.enable_highlight_recovery);
        card_sizer->Add(highlight_recovery, 0, wxLEFT | wxRIGHT | wxBOTTOM | wxEXPAND, 7);
      }
      card->SetSizer(card_sizer);

      auto select = [this, row = static_cast<int>(index)](wxMouseEvent& event) {
        SelectThumbnailRow(row, event.CmdDown() || event.ControlDown());
      };
      card->Bind(wxEVT_LEFT_DOWN, select);
      name->Bind(wxEVT_LEFT_DOWN, select);
      if (detail_label != nullptr) {
        detail_label->Bind(wxEVT_LEFT_DOWN, select);
      }
      preview_window->Bind(wxEVT_LEFT_DOWN, select);
      preview_window->Bind(wxEVT_RIGHT_DOWN,
                           [this, preview_window, row = static_cast<int>(index)](wxMouseEvent& event) {
        ShowThumbnailContextMenu(preview_window, row, event.GetPosition());
      });
      if (highlight_recovery != nullptr) {
        highlight_recovery->Bind(wxEVT_CHECKBOX, [this, row = static_cast<int>(index)](wxCommandEvent& event) {
          if (row < 0 || row >= static_cast<int>(queue_.size()) || !queue_[row].prepared.has_value()) {
            return;
          }
          QueueItem& queue_item = queue_[row];
          queue_item.enable_highlight_recovery = event.IsChecked();
          queue_item.prepared->enable_highlight_recovery = queue_item.enable_highlight_recovery;
          if (row == selected_row_) {
            ScheduleConvertedPreview();
          }
          RefreshQueue();
        });
      }
      remove->Bind(wxEVT_BUTTON, [this, row = static_cast<int>(index)](wxCommandEvent&) {
        RemoveQueueRows({row});
      });
      reset_settings->Bind(wxEVT_BUTTON, [this, row = static_cast<int>(index)](wxCommandEvent&) {
        ApplyProcessingPresetToRow(row, ProcessingPreset::kSmall, "Small");
      });

      queue_sizer_->Add(card, 0, wxBOTTOM | wxEXPAND, 8);
    }
    queue_scroll_->FitInside();
    queue_scroll_->Layout();
    queue_scroll_->Thaw();
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
    RefreshQueue();
  }

  void NotePreviewCacheUse(int index) {
    if (index < 0 || index >= static_cast<int>(queue_.size())) {
      return;
    }
    queue_[index].preview_cache_access_sequence = ++preview_cache_access_sequence_;
  }

  void EnforcePreviewCacheBudget() {
    size_t total_bytes = 0;
    for (const QueueItem& item : queue_) {
      if (item.prepared.has_value()) {
        total_bytes += GetPreviewCacheBytes(*item.prepared);
      }
    }

    while (total_bytes > kPreviewCacheBudgetBytes) {
      int victim = -1;
      uint64_t oldest_access = 0;
      for (size_t index = 0; index < queue_.size(); ++index) {
        const QueueItem& item = queue_[index];
        if (static_cast<int>(index) == selected_row_ || !item.prepared.has_value() ||
            item.preview_cache_access_sequence == 0) {
          continue;
        }
        if (victim < 0 || item.preview_cache_access_sequence < oldest_access) {
          victim = static_cast<int>(index);
          oldest_access = item.preview_cache_access_sequence;
        }
      }
      if (victim < 0) {
        return;
      }

      const size_t bytes_before = GetPreviewCacheBytes(*queue_[victim].prepared);
      ReleasePreviewProcessingCache(&*queue_[victim].prepared);
      const size_t bytes_after = GetPreviewCacheBytes(*queue_[victim].prepared);
      queue_[victim].preview_cache_access_sequence = 0;
      total_bytes -= std::min(total_bytes, bytes_before - std::min(bytes_before, bytes_after));
    }
  }

  void SelectThumbnailRow(int row, bool toggle_selection) {
    if (row < 0 || row >= static_cast<int>(queue_.size())) {
      return;
    }
    if (toggle_selection) {
      if (selected_rows_.count(row) != 0) {
        selected_rows_.erase(row);
        if (selected_row_ == row) {
          selected_row_ = selected_rows_.empty() ? -1 : *selected_rows_.rbegin();
        }
      } else {
        selected_rows_.insert(row);
        selected_row_ = row;
      }
    } else {
      selected_rows_.clear();
      selected_rows_.insert(row);
      selected_row_ = row;
    }
    RefreshQueue();
    UpdateButtons();
    if (selected_row_ >= 0) {
      StartSelectionLoad();
    }
  }

  void RemoveQueueRows(std::vector<int> rows) {
    if (rows.empty()) {
      return;
    }
    InvalidateConvertedPreviewRequests();
    std::sort(rows.begin(), rows.end());
    rows.erase(std::unique(rows.begin(), rows.end()), rows.end());
    for (auto it = rows.rbegin(); it != rows.rend(); ++it) {
      if (*it >= 0 && *it < static_cast<int>(queue_.size())) {
        queue_.erase(queue_.begin() + *it);
      }
    }
    selected_rows_.clear();
    selected_row_ = -1;
    RefreshQueue();
    if (!queue_.empty()) {
      SelectRow(0);
    } else {
      compare_canvas_->SetOriginalPreview(nullptr);
      compare_canvas_->SetConvertedPreview(nullptr);
      UpdateResolvedSliderValues();
      FinishStatusActivity("Ready");
    }
  }

  void BeginMetadataProbe(uint64_t item_id, const std::string& source_path) {
    LaunchWorker(WorkerPriority::kBackground, [this, item_id, source_path]() {
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
      selected_rows_.clear();
      return;
    }
    selected_row_ = row;
    selected_rows_.clear();
    selected_rows_.insert(row);
    RefreshQueue();
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
    InvalidateConvertedPreviewRequests();

    if (item_copy.prepared.has_value()) {
      std::shared_ptr<const PreviewImage> cached_preview;
      if (TryGetCachedOriginalPreview(*item_copy.prepared, {}, &cached_preview)) {
        compare_canvas_->SetOriginalPreview(
            MakeComparisonOriginalPreview(cached_preview, *item_copy.prepared));
        compare_canvas_->SetConvertedPreview(nullptr);
        queue_[selected_row_].resolution_label = ResolutionLabelForPrepared(*item_copy.prepared);
        RefreshQueueRow(selected_row_);
        status_label_->SetLabel("Original preview ready. Rendering converted preview...");
        UpdateResolvedSliderValues();
        UpdateButtons();
        NotePreviewCacheUse(selected_row_);
        ScheduleConvertedPreview();
        return;
      }
    }

    const uint64_t request_id = selection_request_id_;
    selection_cancel_ = std::make_shared<std::atomic_bool>(false);
    compare_canvas_->SetOriginalPreview(nullptr);
    compare_canvas_->SetConvertedPreview(nullptr);
    BeginStatusActivity("Loading original preview...");

    LaunchWorker(WorkerPriority::kInteractive, [this, item_copy, item_id, request_id, cancel_token = selection_cancel_]() {
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

      prepared.enable_highlight_recovery = item_copy.enable_highlight_recovery;

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
      auto* event = new wxThreadEvent(EVT_HIRACO_SELECTION_READY);
      event->SetPayload(payload);
      wxQueueEvent(this, event);
    });
  }

  void ScheduleConvertedPreview() {
    if (close_requested_.load() ||
        selected_row_ < 0 || selected_row_ >= static_cast<int>(queue_.size())) {
      return;
    }
    ++converted_preview_request_id_;
    converted_preview_queued_ = true;
    if (converted_preview_worker_running_ && converted_preview_cancel_) {
      converted_preview_cancel_->store(true);
    }
    StartQueuedConvertedPreviewIfIdle();
  }

  void StartConvertedPreviewWorker() {
    if (close_requested_.load() ||
        selected_row_ < 0 || selected_row_ >= static_cast<int>(queue_.size())) {
      return;
    }
    QueueItem& item = queue_[selected_row_];
    if (!item.prepared.has_value()) {
      return;
    }

    converted_preview_queued_ = false;
    converted_preview_worker_running_ = true;
    const uint64_t request_id = converted_preview_request_id_;
    active_converted_preview_request_id_ = request_id;
    if (converted_preview_cancel_) {
      converted_preview_cancel_->store(true);
    }
    converted_preview_cancel_ = std::make_shared<std::atomic_bool>(false);
    const uint64_t item_id = item.id;
    PreparedSource prepared = *item.prepared;
    prepared.enable_highlight_recovery = item.enable_highlight_recovery;
    const StageOverrideSet stage_overrides = ResolveEffectiveStageOverrides(item.stage_overrides);
    BeginStatusActivity("Rendering converted preview...");

    LaunchWorker(WorkerPriority::kInteractive, [this, item_id, request_id, prepared, stage_overrides,
                  cancel_token = converted_preview_cancel_]() mutable {
      ConvertedPreviewReadyPayload payload;
      payload.item_id = item_id;
      payload.request_id = request_id;
      payload.converted_preview = std::make_shared<PreviewImage>();

      if (cancel_token->load()) {
        payload.error = "operation canceled";
        auto* event = new wxThreadEvent(EVT_HIRACO_CONVERTED_PREVIEW_READY);
        event->SetPayload(payload);
        wxQueueEvent(this, event);
        return;
      }

      auto report_progress = [this, item_id, request_id](const ProcessingProgress& progress) {
        const double normalized = NormalizeConvertedPreviewProgress(progress);
        const std::string message = progress.message;
        CallAfter([this, item_id, request_id, normalized, message]() {
          if (close_requested_.load() || conversion_running_ ||
              request_id != converted_preview_request_id_) {
            return;
          }
          progress_gauge_->SetValue(static_cast<int>(std::round(std::clamp(normalized, 0.0, 1.0) * 100.0)));
          status_label_->SetLabel(message);
        });
      };

      std::string error;
      if (!RenderConvertedFullPreview(&prepared,
                                      stage_overrides,
                                      kFullPreviewMaxDimension,
                                      payload.converted_preview,
                                      {},
                                      report_progress,
                                      [cancel_token]() { return cancel_token->load(); },
                                      &error)) {
        payload.ok = false;
        payload.error = error;
      } else {
        payload.ok = true;
      }

      auto* event = new wxThreadEvent(EVT_HIRACO_CONVERTED_PREVIEW_READY);
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
    if (converted_preview_cancel_) {
      converted_preview_cancel_->store(true);
    }
  }

  void BeginCloseRequest() {
    if (close_requested_.exchange(true)) {
      return;
    }

    RequestBackgroundCancel();
    const size_t discarded_tasks = processing_tasks_.DiscardPending();
    if (discarded_tasks != 0) {
      active_workers_.fetch_sub(static_cast<int>(discarded_tasks));
    }
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

  void OnClearQueue(wxCommandEvent&) {
    InvalidateConvertedPreviewRequests();
    queue_.clear();
    selected_row_ = -1;
    selected_rows_.clear();
    compare_canvas_->SetOriginalPreview(nullptr);
    compare_canvas_->SetConvertedPreview(nullptr);
    UpdateResolvedSliderValues();
    FinishStatusActivity("Ready");
    RefreshQueue();
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
    if (specific_directory_radio_->GetValue()) {
      output_location_mode_ = OutputLocationMode::kSpecificDirectory;
    } else if (relative_subdir_radio_->GetValue()) {
      output_location_mode_ = OutputLocationMode::kSubfolderUnderOriginal;
    } else {
      output_location_mode_ = OutputLocationMode::kNextToOriginal;
    }
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
    compare_canvas_->SetZoomMode(CompareCanvas::ZoomMode::kFit);
    UpdateZoomButtons();
  }

  void OnResetStage1Defaults(wxCommandEvent&) {
    QueueItem* item = SelectedItem();
    if (StageOverrideSet* overrides = SelectedStageOverrides()) {
      const ResolvedStageSettings preset = LastPresetSettings(item);
      overrides->stage1_psf_sigma = preset.stage1_psf_sigma;
      overrides->stage1_nsr = preset.stage1_nsr;
      NormalizeStageOverrides(item);
      UpdatePresetButtons();
      RefreshQueueRow(selected_row_);
    }
    RefreshConvertedPreviewIfPossible();
  }

  void OnResetStage2Defaults(wxCommandEvent&) {
    QueueItem* item = SelectedItem();
    if (StageOverrideSet* overrides = SelectedStageOverrides()) {
      const ResolvedStageSettings preset = LastPresetSettings(item);
      overrides->stage2_denoise = preset.stage2_denoise;
      overrides->stage2_gain1 = preset.stage2_gain1;
      overrides->stage2_gain2 = preset.stage2_gain2;
      overrides->stage2_gain3 = preset.stage2_gain3;
      NormalizeStageOverrides(item);
      UpdatePresetButtons();
      RefreshQueueRow(selected_row_);
    }
    RefreshConvertedPreviewIfPossible();
  }

  void OnResetStage3Defaults(wxCommandEvent&) {
    QueueItem* item = SelectedItem();
    if (StageOverrideSet* overrides = SelectedStageOverrides()) {
      const ResolvedStageSettings preset = LastPresetSettings(item);
      overrides->stage3_radius = preset.stage3_radius;
      overrides->stage3_gain = preset.stage3_gain;
      NormalizeStageOverrides(item);
      UpdatePresetButtons();
      RefreshQueueRow(selected_row_);
    }
    RefreshConvertedPreviewIfPossible();
  }

  void CopySettingsFromRow(int row) {
    if (row < 0 || row >= static_cast<int>(queue_.size())) {
      return;
    }
    copied_stage_overrides_ = MakeExplicitStageOverrides(ResolvedSettingsForItem(queue_[row]));
    status_label_->SetLabel("Copied settings");
    UpdateButtons();
  }

  void PasteSettingsToRows(const std::vector<int>& rows, const wxString& message) {
    if (!copied_stage_overrides_.has_value()) {
      return;
    }
    if (rows.empty()) {
      return;
    }

    bool current_row_updated = false;
    for (const int row : rows) {
      if (row < 0 || row >= static_cast<int>(queue_.size())) {
        continue;
      }
      queue_[row].stage_overrides = *copied_stage_overrides_;
      NormalizeStageOverrides(&queue_[row]);
      current_row_updated = current_row_updated || row == selected_row_;
    }

    RefreshQueue();

    if (current_row_updated) {
      RefreshConvertedPreviewIfPossible();
    }

    status_label_->SetLabel(message);
    UpdatePresetButtons();
    UpdateButtons();
  }

  void SetCustomPresetFromRow(int row) {
    if (row < 0 || row >= static_cast<int>(queue_.size()) ||
        SettingsMatchAnyBuiltInPreset(queue_[row])) {
      return;
    }
    custom_preset_settings_ = ResolvedSettingsForItem(queue_[row]);
    queue_[row].last_preset = ProcessingPreset::kCustom;
    UpdatePresetButtons();
    status_label_->SetLabel("Set Custom preset");
  }

  void ShowThumbnailContextMenu(wxWindow* thumbnail, int row, const wxPoint& position) {
    if (row < 0 || row >= static_cast<int>(queue_.size()) || conversion_running_ || close_requested_.load()) {
      return;
    }

    wxMenu menu;
    const int copy_id = wxWindow::NewControlId();
    const int paste_id = wxWindow::NewControlId();
    const int paste_all_id = wxWindow::NewControlId();
    const int custom_id = wxWindow::NewControlId();
    menu.Append(copy_id, "Copy settings");
    const bool can_paste = copied_stage_overrides_.has_value();
    if (can_paste) {
      menu.Append(paste_id, "Paste settings");
      menu.Append(paste_all_id, "Paste to all images");
    }
    if (!SettingsMatchAnyBuiltInPreset(queue_[row])) {
      menu.AppendSeparator();
      menu.Append(custom_id, "Set as Custom preset");
    }
    menu.Bind(wxEVT_MENU, [this, row, copy_id, paste_id, paste_all_id, custom_id](wxCommandEvent& event) {
      if (event.GetId() == copy_id) {
        CopySettingsFromRow(row);
      } else if (event.GetId() == paste_id) {
        PasteSettingsToRows({row}, "Pasted settings");
      } else if (event.GetId() == paste_all_id) {
        std::vector<int> rows;
        rows.reserve(queue_.size());
        for (size_t index = 0; index < queue_.size(); ++index) {
          rows.push_back(static_cast<int>(index));
        }
        PasteSettingsToRows(rows, wxString::Format("Pasted settings to %zu images", rows.size()));
      } else if (event.GetId() == custom_id) {
        SetCustomPresetFromRow(row);
      }
    });
    thumbnail->PopupMenu(&menu, position);
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
      queue_[index].prepared->enable_highlight_recovery = queue_[index].enable_highlight_recovery;
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
      RefreshQueueRow(index);
      FinishStatusActivity(payload.error);
      return;
    }

    queue_[index].prepared = payload.prepared;
    queue_[index].prepared->enable_highlight_recovery = queue_[index].enable_highlight_recovery;
    queue_[index].resolution_label = ResolutionLabelForPrepared(payload.prepared);
    RefreshQueueRow(index);
    UpdateButtons();

    if (index >= 0 && index == selected_row_) {
      compare_canvas_->SetOriginalPreview(
          MakeComparisonOriginalPreview(payload.original_preview, payload.prepared));
      compare_canvas_->SetConvertedPreview(nullptr);
      status_label_->SetLabel("Original preview ready. Rendering converted preview...");
      UpdateResolvedSliderValues();
      ScheduleConvertedPreview();
    }
  }

  void OnConvertedPreviewReady(wxThreadEvent& event) {
    const ConvertedPreviewReadyPayload payload = event.GetPayload<ConvertedPreviewReadyPayload>();
    if (payload.request_id == active_converted_preview_request_id_) {
      converted_preview_worker_running_ = false;
      active_converted_preview_request_id_ = 0;
    }

    if (close_requested_.load()) {
      return;
    }

    if (conversion_running_) {
      return;
    }

    if (payload.request_id != converted_preview_request_id_) {
      StartQueuedConvertedPreviewIfIdle();
      return;
    }

    if (!payload.ok) {
      FinishStatusActivity(payload.error);
      StartQueuedConvertedPreviewIfIdle();
      return;
    }

    const int index = FindItemIndex(payload.item_id);
    if (index >= 0 && index == selected_row_) {
      compare_canvas_->SetConvertedPreview(payload.converted_preview);
      NotePreviewCacheUse(index);
      EnforcePreviewCacheBudget();
      FinishStatusActivity("Ready");
    }
    StartQueuedConvertedPreviewIfIdle();
  }

  void OnConvert(wxCommandEvent&) {
    if (close_requested_.load() || conversion_running_ || queue_.empty()) {
      return;
    }

    InvalidateSelectionRequests();
    InvalidateConvertedPreviewRequests();

    // A full-resolution export needs substantially more transient memory than
    // a display preview. The canvas retains its already displayed bitmap, but
    // the recomputable raw/display caches are no longer useful once conversion
    // starts. Releasing them here leaves the export headroom instead of keeping
    // up to the GUI LRU budget resident alongside it.
    for (QueueItem& item : queue_) {
      if (item.prepared.has_value()) {
        ReleasePreviewProcessingCache(&*item.prepared);
        item.preview_cache_access_sequence = 0;
      }
    }
    conversion_running_ = true;
    conversion_cancel_.store(false);
    overwrite_policy_ = OverwritePolicy::kAsk;
    BeginStatusActivity("Converting...");
    UpdateButtons();

    const std::vector<QueueItem> queue_snapshot = queue_;

    LaunchWorker(WorkerPriority::kBackground, [this, queue_snapshot]() {
      for (size_t item_index = 0; item_index < queue_snapshot.size(); ++item_index) {
        if (conversion_cancel_.load()) {
          break;
        }

        const QueueItem& item = queue_snapshot[item_index];
        auto* progress_event = new wxThreadEvent(EVT_HIRACO_CONVERT_PROGRESS);
        progress_event->SetPayload(ConvertProgressPayload{static_cast<double>(item_index) / std::max<size_t>(1, queue_snapshot.size()),
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
            auto* error_event = new wxThreadEvent(EVT_HIRACO_CONVERT_PROGRESS);
            error_event->SetPayload(ConvertProgressPayload{
                static_cast<double>(item_index + 1) / std::max<size_t>(1, queue_snapshot.size()),
                error});
            wxQueueEvent(this, error_event);
            continue;
          }
        }
        prepared.enable_highlight_recovery = item.enable_highlight_recovery;

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
            break;
          }
          if (!decision.should_write) {
            auto* skipped_event = new wxThreadEvent(EVT_HIRACO_CONVERT_PROGRESS);
            skipped_event->SetPayload(ConvertProgressPayload{
                static_cast<double>(item_index + 1) / std::max<size_t>(1, queue_snapshot.size()),
                "Skipped existing target"});
            wxQueueEvent(this, skipped_event);
            continue;
          }
        }

        DngWriteResult result = ConvertToDng(prepared,
                     item.target_path,
                     compression_,
                     ResolveEffectiveStageOverrides(item.stage_overrides),
                                             {},
                                             [this, item_index, total = queue_snapshot.size()](const ProcessingProgress& progress) {
                                               auto* event = new wxThreadEvent(EVT_HIRACO_CONVERT_PROGRESS);
                                               const double overall =
                                                   (static_cast<double>(item_index) + NormalizeConvertProgress(progress)) /
                                                   std::max<size_t>(1, total);
                                               event->SetPayload(ConvertProgressPayload{overall, progress.message});
                                               wxQueueEvent(this, event);
                                             },
                                             [this]() { return conversion_cancel_.load(); });

        auto* completed_event = new wxThreadEvent(EVT_HIRACO_CONVERT_PROGRESS);
        completed_event->SetPayload(ConvertProgressPayload{
            static_cast<double>(item_index + 1) / std::max<size_t>(1, queue_snapshot.size()),
            result.ok ? "Converted file" : result.message});
        wxQueueEvent(this, completed_event);

      }

      CallAfter([this]() {
        conversion_running_ = false;
        UpdateButtons();
        if (conversion_cancel_.load()) {
          FinishStatusActivity("Ready");
        } else {
          FinishStatusActivity("Ready", 100);
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

  }

  wxButton* add_files_button_ = nullptr;
  wxButton* clear_button_ = nullptr;
  wxButton* stage1_reset_button_ = nullptr;
  wxButton* stage2_reset_button_ = nullptr;
  wxButton* stage3_reset_button_ = nullptr;
  PaletteButton* small_preset_button_ = nullptr;
  PaletteButton* medium_preset_button_ = nullptr;
  PaletteButton* strong_preset_button_ = nullptr;
  PaletteButton* custom_preset_button_ = nullptr;
  std::vector<wxButton*> thumbnail_reset_buttons_;
  wxButton* convert_button_ = nullptr;
  wxButton* cancel_button_ = nullptr;
  wxScrolledWindow* queue_scroll_ = nullptr;
  wxBoxSizer* queue_sizer_ = nullptr;
  wxPanel* left_panel_ = nullptr;
  wxPanel* inspector_panel_ = nullptr;
  CompareCanvas* compare_canvas_ = nullptr;
  wxSplitterWindow* workspace_splitter_ = nullptr;
  PaletteButton* original_mode_button_ = nullptr;
  PaletteButton* converted_mode_button_ = nullptr;
  PaletteButton* side_by_side_mode_button_ = nullptr;
  PaletteButton* zoom_out_button_ = nullptr;
  PaletteButton* zoom_fit_button_ = nullptr;
  PaletteButton* zoom_50_button_ = nullptr;
  PaletteButton* zoom_100_button_ = nullptr;
  PaletteButton* zoom_in_button_ = nullptr;
  wxChoice* compression_choice_ = nullptr;
  wxCollapsiblePane* output_options_pane_ = nullptr;
  wxRadioButton* specific_directory_radio_ = nullptr;
  wxRadioButton* next_to_source_radio_ = nullptr;
  wxRadioButton* relative_subdir_radio_ = nullptr;
  wxDirPickerCtrl* output_dir_picker_ = nullptr;
  wxTextCtrl* relative_subdir_ctrl_ = nullptr;
  wxActivityIndicator* activity_indicator_ = nullptr;
  wxGauge* progress_gauge_ = nullptr;
  wxStaticText* status_label_ = nullptr;
  wxTimer shutdown_timer_;

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
  std::optional<ResolvedStageSettings> custom_preset_settings_;
  int selected_row_ = -1;
  std::set<int> selected_rows_;
  uint64_t next_item_id_ = 1;
  uint64_t selection_request_id_ = 0;
  uint64_t converted_preview_request_id_ = 0;
  uint64_t active_converted_preview_request_id_ = 0;
  uint64_t preview_cache_access_sequence_ = 0;
  bool converted_preview_worker_running_ = false;
  bool converted_preview_queued_ = false;
  bool updating_sliders_ = false;
  bool conversion_running_ = false;
  std::atomic_int active_workers_ = 0;
  std::atomic_bool close_requested_ = false;
  OverwritePolicy overwrite_policy_ = OverwritePolicy::kAsk;
  std::shared_ptr<std::atomic_bool> selection_cancel_;
  std::shared_ptr<std::atomic_bool> converted_preview_cancel_;
  std::atomic_bool conversion_cancel_ = false;
  ProcessingTaskQueue processing_tasks_;
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
