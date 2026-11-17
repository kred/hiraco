#pragma once

#include "hiraco_types.h"

#include <cstddef>
#include <filesystem>
#include <memory>
#include <string>

enum class HiracoCompression {
  kUncompressed,
  kDeflate,
  kJpegXl,
};

const char* ToCompressionString(HiracoCompression compression);
bool ParseCompressionString(const std::string& value, HiracoCompression* compression);

struct PreparedSourceData;

struct PreparedSource {
  std::string source_path;
  std::string source_name;
  std::string highlight_recovery_source_path;
  bool enable_highlight_recovery = false;
  SourceLinearDngMetadata metadata;
  uint32_t image_width = 0;
  uint32_t image_height = 0;
  std::shared_ptr<PreparedSourceData> data;

  bool IsValid() const {
    return !source_path.empty() && image_width > 0 && image_height > 0;
  }

  bool HasHighlightRecoverySource() const {
    return !highlight_recovery_source_path.empty();
  }

  bool ShouldUseHighlightRecovery() const {
    return enable_highlight_recovery && HasHighlightRecoverySource();
  }
};

enum class OverwritePolicy {
  kAsk,
  kOverwriteAll,
  kSkipAll,
  kCancel,
};

enum class OverwriteResponse {
  kYes,
  kYesToAll,
  kNo,
  kNoToAll,
  kCancel,
};

struct OverwriteDecision {
  bool should_write = false;
  bool canceled = false;
  OverwritePolicy next_policy = OverwritePolicy::kAsk;
};

bool PrepareSource(const std::string& source_path,
                   PreparedSource* prepared,
                   std::string* error_message,
                   ProgressCallback progress = {},
                   CancelCheck cancel = {});

bool RenderOriginalPreview(PreparedSource* prepared,
                           std::shared_ptr<PreviewImage> preview,
                           const LibRawOverrideSet& libraw_overrides,
                           ProgressCallback progress = {},
                           CancelCheck cancel = {},
                           std::string* error_message = nullptr,
                           bool full_resolution = true);

bool TryGetCachedOriginalPreview(const PreparedSource& prepared,
                                 const LibRawOverrideSet& libraw_overrides,
                                 std::shared_ptr<const PreviewImage>* preview);

// Builds the original preview from the processing cache that the converted
// preview already populated.  Returns false when no such cache exists, so the
// caller can fall back to the LibRaw decode path.
bool RenderOriginalPreviewFromProcessingCache(PreparedSource* prepared,
                                              uint32_t max_dimension,
                                              std::shared_ptr<PreviewImage> preview,
                                              ProgressCallback progress = {},
                                              CancelCheck cancel = {},
                                              std::string* error_message = nullptr);

// Releases only recomputable converted-preview data. The original preview and
// metadata remain available, so a queue item can be revisited cheaply.
void ReleasePreviewProcessingCache(PreparedSource* prepared);

// Returns the bytes currently retained by preview and processing caches for a
// prepared source. This supports bounded cache residency in the GUI.
size_t GetPreviewCacheBytes(const PreparedSource& prepared);

bool RenderConvertedCrop(PreparedSource* prepared,
                         const CropRect& crop_rect,
                         const StageOverrideSet& stage_overrides,
                         std::shared_ptr<PreviewImage> preview,
                         const LibRawOverrideSet& libraw_overrides,
                         ProgressCallback progress = {},
                         CancelCheck cancel = {},
                         std::string* error_message = nullptr);

// Renders the same processing result used by conversion as an oriented
// full-frame preview.  The expensive stages run once against the
// full-resolution raster and the result is cached on the prepared source;
// max_dimension (0 = uncapped) only controls the size of the returned copy,
// which is produced by resizing the cached full preview.
bool RenderConvertedFullPreview(PreparedSource* prepared,
                                const StageOverrideSet& stage_overrides,
                                uint32_t max_dimension,
                                std::shared_ptr<PreviewImage> preview,
                                const LibRawOverrideSet& libraw_overrides,
                                ProgressCallback progress = {},
                                CancelCheck cancel = {},
                                std::string* error_message = nullptr,
                                bool scale_cache_for_preview = false);

// Renders the interactive preview from a display-sized proxy.  Unlike the
// full-resolution preview this is deliberately approximate: the enhancement
// stages run on at most `max_dimension` pixels, so the browser does not pay for
// a 50-80 MP pipeline on every fit/50% request.  The DNG conversion and the
// 100% preview never use this path.
bool RenderConvertedProxyPreview(PreparedSource* prepared,
                                 const StageOverrideSet& stage_overrides,
                                 uint32_t max_dimension,
                                 std::shared_ptr<PreviewImage> preview,
                                 const LibRawOverrideSet& libraw_overrides,
                                 ProgressCallback progress = {},
                                 CancelCheck cancel = {},
                                 std::string* error_message = nullptr);

DngWriteResult ConvertToDng(const PreparedSource& prepared,
                            const std::filesystem::path& output_path,
                            HiracoCompression compression,
                            const StageOverrideSet& stage_overrides,
                            const LibRawOverrideSet& libraw_overrides,
                            ProgressCallback progress = {},
                            CancelCheck cancel = {});

ResolvedStageSettings GetResolvedStageSettings(const PreparedSource& prepared,
                                              const StageOverrideSet& overrides);

CropRect ClampCropRect(const CropRect& requested,
                      uint32_t image_width,
                      uint32_t image_height);

CropRect CenterCropRect(uint32_t image_width,
                        uint32_t image_height,
                        uint32_t crop_width,
                        uint32_t crop_height);

std::filesystem::path ResolveOutputPath(const std::string& source_path,
                                        const std::filesystem::path& base_output_dir,
                                        const std::filesystem::path& relative_subdir);

// Whether two paths name the same file, including through symlinks, hard links
// or a not-yet-existing target reached by different spellings.  Used to refuse
// conversions whose output would overwrite the source RAW.
bool PathsReferToSameFile(const std::filesystem::path& left,
                          const std::filesystem::path& right);

OverwriteDecision ResolveOverwriteDecision(OverwritePolicy current_policy,
                                           bool target_exists,
                                           OverwriteResponse response);

StageOverrideSet ReadStageOverridesFromEnvironment();
LibRawOverrideSet ReadLibRawOverridesFromEnvironment();
