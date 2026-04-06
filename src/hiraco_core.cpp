#include "hiraco_core.h"

#include "dng_writer_bridge.h"
#include "hiraco_timing.h"
#include "stack_guidance.h"
#include "vendor_makernote.h"

#include <algorithm>
#include <array>
#include <cstdlib>
#include <filesystem>
#include <libraw/libraw.h>
#include <mutex>
#include <string>

namespace {

struct DecodeSummary {
  std::string camera_make;
  std::string camera_model;
  int libraw_flip = 0;
  unsigned raw_width = 0;
  unsigned raw_height = 0;
  unsigned image_width = 0;
  unsigned image_height = 0;
  unsigned black_level = 0;
  unsigned white_level = 0;
  bool has_color_matrix1 = false;
  std::array<double, 9> color_matrix1 = {0.0, 0.0, 0.0,
                                         0.0, 0.0, 0.0,
                                         0.0, 0.0, 0.0};
  bool has_rgb_cam = false;
  std::array<double, 9> rgb_cam = {0.0, 0.0, 0.0,
                                   0.0, 0.0, 0.0,
                                   0.0, 0.0, 0.0};
  bool has_as_shot_neutral = false;
  std::array<double, 3> as_shot_neutral = {1.0, 1.0, 1.0};
  bool has_default_crop = false;
  uint32_t default_crop_origin_h = 0;
  uint32_t default_crop_origin_v = 0;
  uint32_t default_crop_width = 0;
  uint32_t default_crop_height = 0;
};

bool ReadEnvInt(const char* name, int* value) {
  const char* raw = std::getenv(name);
  if (raw == nullptr || *raw == '\0') {
    return false;
  }

  char* end = nullptr;
  const long parsed = std::strtol(raw, &end, 10);
  if (end == raw || (end != nullptr && *end != '\0')) {
    return false;
  }

  *value = static_cast<int>(parsed);
  return true;
}

bool ReadEnvFloat(const char* name, float* value) {
  const char* raw = std::getenv(name);
  if (raw == nullptr || *raw == '\0') {
    return false;
  }

  char* end = nullptr;
  const float parsed = std::strtof(raw, &end);
  if (end == raw || (end != nullptr && *end != '\0')) {
    return false;
  }

  *value = parsed;
  return true;
}

bool IsCancelled(const CancelCheck& cancel) {
  return static_cast<bool>(cancel) && cancel();
}

void ReportProgress(const ProgressCallback& progress,
                    const std::string& phase,
                    double fraction,
                    const std::string& message) {
  if (!progress) {
    return;
  }

  ProcessingProgress update;
  update.phase = phase;
  update.fraction = fraction;
  update.message = message;
  progress(update);
}

CropRect ExpandCropRectToPreviewRegion(const CropRect& crop_rect,
                                       uint32_t source_width,
                                       uint32_t source_height) {
  CropRect region;
  if (source_width == 0 || source_height == 0) {
    return region;
  }

  const uint32_t clamped_x = std::min(crop_rect.x, source_width - 1);
  const uint32_t clamped_y = std::min(crop_rect.y, source_height - 1);
  const uint32_t clamped_width = std::max(1u, std::min(crop_rect.width, source_width - clamped_x));
  const uint32_t clamped_height = std::max(1u, std::min(crop_rect.height, source_height - clamped_y));

  const uint32_t left = clamped_x > kCropPreviewProcessingBorder
      ? clamped_x - kCropPreviewProcessingBorder
      : 0;
  const uint32_t top = clamped_y > kCropPreviewProcessingBorder
      ? clamped_y - kCropPreviewProcessingBorder
      : 0;
  const uint32_t right = std::min(source_width,
                                  clamped_x + clamped_width + kCropPreviewProcessingBorder);
  const uint32_t bottom = std::min(source_height,
                                   clamped_y + clamped_height + kCropPreviewProcessingBorder);

  region.x = left;
  region.y = top;
  region.width = right - left;
  region.height = bottom - top;
  return region;
}

bool ProcessingCacheCoversCrop(const ProcessingCache& cache,
                               const CropRect& crop_rect) {
  if (!cache.has_cached_crop) {
    return true;
  }

  const uint32_t source_width = cache.source_width > 0 ? cache.source_width : cache.raw_image.width;
  const uint32_t source_height = cache.source_height > 0 ? cache.source_height : cache.raw_image.height;
  if (source_width == 0 || source_height == 0) {
    return false;
  }

  const CropRect needed_region = ExpandCropRectToPreviewRegion(crop_rect,
                                                               source_width,
                                                               source_height);
  const uint64_t needed_right = static_cast<uint64_t>(needed_region.x) + needed_region.width;
  const uint64_t needed_bottom = static_cast<uint64_t>(needed_region.y) + needed_region.height;
  const uint64_t cached_right = static_cast<uint64_t>(cache.region_origin_x) + cache.raw_image.width;
  const uint64_t cached_bottom = static_cast<uint64_t>(cache.region_origin_y) + cache.raw_image.height;
  return needed_region.x >= cache.region_origin_x &&
         needed_region.y >= cache.region_origin_y &&
         needed_right <= cached_right &&
         needed_bottom <= cached_bottom;
}

bool DecodeWithLibRaw(const std::string& source_path,
                      DecodeSummary* summary,
                      std::string* error_message) {
  auto processor = std::make_unique<LibRaw>();

  int result = processor->open_file(source_path.c_str());
  if (result != LIBRAW_SUCCESS) {
    *error_message = std::string("LibRaw open_file failed: ") + libraw_strerror(result);
    return false;
  }

  summary->camera_make = processor->imgdata.idata.make;
  summary->camera_model = processor->imgdata.idata.model;
  summary->libraw_flip = processor->imgdata.sizes.flip;
  summary->raw_width = processor->imgdata.sizes.raw_width;
  summary->raw_height = processor->imgdata.sizes.raw_height;
  summary->image_width = processor->imgdata.sizes.width;
  summary->image_height = processor->imgdata.sizes.height;
  summary->black_level = processor->imgdata.color.black;
  summary->white_level = processor->imgdata.color.maximum;

  result = processor->unpack();
  if (result != LIBRAW_SUCCESS) {
    *error_message = std::string("LibRaw unpack failed: ") + libraw_strerror(result);
    processor->recycle();
    return false;
  }

  summary->black_level = processor->imgdata.color.black;
  summary->white_level = processor->imgdata.color.maximum;
  summary->libraw_flip = processor->imgdata.sizes.flip;

  bool all_zero = true;
  for (int row = 0; row < 3; ++row) {
    for (int col = 0; col < 3; ++col) {
      const double value = processor->imgdata.color.cmatrix[row][col];
      summary->color_matrix1[row * 3 + col] = value;
      if (value != 0.0) {
        all_zero = false;
      }
    }
  }
  summary->has_color_matrix1 = !all_zero;

  bool rgb_cam_all_zero = true;
  for (int row = 0; row < 3; ++row) {
    for (int col = 0; col < 3; ++col) {
      const double value = processor->imgdata.color.rgb_cam[row][col];
      summary->rgb_cam[row * 3 + col] = value;
      if (value != 0.0) {
        rgb_cam_all_zero = false;
      }
    }
  }
  summary->has_rgb_cam = !rgb_cam_all_zero;

  if (processor->imgdata.color.cam_mul[0] > 0.0f &&
      processor->imgdata.color.cam_mul[1] > 0.0f &&
      processor->imgdata.color.cam_mul[2] > 0.0f) {
    summary->has_as_shot_neutral = true;
    const double green = processor->imgdata.color.cam_mul[1];
    summary->as_shot_neutral[0] = static_cast<double>(green / processor->imgdata.color.cam_mul[0]);
    summary->as_shot_neutral[1] = 1.0;
    summary->as_shot_neutral[2] = static_cast<double>(green / processor->imgdata.color.cam_mul[2]);
  }

  const libraw_image_sizes_t& sizes = processor->imgdata.sizes;
  if (sizes.raw_inset_crops[0].cwidth > 0 && sizes.raw_inset_crops[0].cheight > 0) {
    summary->has_default_crop = true;
    summary->default_crop_origin_h = sizes.raw_inset_crops[0].cleft;
    summary->default_crop_origin_v = sizes.raw_inset_crops[0].ctop;
    summary->default_crop_width = sizes.raw_inset_crops[0].cwidth;
    summary->default_crop_height = sizes.raw_inset_crops[0].cheight;
  }

  processor->recycle();
  return true;
}

SourceLinearDngMetadata BuildMetadataFromLibRaw(const std::string& source_path,
                                                const DecodeSummary& decode_summary) {
  SourceLinearDngMetadata metadata;
  metadata.make = decode_summary.camera_make;
  metadata.model = decode_summary.camera_model;
  metadata.unique_camera_model = decode_summary.camera_make + " " + decode_summary.camera_model;
  metadata.libraw_flip = NormalizeLibRawFlip(decode_summary.libraw_flip);

  const unsigned raw_white = decode_summary.white_level;
  if (raw_white > 0) {
    const unsigned raw_bits = 32u - static_cast<unsigned>(__builtin_clz(raw_white));
    const unsigned shift = std::max(0, 16 - static_cast<int>(raw_bits));
    metadata.has_black_level = true;
    metadata.black_level = static_cast<double>(decode_summary.black_level) * (1u << shift);
    metadata.has_white_level = true;
    metadata.white_level = 65535.0;
  } else {
    metadata.has_black_level = true;
    metadata.black_level = decode_summary.black_level;
    metadata.has_white_level = true;
    metadata.white_level = decode_summary.white_level;
  }

  metadata.has_color_matrix1 = decode_summary.has_color_matrix1;
  if (metadata.has_color_matrix1) {
    for (size_t index = 0; index < decode_summary.color_matrix1.size(); ++index) {
      metadata.color_matrix1[index] = decode_summary.color_matrix1[index];
    }
  }

  metadata.has_rgb_cam = decode_summary.has_rgb_cam;
  if (metadata.has_rgb_cam) {
    for (size_t index = 0; index < decode_summary.rgb_cam.size(); ++index) {
      metadata.rgb_cam[index] = decode_summary.rgb_cam[index];
    }
  }

  metadata.has_as_shot_neutral = decode_summary.has_as_shot_neutral;
  if (metadata.has_as_shot_neutral) {
    metadata.as_shot_neutral[0] = decode_summary.as_shot_neutral[0];
    metadata.as_shot_neutral[1] = decode_summary.as_shot_neutral[1];
    metadata.as_shot_neutral[2] = decode_summary.as_shot_neutral[2];
  }

  metadata.has_default_crop = decode_summary.has_default_crop;
  if (metadata.has_default_crop) {
    metadata.default_crop_origin_h = decode_summary.default_crop_origin_h;
    metadata.default_crop_origin_v = decode_summary.default_crop_origin_v;
    metadata.default_crop_width = decode_summary.default_crop_width;
    metadata.default_crop_height = decode_summary.default_crop_height;
  }

  metadata.has_predicted_detail_gain = true;
  metadata.predicted_detail_gain = 1.8;

  (void) source_path;
  return metadata;
}

void ApplyStackGuidanceMaps(const StackGuidanceMaps& guidance,
                           SourceLinearDngMetadata* metadata) {
  if (metadata == nullptr || !guidance.ok) {
    return;
  }

  metadata->has_stack_stability_map = true;
  metadata->stack_stability_width = StackGuidanceMaps::kTileWidth;
  metadata->stack_stability_height = StackGuidanceMaps::kTileHeight;
  metadata->stack_stability_data = guidance.stability;

  metadata->has_stack_mean_map = true;
  metadata->stack_mean_width = StackGuidanceMaps::kTileWidth;
  metadata->stack_mean_height = StackGuidanceMaps::kTileHeight;
  metadata->stack_mean_data = guidance.mean;

  metadata->has_stack_guide_map = true;
  metadata->stack_guide_width = StackGuidanceMaps::kTileWidth;
  metadata->stack_guide_height = StackGuidanceMaps::kTileHeight;
  metadata->stack_guide_data = guidance.guide;

  metadata->has_stack_tensor_x_map = true;
  metadata->stack_tensor_x_width = StackGuidanceMaps::kTileWidth;
  metadata->stack_tensor_x_height = StackGuidanceMaps::kTileHeight;
  metadata->stack_tensor_x_data = guidance.tensor_x;

  metadata->has_stack_tensor_y_map = true;
  metadata->stack_tensor_y_width = StackGuidanceMaps::kTileWidth;
  metadata->stack_tensor_y_height = StackGuidanceMaps::kTileHeight;
  metadata->stack_tensor_y_data = guidance.tensor_y;

  metadata->has_stack_tensor_coherence_map = true;
  metadata->stack_tensor_coherence_width = StackGuidanceMaps::kTileWidth;
  metadata->stack_tensor_coherence_height = StackGuidanceMaps::kTileHeight;
  metadata->stack_tensor_coherence_data = guidance.tensor_coherence;

  metadata->has_stack_alias_map = true;
  metadata->stack_alias_width = StackGuidanceMaps::kTileWidth;
  metadata->stack_alias_height = StackGuidanceMaps::kTileHeight;
  metadata->stack_alias_data = guidance.alias;
}

bool BuildEnhancementMetadata(const std::string& source_path,
                              const SourceLinearDngMetadata& base_metadata,
                              std::shared_ptr<const SourceLinearDngMetadata>* metadata,
                              std::string* error_message) {
  if (metadata == nullptr) {
    if (error_message != nullptr) {
      *error_message = "missing enhancement metadata output";
    }
    return false;
  }

  auto enriched_metadata = std::make_shared<SourceLinearDngMetadata>(base_metadata);
  const hiraco::ScopedTimingLog metadata_timer("metadata", "Build enhancement metadata");

  VendorMakerNoteResult maker_note;
  {
    const hiraco::ScopedTimingLog timer("metadata", "Read vendor maker note");
    maker_note = ReadVendorMakerNote(source_path);
  }

  if (maker_note.ok) {
    if (!maker_note.tiff_make.empty()) {
      enriched_metadata->make = maker_note.tiff_make;
    }
    if (!maker_note.tiff_model.empty()) {
      enriched_metadata->model = maker_note.tiff_model;
    }
    enriched_metadata->unique_camera_model = enriched_metadata->make + " " + enriched_metadata->model;

    if (maker_note.has_working_geometry) {
      enriched_metadata->has_working_geometry = true;
      enriched_metadata->working_width = maker_note.working_width;
      enriched_metadata->working_height = maker_note.working_height;
    }

    if (maker_note.has_stacked_image &&
        maker_note.stacked_image_label == "Hand-held high resolution (11 12)" &&
        !maker_note.unknown_block_3.empty()) {
      StackGuidanceMaps guidance;
      {
        const hiraco::ScopedTimingLog timer("metadata", "Compute stack guidance");
        guidance = ComputeStackGuidance(maker_note.unknown_block_3);
      }
      ApplyStackGuidanceMaps(guidance, enriched_metadata.get());
    }
  }

  *metadata = enriched_metadata;
  return true;
}

bool LibRawOverridesEqual(const LibRawOverrideSet& lhs, const LibRawOverrideSet& rhs) {
  return lhs.user_qual == rhs.user_qual &&
         lhs.four_color_rgb == rhs.four_color_rgb &&
         lhs.green_matching == rhs.green_matching &&
         lhs.med_passes == rhs.med_passes &&
         lhs.no_auto_scale == rhs.no_auto_scale &&
         lhs.no_auto_bright == rhs.no_auto_bright &&
         lhs.adjust_maximum_thr == rhs.adjust_maximum_thr;
}

}  // namespace

