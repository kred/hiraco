#pragma once

#include "hiraco_types.h"

#include <cstdint>
#include <memory>
#include <string>

struct DngWriterRuntimeSummary {
  bool enabled = false;
  bool host_created = false;
  bool negative_created = false;
  std::string status;
};

DngWriterRuntimeSummary BuildDngWriterRuntimeSummary(const std::string& compression);

/// Reports whether the DNG writer can actually be used in this build for the
/// given compression mode.
///
/// The capability summaries in dng_sdk_bridge.h are built on top of this, so
/// that they report what the running build can do rather than what the
/// source was compiled with.  `reason` receives a human readable explanation
/// when the writer cannot be used.
bool CanWriteLinearDng(const std::string& compression, std::string* reason);

// Diagnostics for the most recent ORI highlight-recovery run.  Surfaced in the
// GUI status line so users can see recovery engage (or learn that the image has
// no recoverable highlights) without HIRACO_DEBUG_HL_RECOVERY.  run_sequence
// increments on every run so callers can distinguish a fresh run from a cached
// preview render that skipped recovery entirely.
struct HighlightRecoveryStats {
  bool applicable = false;
  uint64_t candidates = 0;
  uint64_t changed = 0;
  uint64_t run_sequence = 0;
};

// Which pane of the compare canvas the recovered-pixel cast is drawn on.
// This is the whole of the "where does the cast appear" rule, kept in
// hiraco-core so it can be tested without a GUI: the GUI's OnPaint only has to
// ask this question once and draw the bitmap it returns.
struct RecoveredPixelCastPlacement {
  bool on_original = false;
  bool on_converted = false;
};

// The cast marks what *recovery* changed, so it belongs to the converted image
// only.  It is therefore drawn on the converted pane when the converted image is
// visible at all, and never on the original: kOriginal shows the unprocessed ORF
// preview, in which recovery never ran.  Zoom never changes the answer -- the
// bitmap is picked before the pane scaling -- but it is a parameter anyway so the
// rule is stated over every combination the user can reach.
inline RecoveredPixelCastPlacement ResolveRecoveredPixelCastPlacement(
    bool show_recovered_pixels,
    bool has_recovered_mask,
    bool converted_visible,
    bool original_visible) {
  (void)original_visible;  // The cast never goes on the original pane.
  if (!show_recovered_pixels || !has_recovered_mask) {
    return {};
  }
  RecoveredPixelCastPlacement placement;
  placement.on_converted = converted_visible;
  return placement;
}

HighlightRecoveryStats GetLastHighlightRecoveryStats();

// Marks the recovered pixels of a converted preview so the GUI can show them.
// Every pixel whose `recovered_mask` byte is 1 is replaced with solid pink
// (255, 0, 255); every other pixel is left byte-identical, so the rest of the
// preview is unchanged and the cast never touches a pixel recovery did not
// recover.  Returns the number of pixels it changed.  A preview whose mask is
// empty, sized differently from its pixels, or has fewer than three colours is
// left untouched and reports 0, so the helper refuses caller bugs instead of
// reading or writing out of bounds.
size_t ApplyRecoveredPixelCast(PreviewImage* preview);

ResolvedStageSettings ResolveStageSettingsForImage(const SourceLinearDngMetadata& metadata,
                                                   uint32_t width,
                                                   uint32_t height,
                                                   const StageOverrideSet& overrides);

bool BuildOriginalPreviewFromRaw(const std::string& source_path,
                                 const SourceLinearDngMetadata& metadata,
                                 const LibRawOverrideSet& libraw_overrides,
                                 std::shared_ptr<PreviewImage> preview,
                                 ProgressCallback progress = {},
                                 CancelCheck cancel = {},
                                 std::string* error_message = nullptr);

bool EstimatePreviewAutoBrightGainFromRaw(const std::string& source_path,
                                          const SourceLinearDngMetadata& metadata,
                                          const LibRawOverrideSet& libraw_overrides,
                                          double* gain,
                                          std::string* error_message = nullptr);

bool BuildProcessingCacheFromRaw(const std::string& source_path,
                                 const SourceLinearDngMetadata& metadata,
                                 uint32_t source_width,
                                 uint32_t source_height,
                                 const CropRect& crop_rect,
                                 const LibRawOverrideSet& libraw_overrides,
                                 ProcessingCache* cache,
                                 ProgressCallback progress = {},
                                 CancelCheck cancel = {},
                                 std::string* error_message = nullptr);

// Builds a display-resolution derivative of an immutable full-resolution
// processing cache. It is intended for responsive whole-image previews;
// export and future 100% tiles continue to use the source cache.
bool BuildDisplayProcessingCache(const ProcessingCache& source,
                                 uint32_t max_dimension,
                                 ProcessingCache* display_cache,
                                 std::string* error_message = nullptr);

bool RenderConvertedCropPreview(const SourceLinearDngMetadata& metadata,
                                const ProcessingCache& cache,
                                const CropRect& crop_rect,
                                const std::string& highlight_recovery_source_path,
                                bool enable_highlight_recovery,
                                const StageOverrideSet& stage_overrides,
                                std::shared_ptr<PreviewImage> preview,
                                ProgressCallback progress = {},
                                CancelCheck cancel = {},
                                std::string* error_message = nullptr,
                                uint32_t max_dimension = 0,
                                const RecoveryImageCoordinates* recovery_coordinates = nullptr);

bool ApplyResolvedStageSettingsForTesting(const SourceLinearDngMetadata& metadata,
                                          const ResolvedStageSettings& settings,
                                          const RasterImage* cfa_guide_image,
                                          RasterImage* image,
                                          ProgressCallback progress = {},
                                          CancelCheck cancel = {},
                                          std::string* error_message = nullptr);

DngWriteResult WriteLinearDngFromRaw(const std::string& source_path,
                                     const std::string& output_path,
                                     const std::string& compression,
                                     const SourceLinearDngMetadata& metadata,
                                     const std::string& highlight_recovery_source_path = {},
                                     bool enable_highlight_recovery = false,
                                     const StageOverrideSet& stage_overrides = {},
                                     const LibRawOverrideSet& libraw_overrides = {},
                                     std::shared_ptr<const PreviewImage> preview_override = {},
                                     double preview_auto_bright_gain = 1.0,
                                     ProgressCallback progress = {},
                                     CancelCheck cancel = {});
