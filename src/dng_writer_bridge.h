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

bool RenderConvertedCropPreview(const SourceLinearDngMetadata& metadata,
                                const ProcessingCache& cache,
                                const CropRect& crop_rect,
                                const StageOverrideSet& stage_overrides,
                                std::shared_ptr<PreviewImage> preview,
                                ProgressCallback progress = {},
                                CancelCheck cancel = {},
                                std::string* error_message = nullptr);

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
                                     const StageOverrideSet& stage_overrides = {},
                                     const LibRawOverrideSet& libraw_overrides = {},
                                     std::shared_ptr<const PreviewImage> preview_override = {},
                                     double preview_auto_bright_gain = 1.0,
                                     ProgressCallback progress = {},
                                     CancelCheck cancel = {});

DngWriteResult WriteSyntheticLinearDng(const std::string& output_path,
                                       const std::string& compression);