struct PreparedSourceData {
  std::mutex mutex;
  bool has_original_preview = false;
  PreviewImage original_preview;
  bool has_processing_cache = false;
  ProcessingCache processing_cache;
  LibRawOverrideSet cached_libraw_overrides;
  bool has_preview_auto_bright_gain = false;
  double preview_auto_bright_gain = 1.0;
  LibRawOverrideSet preview_auto_bright_libraw_overrides;
  std::shared_ptr<const SourceLinearDngMetadata> enhancement_metadata;
};

namespace {

bool EnsureEnhancementMetadata(const PreparedSource& prepared,
                              std::shared_ptr<const SourceLinearDngMetadata>* metadata,
                              std::string* error_message) {
  if (metadata == nullptr) {
    if (error_message != nullptr) {
      *error_message = "missing enhancement metadata output";
    }
    return false;
  }

  if (prepared.data) {
    std::lock_guard<std::mutex> lock(prepared.data->mutex);
    if (prepared.data->enhancement_metadata) {
      *metadata = prepared.data->enhancement_metadata;
      return true;
    }
  }

  std::shared_ptr<const SourceLinearDngMetadata> built_metadata;
  if (!BuildEnhancementMetadata(prepared.source_path,
                                prepared.metadata,
                                &built_metadata,
                                error_message)) {
    return false;
  }

  if (!prepared.data) {
    *metadata = built_metadata;
    return true;
  }

  std::lock_guard<std::mutex> lock(prepared.data->mutex);
  if (!prepared.data->enhancement_metadata) {
    prepared.data->enhancement_metadata = std::move(built_metadata);
  }
  *metadata = prepared.data->enhancement_metadata;
  return true;
}

bool EnsurePreviewAutoBrightGain(PreparedSource* prepared,
                                 const LibRawOverrideSet& libraw_overrides,
                                 std::string* error_message) {
  if (prepared == nullptr || !prepared->data) {
    if (error_message != nullptr) {
      *error_message = "prepared source is missing preview brightness state";
    }
    return false;
  }

  {
    std::lock_guard<std::mutex> lock(prepared->data->mutex);
    if (prepared->data->has_preview_auto_bright_gain &&
        LibRawOverridesEqual(prepared->data->preview_auto_bright_libraw_overrides,
                             libraw_overrides)) {
      return true;
    }
  }

  double gain = 1.0;
  if (!EstimatePreviewAutoBrightGainFromRaw(prepared->source_path,
                                            prepared->metadata,
                                            libraw_overrides,
                                            &gain,
                                            error_message)) {
    return false;
  }

  std::lock_guard<std::mutex> lock(prepared->data->mutex);
  prepared->data->has_preview_auto_bright_gain = true;
  prepared->data->preview_auto_bright_gain = gain;
  prepared->data->preview_auto_bright_libraw_overrides = libraw_overrides;
  return true;
}

}  // namespace

