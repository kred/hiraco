#pragma once

#include "hiraco_types.h"

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
  SourceLinearDngMetadata metadata;
  uint32_t image_width = 0;
  uint32_t image_height = 0;
  std::shared_ptr<PreparedSourceData> data;

  bool IsValid() const {
    return !source_path.empty() && image_width > 0 && image_height > 0;
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
                           std::string* error_message = nullptr);

bool TryGetCachedOriginalPreview(const PreparedSource& prepared,
                                 const LibRawOverrideSet& libraw_overrides,
                                 std::shared_ptr<PreviewImage> preview);

bool RenderConvertedCrop(PreparedSource* prepared,
                         const CropRect& crop_rect,
                         const StageOverrideSet& stage_overrides,
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

OverwriteDecision ResolveOverwriteDecision(OverwritePolicy current_policy,
                                           bool target_exists,
                                           OverwriteResponse response);

StageOverrideSet ReadStageOverridesFromEnvironment();
LibRawOverrideSet ReadLibRawOverridesFromEnvironment();