const char* ToCompressionString(HiracoCompression compression) {
  switch (compression) {
    case HiracoCompression::kUncompressed:
      return "uncompressed";
    case HiracoCompression::kDeflate:
      return "deflate";
    case HiracoCompression::kJpegXl:
      return "jpeg-xl";
  }
  return "uncompressed";
}

bool ParseCompressionString(const std::string& value, HiracoCompression* compression) {
  if (compression == nullptr) {
    return false;
  }
  if (value == "uncompressed") {
    *compression = HiracoCompression::kUncompressed;
    return true;
  }
  if (value == "deflate") {
    *compression = HiracoCompression::kDeflate;
    return true;
  }
  if (value == "jpeg-xl") {
    *compression = HiracoCompression::kJpegXl;
    return true;
  }
  return false;
}

bool PrepareSource(const std::string& source_path,
                   PreparedSource* prepared,
                   std::string* error_message,
                   ProgressCallback progress,
                   CancelCheck cancel) {
  const hiraco::ScopedTimingLog prepare_timer("prepare", "Prepare source");
  if (prepared == nullptr || error_message == nullptr) {
    return false;
  }

  if (!std::filesystem::exists(source_path)) {
    *error_message = "source file does not exist: " + source_path;
    return false;
  }

  ReportProgress(progress, "prepare", 0.05, "Inspecting source metadata");
  if (IsCancelled(cancel)) {
    *error_message = "operation canceled";
    return false;
  }

  DecodeSummary summary;
  {
    const hiraco::ScopedTimingLog timer("prepare", "Decode LibRaw summary");
    if (!DecodeWithLibRaw(source_path, &summary, error_message)) {
      return false;
    }
  }

  if (IsCancelled(cancel)) {
    *error_message = "operation canceled";
    return false;
  }

  ReportProgress(progress, "prepare", 0.55, "Building reusable metadata");
  prepared->source_path = source_path;
  prepared->source_name = std::filesystem::path(source_path).filename().string();
  {
    const hiraco::ScopedTimingLog timer("prepare", "Build base metadata");
    prepared->metadata = BuildMetadataFromLibRaw(source_path, summary);
  }
  prepared->image_width = summary.raw_width > 0 ? summary.raw_width : summary.image_width;
  prepared->image_height = summary.raw_height > 0 ? summary.raw_height : summary.image_height;
  prepared->data = std::make_shared<PreparedSourceData>();

  ReportProgress(progress, "prepare", 1.0, "Source is ready");
  return true;
}

bool RenderOriginalPreview(PreparedSource* prepared,
                           std::shared_ptr<PreviewImage> preview,
                           const LibRawOverrideSet& libraw_overrides,
                           ProgressCallback progress,
                           CancelCheck cancel,
                           std::string* error_message) {
  if (prepared == nullptr || !preview) {
    if (error_message != nullptr) {
      *error_message = "invalid preview request";
    }
    return false;
  }

  if (!prepared->data) {
    if (error_message != nullptr) {
      *error_message = "prepared source is missing cache state";
    }
    return false;
  }

  {
    std::lock_guard<std::mutex> lock(prepared->data->mutex);
    if (prepared->data->has_original_preview &&
        LibRawOverridesEqual(prepared->data->cached_libraw_overrides, libraw_overrides)) {
      *preview = prepared->data->original_preview;
      return true;
    }
  }

  if (!BuildOriginalPreviewFromRaw(prepared->source_path,
                                   prepared->metadata,
                                   libraw_overrides,
                                   preview,
                                   progress,
                                   cancel,
                                   error_message)) {
    return false;
  }

  std::lock_guard<std::mutex> lock(prepared->data->mutex);
  prepared->data->has_original_preview = true;
  prepared->data->cached_libraw_overrides = libraw_overrides;
  prepared->data->original_preview = *preview;
  return true;
}

bool TryGetCachedOriginalPreview(const PreparedSource& prepared,
                                 const LibRawOverrideSet& libraw_overrides,
                                 std::shared_ptr<PreviewImage> preview) {
  if (!preview || !prepared.data) {
    return false;
  }

  std::lock_guard<std::mutex> lock(prepared.data->mutex);
  if (!prepared.data->has_original_preview ||
      !LibRawOverridesEqual(prepared.data->cached_libraw_overrides, libraw_overrides)) {
    return false;
  }

  *preview = prepared.data->original_preview;
  return true;
}

bool RenderConvertedCrop(PreparedSource* prepared,
                         const CropRect& crop_rect,
                         const StageOverrideSet& stage_overrides,
                         std::shared_ptr<PreviewImage> preview,
                         const LibRawOverrideSet& libraw_overrides,
                         ProgressCallback progress,
                         CancelCheck cancel,
                         std::string* error_message) {
  const hiraco::ScopedTimingLog crop_timer("crop", "Render converted crop");
  if (prepared == nullptr || !preview) {
    if (error_message != nullptr) {
      *error_message = "invalid crop preview request";
    }
    return false;
  }

  if (!prepared->data) {
    if (error_message != nullptr) {
      *error_message = "prepared source is missing cache state";
    }
    return false;
  }

  std::shared_ptr<const SourceLinearDngMetadata> enhancement_metadata;
  if (!EnsureEnhancementMetadata(*prepared, &enhancement_metadata, error_message)) {
    return false;
  }

  if (!EnsurePreviewAutoBrightGain(prepared, libraw_overrides, error_message)) {
    return false;
  }

  {
    std::lock_guard<std::mutex> lock(prepared->data->mutex);
    if (prepared->data->has_processing_cache &&
        LibRawOverridesEqual(prepared->data->cached_libraw_overrides, libraw_overrides) &&
        ProcessingCacheCoversCrop(prepared->data->processing_cache, crop_rect)) {
      prepared->data->processing_cache.preview_auto_bright_gain =
          prepared->data->preview_auto_bright_gain;
        return RenderConvertedCropPreview(*enhancement_metadata,
                                        prepared->data->processing_cache,
                                        crop_rect,
                                        stage_overrides,
                                        preview,
                                        progress,
                                        cancel,
                                        error_message);
    }
  }

  ProcessingCache cache;
  if (!BuildProcessingCacheFromRaw(prepared->source_path,
                                   *enhancement_metadata,
                                   prepared->image_width,
                                   prepared->image_height,
                                   crop_rect,
                                   libraw_overrides,
                                   &cache,
                                   progress,
                                   cancel,
                                   error_message)) {
    return false;
  }

  {
    std::lock_guard<std::mutex> lock(prepared->data->mutex);
    cache.preview_auto_bright_gain = prepared->data->preview_auto_bright_gain;
  }

  {
    std::lock_guard<std::mutex> lock(prepared->data->mutex);
    prepared->data->has_processing_cache = true;
    prepared->data->cached_libraw_overrides = libraw_overrides;
    prepared->data->processing_cache = cache;
  }

  std::lock_guard<std::mutex> lock(prepared->data->mutex);
  return RenderConvertedCropPreview(*enhancement_metadata,
                                    prepared->data->processing_cache,
                                    crop_rect,
                                    stage_overrides,
                                    preview,
                                    progress,
                                    cancel,
                                    error_message);
}

DngWriteResult ConvertToDng(const PreparedSource& prepared,
                            const std::filesystem::path& output_path,
                            HiracoCompression compression,
                            const StageOverrideSet& stage_overrides,
                            const LibRawOverrideSet& libraw_overrides,
                            ProgressCallback progress,
                            CancelCheck cancel) {
  std::shared_ptr<const SourceLinearDngMetadata> enhancement_metadata;
  DngWriteResult result;
  if (!EnsureEnhancementMetadata(prepared, &enhancement_metadata, &result.message)) {
    return result;
  }

  std::shared_ptr<const PreviewImage> preview_override;
  if (prepared.data) {
    std::lock_guard<std::mutex> lock(prepared.data->mutex);
    if (prepared.data->has_original_preview) {
      preview_override = std::make_shared<PreviewImage>(prepared.data->original_preview);
    }
  }

  return WriteLinearDngFromRaw(prepared.source_path,
                               output_path.string(),
                               ToCompressionString(compression),
                               *enhancement_metadata,
                               stage_overrides,
                               libraw_overrides,
                               preview_override,
                               progress,
                               cancel);
}

ResolvedStageSettings GetResolvedStageSettings(const PreparedSource& prepared,
                                              const StageOverrideSet& overrides) {
  return ResolveStageSettingsForImage(prepared.metadata,
                                      prepared.image_width,
                                      prepared.image_height,
                                      overrides);
}

CropRect ClampCropRect(const CropRect& requested,
                      uint32_t image_width,
                      uint32_t image_height) {
  CropRect result = requested;
  result.width = std::min(std::max(result.width, 1u), image_width);
  result.height = std::min(std::max(result.height, 1u), image_height);
  result.x = std::min(result.x, image_width - result.width);
  result.y = std::min(result.y, image_height - result.height);
  return result;
}

CropRect CenterCropRect(uint32_t image_width,
                        uint32_t image_height,
                        uint32_t crop_width,
                        uint32_t crop_height) {
  CropRect rect;
  rect.width = std::min(std::max(crop_width, 1u), image_width);
  rect.height = std::min(std::max(crop_height, 1u), image_height);
  rect.x = (image_width - rect.width) / 2;
  rect.y = (image_height - rect.height) / 2;
  return rect;
}

std::filesystem::path ResolveOutputPath(const std::string& source_path,
                                        const std::filesystem::path& base_output_dir,
                                        const std::filesystem::path& relative_subdir) {
  return base_output_dir / relative_subdir / (std::filesystem::path(source_path).stem().string() + ".dng");
}

OverwriteDecision ResolveOverwriteDecision(OverwritePolicy current_policy,
                                           bool target_exists,
                                           OverwriteResponse response) {
  OverwriteDecision decision;
  decision.next_policy = current_policy;

  if (!target_exists) {
    decision.should_write = true;
    return decision;
  }

  switch (current_policy) {
    case OverwritePolicy::kOverwriteAll:
      decision.should_write = true;
      return decision;
    case OverwritePolicy::kSkipAll:
      decision.should_write = false;
      return decision;
    case OverwritePolicy::kCancel:
      decision.canceled = true;
      return decision;
    case OverwritePolicy::kAsk:
      break;
  }

  switch (response) {
    case OverwriteResponse::kYes:
      decision.should_write = true;
      break;
    case OverwriteResponse::kYesToAll:
      decision.should_write = true;
      decision.next_policy = OverwritePolicy::kOverwriteAll;
      break;
    case OverwriteResponse::kNo:
      decision.should_write = false;
      break;
    case OverwriteResponse::kNoToAll:
      decision.should_write = false;
      decision.next_policy = OverwritePolicy::kSkipAll;
      break;
    case OverwriteResponse::kCancel:
      decision.canceled = true;
      decision.next_policy = OverwritePolicy::kCancel;
      break;
  }

  return decision;
}

StageOverrideSet ReadStageOverridesFromEnvironment() {
  StageOverrideSet overrides;
  auto maybe_float = [](const char* name) -> std::optional<float> {
    float value = 0.0f;
    if (!ReadEnvFloat(name, &value)) {
      return std::nullopt;
    }
    return value;
  };
  auto maybe_int = [](const char* name) -> std::optional<int> {
    int value = 0;
    if (!ReadEnvInt(name, &value)) {
      return std::nullopt;
    }
    return value;
  };

  overrides.stage1_psf_sigma = maybe_float("HIRACO_STAGE1_PSF_SIGMA");
  overrides.stage1_nsr = maybe_float("HIRACO_STAGE1_NSR");
  overrides.stage2_denoise = maybe_float("HIRACO_STAGE2_DENOISE");
  overrides.stage2_gain0 = maybe_float("HIRACO_STAGE2_GAIN0");
  overrides.stage2_gain1 = maybe_float("HIRACO_STAGE2_GAIN1");
  overrides.stage2_gain2 = maybe_float("HIRACO_STAGE2_GAIN2");
  overrides.stage2_gain3 = maybe_float("HIRACO_STAGE2_GAIN3");
  overrides.stage3_radius = maybe_int("HIRACO_STAGE3_RADIUS");
  overrides.stage3_gain = maybe_float("HIRACO_STAGE3_GAIN");
  return overrides;
}

LibRawOverrideSet ReadLibRawOverridesFromEnvironment() {
  LibRawOverrideSet overrides;
  auto maybe_float = [](const char* name) -> std::optional<float> {
    float value = 0.0f;
    if (!ReadEnvFloat(name, &value)) {
      return std::nullopt;
    }
    return value;
  };
  auto maybe_int = [](const char* name) -> std::optional<int> {
    int value = 0;
    if (!ReadEnvInt(name, &value)) {
      return std::nullopt;
    }
    return value;
  };

  overrides.user_qual = maybe_int("HIRACO_LIBRAW_USER_QUAL");
  overrides.four_color_rgb = maybe_int("HIRACO_LIBRAW_FOUR_COLOR_RGB");
  overrides.green_matching = maybe_int("HIRACO_LIBRAW_GREEN_MATCHING");
  overrides.med_passes = maybe_int("HIRACO_LIBRAW_MED_PASSES");
  overrides.no_auto_scale = maybe_int("HIRACO_LIBRAW_NO_AUTO_SCALE");
  overrides.no_auto_bright = maybe_int("HIRACO_LIBRAW_NO_AUTO_BRIGHT");
  overrides.adjust_maximum_thr = maybe_float("HIRACO_LIBRAW_ADJUST_MAXIMUM_THR");
  return overrides;
}
