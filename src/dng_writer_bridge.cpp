#include <iostream>

#include "dng_writer_bridge.h"
#include "hiraco_timing.h"

#if defined(HIRACO_ENABLE_DNG_SDK) && HIRACO_ENABLE_DNG_SDK

#include "dng_auto_ptr.h"
#include "dng_abort_sniffer.h"
#include "dng_camera_profile.h"
#include "dng_exceptions.h"
#include "dng_exif.h"
#include "dng_file_stream.h"
#include "dng_host.h"
#include "dng_image.h"
#include "dng_image_writer.h"
#include "dng_matrix.h"
#include "dng_negative.h"
#include "dng_pixel_buffer.h"
#include "dng_preview.h"
#include "dng_tag_values.h"
#include "dng_xy_coord.h"

#include <algorithm>
#include <array>
#include <atomic>
#include <cmath>
#include <cstdlib>
#include <filesystem>
#include <fftw3.h>
#include <fstream>
#include <future>
#include <libraw/libraw.h>
#include <memory>
#include <string>
#include <thread>
#include <vector>

#include "HalideBuffer.h"
#include "hiraco_atrous_wavelet.h"
#include "hiraco_guided_filter.h"

namespace {

struct LinearDngPayload {
  RasterImage raw_image;
  RasterImage cfa_guide_image;
  bool raw_image_is_camera_space = false;
};

struct GuideExtractionResult {
  bool ok = false;
  RasterImage guide_image;
  std::string error;
};

struct RenderSettings {
  int output_color = 1;
  int use_camera_wb = 1;
  int use_camera_matrix = 1;
  int no_auto_scale = 0;
  int no_auto_bright = 1;
  int user_flip = 0;
  int user_qual = -1;
  int half_size = 0;
  int four_color_rgb = 0;
  int green_matching = 0;
  int med_passes = 0;
  float adjust_maximum_thr = 0.75f;
  float gamma_power = 1.0f;
  float gamma_slope = 1.0f;
};

bool IsOm3HighResMetadata(const SourceLinearDngMetadata& metadata) {
  // 50 MP hand-held high-res (8172×6132 raw, 8160×6120 crop, origin 6,6)
  if (metadata.default_crop_origin_h == 6 &&
      metadata.default_crop_origin_v == 6 &&
      metadata.default_crop_width == 8160 &&
      metadata.default_crop_height == 6120) return true;
  // 80 MP tripod high-res (10386×7792 raw, 10368×7776 crop, origin 8,8)
  if (metadata.default_crop_origin_h == 8 &&
      metadata.default_crop_origin_v == 8 &&
      metadata.default_crop_width == 10368 &&
      metadata.default_crop_height == 7776) return true;
  return false;
}

bool IsCancelled(const CancelCheck& cancel) {
  return static_cast<bool>(cancel) && cancel();
}

bool CheckCancelled(const CancelCheck& cancel, std::string* error_message) {
  if (!IsCancelled(cancel)) {
    return false;
  }
  if (error_message != nullptr) {
    *error_message = "operation canceled";
  }
  return true;
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

bool Is80MpFrame(uint32_t width, uint32_t height) {
  return width == 10386 && height == 7792;
}

constexpr float kStage2GainMin = 0.25f;
constexpr float kStage2GainMax = 4.0f;
constexpr float kStage2FineFromSmallScale = 1.6f;

float ClampStage2Gain(float gain) {
  return std::clamp(gain, kStage2GainMin, kStage2GainMax);
}

float DeriveFineScaleGainFromSmall(float small_gain) {
  return ClampStage2Gain(1.0f + kStage2FineFromSmallScale * (small_gain - 1.0f));
}

void FinalizeStage2Settings(ResolvedStageSettings* settings) {
  settings->stage2_gain1 = ClampStage2Gain(settings->stage2_gain1);
  settings->stage2_gain2 = ClampStage2Gain(settings->stage2_gain2);
  settings->stage2_gain3 = ClampStage2Gain(settings->stage2_gain3);
}

ResolvedStageSettings ResolveStageSettingsForImageImpl(const SourceLinearDngMetadata& metadata,
                                                       uint32_t width,
                                                       uint32_t height,
                                                       const StageOverrideSet& overrides) {
  (void) metadata;

  ResolvedStageSettings settings;
  if (Is80MpFrame(width, height)) {
    settings.stage1_psf_sigma = 2.5f;
  }

  if (overrides.stage1_psf_sigma.has_value()) {
    settings.stage1_psf_sigma = *overrides.stage1_psf_sigma;
  }
  if (overrides.stage1_nsr.has_value()) {
    settings.stage1_nsr = *overrides.stage1_nsr;
  }
  if (overrides.stage2_denoise.has_value()) {
    settings.stage2_denoise = *overrides.stage2_denoise;
  }
  if (overrides.stage2_gain1.has_value()) {
    settings.stage2_gain1 = *overrides.stage2_gain1;
  }
  if (overrides.stage2_gain2.has_value()) {
    settings.stage2_gain2 = *overrides.stage2_gain2;
  }
  if (overrides.stage2_gain3.has_value()) {
    settings.stage2_gain3 = *overrides.stage2_gain3;
  }
  if (overrides.stage3_radius.has_value()) {
    settings.stage3_radius = *overrides.stage3_radius;
  }
  if (overrides.stage3_gain.has_value()) {
    settings.stage3_gain = *overrides.stage3_gain;
  }

  FinalizeStage2Settings(&settings);

  return settings;
}

// --- FFTW threading and wisdom management ---

bool g_fftw_threads_initialized = false;

void InitFftwThreads() {
  if (g_fftw_threads_initialized) return;
  if (fftw_init_threads()) {
    const int num_threads = static_cast<int>(std::thread::hardware_concurrency());
    fftw_plan_with_nthreads(std::max(num_threads, 1));
    std::cerr << "[hiraco] FFTW using " << std::max(num_threads, 1) << " threads\n";
  }
  g_fftw_threads_initialized = true;
}

std::string FftwWisdomPath() {
  const char* home = std::getenv("HOME");
  if (!home) home = std::getenv("USERPROFILE");
  if (home) return std::string(home) + "/.hiraco_fftw_wisdom";
  return "";
}

void LoadFftwWisdom() {
  const std::string path = FftwWisdomPath();
  if (!path.empty()) {
    fftw_import_wisdom_from_filename(path.c_str());
  }
}

void SaveFftwWisdom() {
  const std::string path = FftwWisdomPath();
  if (!path.empty()) {
    fftw_export_wisdom_to_filename(path.c_str());
  }
}

double RawDomainBlackLevel(const LibRaw& processor) {
  const unsigned black = processor.imgdata.color.black;
  if (black > 0) {
    return static_cast<double>(black);
  }

  double sum = 0.0;
  int count = 0;
  for (int index = 0; index < 4; ++index) {
    if (processor.imgdata.color.cblack[index] > 0) {
      sum += static_cast<double>(processor.imgdata.color.cblack[index]);
      ++count;
    }
  }
  return count > 0 ? sum / count : 0.0;
}

double RawDomainWhiteLevel(const LibRaw& processor) {
  const unsigned maximum = processor.imgdata.color.maximum;
  if (maximum > 0) {
    return static_cast<double>(maximum);
  }

  unsigned linear_max = 0;
  for (int index = 0; index < 4; ++index) {
    linear_max = std::max(linear_max, processor.imgdata.color.linear_max[index]);
  }
  if (linear_max > 0) {
    return static_cast<double>(linear_max);
  }

  const unsigned raw_bps = processor.imgdata.color.raw_bps;
  if (raw_bps > 0 && raw_bps < 31) {
    return static_cast<double>((1u << raw_bps) - 1u);
  }

  return 16383.0;
}

bool LoadFloatMap(const std::vector<float>& embedded_data,
                  const std::string& path,
                  uint32_t width,
                  uint32_t height,
                  const char* label,
                  std::vector<double>* output,
                  std::string* error_message) {
  if (output == nullptr) {
    return false;
  }
  output->clear();

  if (path.empty() || width == 0 || height == 0) {
    if (embedded_data.empty()) {
      return true;
    }
  }

  const size_t sample_count = static_cast<size_t>(width) * height;
  std::vector<float> raw_map(sample_count, 0.0f);
  if (!embedded_data.empty()) {
    if (embedded_data.size() != sample_count) {
      *error_message = std::string("Unexpected sample count for ") + label;
      return false;
    }
    raw_map = embedded_data;
  } else {
    std::ifstream input(path, std::ios::binary);
    if (!input) {
      *error_message = std::string("Failed to open ") + label;
      return false;
    }

    input.read(reinterpret_cast<char*>(raw_map.data()),
               static_cast<std::streamsize>(sample_count * sizeof(float)));
    if (!input || input.gcount() != static_cast<std::streamsize>(sample_count * sizeof(float))) {
      *error_message = std::string("Failed to read ") + label;
      return false;
    }
  }

  output->resize(sample_count, 0.0);
  for (size_t i = 0; i < sample_count; ++i) {
    (*output)[i] = static_cast<double>(raw_map[i]);
  }
  return true;
}

bool LoadStackStabilityMap(const SourceLinearDngMetadata& metadata,
                           std::vector<double>* stability_map,
                           std::string* error_message) {
  if (stability_map == nullptr) {
    return false;
  }
  stability_map->clear();

  if (!metadata.has_stack_stability_map ||
      metadata.stack_stability_width == 0 ||
      metadata.stack_stability_height == 0 ||
      (metadata.stack_stability_path.empty() && metadata.stack_stability_data.empty())) {
    return true;
  }

  if (!LoadFloatMap(metadata.stack_stability_data,
                    metadata.stack_stability_path,
                    metadata.stack_stability_width,
                    metadata.stack_stability_height,
                    "stack stability map",
                    stability_map,
                    error_message)) {
    return false;
  }

  for (double& value : *stability_map) {
    value = std::clamp(value, 0.0, 1.0);
  }
  return true;
}

bool LoadStackMeanMap(const SourceLinearDngMetadata& metadata,
                      std::vector<double>* mean_map,
                      std::string* error_message) {
  if (mean_map == nullptr) {
    return false;
  }
  mean_map->clear();

  if (!metadata.has_stack_mean_map ||
      metadata.stack_mean_width == 0 ||
      metadata.stack_mean_height == 0 ||
      (metadata.stack_mean_path.empty() && metadata.stack_mean_data.empty())) {
    return true;
  }

  return LoadFloatMap(metadata.stack_mean_data,
                      metadata.stack_mean_path,
                      metadata.stack_mean_width,
                      metadata.stack_mean_height,
                      "stack mean map",
                      mean_map,
                      error_message);
}

bool LoadStackGuideMap(const SourceLinearDngMetadata& metadata,
                       std::vector<double>* guide_map,
                       std::string* error_message) {
  if (guide_map == nullptr) {
    return false;
  }
  guide_map->clear();

  if (metadata.has_stack_guide_map &&
      metadata.stack_guide_width > 0 &&
      metadata.stack_guide_height > 0 &&
      (!metadata.stack_guide_path.empty() || !metadata.stack_guide_data.empty())) {
    return LoadFloatMap(metadata.stack_guide_data,
                        metadata.stack_guide_path,
                        metadata.stack_guide_width,
                        metadata.stack_guide_height,
                        "stack guide map",
                        guide_map,
                        error_message);
  }

  return LoadStackMeanMap(metadata, guide_map, error_message);
}

bool LoadStackTensorXMap(const SourceLinearDngMetadata& metadata,
                         std::vector<double>* tensor_map,
                         std::string* error_message) {
  if (tensor_map == nullptr) {
    return false;
  }
  tensor_map->clear();

  if (!metadata.has_stack_tensor_x_map ||
      metadata.stack_tensor_x_width == 0 ||
      metadata.stack_tensor_x_height == 0 ||
      (metadata.stack_tensor_x_path.empty() && metadata.stack_tensor_x_data.empty())) {
    return true;
  }

  if (!LoadFloatMap(metadata.stack_tensor_x_data,
                    metadata.stack_tensor_x_path,
                    metadata.stack_tensor_x_width,
                    metadata.stack_tensor_x_height,
                    "stack tensor-x map",
                    tensor_map,
                    error_message)) {
    return false;
  }
  for (double& value : *tensor_map) {
    value = std::clamp(value, 0.0, 1.0);
  }
  return true;
}

bool LoadStackTensorYMap(const SourceLinearDngMetadata& metadata,
                         std::vector<double>* tensor_map,
                         std::string* error_message) {
  if (tensor_map == nullptr) {
    return false;
  }
  tensor_map->clear();

  if (!metadata.has_stack_tensor_y_map ||
      metadata.stack_tensor_y_width == 0 ||
      metadata.stack_tensor_y_height == 0 ||
      (metadata.stack_tensor_y_path.empty() && metadata.stack_tensor_y_data.empty())) {
    return true;
  }

  if (!LoadFloatMap(metadata.stack_tensor_y_data,
                    metadata.stack_tensor_y_path,
                    metadata.stack_tensor_y_width,
                    metadata.stack_tensor_y_height,
                    "stack tensor-y map",
                    tensor_map,
                    error_message)) {
    return false;
  }
  for (double& value : *tensor_map) {
    value = std::clamp(value, 0.0, 1.0);
  }
  return true;
}

bool LoadStackTensorCoherenceMap(const SourceLinearDngMetadata& metadata,
                                 std::vector<double>* coherence_map,
                                 std::string* error_message) {
  if (coherence_map == nullptr) {
    return false;
  }
  coherence_map->clear();

  if (!metadata.has_stack_tensor_coherence_map ||
      metadata.stack_tensor_coherence_width == 0 ||
      metadata.stack_tensor_coherence_height == 0 ||
      (metadata.stack_tensor_coherence_path.empty() && metadata.stack_tensor_coherence_data.empty())) {
    return true;
  }

  if (!LoadFloatMap(metadata.stack_tensor_coherence_data,
                    metadata.stack_tensor_coherence_path,
                    metadata.stack_tensor_coherence_width,
                    metadata.stack_tensor_coherence_height,
                    "stack tensor-coherence map",
                    coherence_map,
                    error_message)) {
    return false;
  }
  for (double& value : *coherence_map) {
    value = std::clamp(value, 0.0, 1.0);
  }
  return true;
}

bool LoadStackAliasMap(const SourceLinearDngMetadata& metadata,
                       std::vector<double>* alias_map,
                       std::string* error_message) {
  if (alias_map == nullptr) {
    return false;
  }
  alias_map->clear();

  if (!metadata.has_stack_alias_map ||
      metadata.stack_alias_width == 0 ||
      metadata.stack_alias_height == 0 ||
      (metadata.stack_alias_path.empty() && metadata.stack_alias_data.empty())) {
    return true;
  }

  if (!LoadFloatMap(metadata.stack_alias_data,
                    metadata.stack_alias_path,
                    metadata.stack_alias_width,
                    metadata.stack_alias_height,
                    "stack alias map",
                    alias_map,
                    error_message)) {
    return false;
  }

  for (double& value : *alias_map) {
    value = std::clamp(value, 0.0, 1.0);
  }
  return true;
}

void UpsampleFloatMap(const std::vector<double>& source,
                      uint32_t source_width,
                      uint32_t source_height,
                      uint32_t target_width,
                      uint32_t target_height,
                      std::vector<double>* target) {
  if (target == nullptr) {
    return;
  }
  target->clear();

  if (source.empty() || source_width == 0 || source_height == 0 ||
      target_width == 0 || target_height == 0) {
    return;
  }

  target->resize(static_cast<size_t>(target_width) * target_height, 0.0);

  const double scale_x = static_cast<double>(source_width) / static_cast<double>(target_width);
  const double scale_y = static_cast<double>(source_height) / static_cast<double>(target_height);
  #pragma omp parallel for schedule(static) if(target_height > 100)
  for (uint32_t row = 0; row < target_height; ++row) {
    const double y = (static_cast<double>(row) + 0.5) * scale_y - 0.5;
    const int y0 = static_cast<int>(std::floor(y));
    const int y1 = y0 + 1;
    const double wy = y - std::floor(y);
    const uint32_t sy0 = static_cast<uint32_t>(std::clamp(y0, 0, static_cast<int>(source_height) - 1));
    const uint32_t sy1 = static_cast<uint32_t>(std::clamp(y1, 0, static_cast<int>(source_height) - 1));
    for (uint32_t col = 0; col < target_width; ++col) {
      const double x = (static_cast<double>(col) + 0.5) * scale_x - 0.5;
      const int x0 = static_cast<int>(std::floor(x));
      const int x1 = x0 + 1;
      const double wx = x - std::floor(x);
      const uint32_t sx0 = static_cast<uint32_t>(std::clamp(x0, 0, static_cast<int>(source_width) - 1));
      const uint32_t sx1 = static_cast<uint32_t>(std::clamp(x1, 0, static_cast<int>(source_width) - 1));

      const double v00 = source[static_cast<size_t>(sy0) * source_width + sx0];
      const double v01 = source[static_cast<size_t>(sy0) * source_width + sx1];
      const double v10 = source[static_cast<size_t>(sy1) * source_width + sx0];
      const double v11 = source[static_cast<size_t>(sy1) * source_width + sx1];
      const double top = (1.0 - wx) * v00 + wx * v01;
      const double bottom = (1.0 - wx) * v10 + wx * v11;
      (*target)[static_cast<size_t>(row) * target_width + col] =
          (1.0 - wy) * top + wy * bottom;
    }
  }
}

void UpsampleFloatMapRegion(const std::vector<double>& source,
                            uint32_t source_width,
                            uint32_t source_height,
                            uint32_t full_width,
                            uint32_t full_height,
                            const CropRect& region,
                            std::vector<double>* target) {
  if (target == nullptr) {
    return;
  }
  target->clear();

  if (source.empty() || source_width == 0 || source_height == 0 ||
      full_width == 0 || full_height == 0 ||
      region.width == 0 || region.height == 0) {
    return;
  }

  target->resize(static_cast<size_t>(region.width) * region.height, 0.0);

  const double scale_x = static_cast<double>(source_width) / static_cast<double>(full_width);
  const double scale_y = static_cast<double>(source_height) / static_cast<double>(full_height);
  #pragma omp parallel for schedule(static) if(region.height > 100)
  for (uint32_t row = 0; row < region.height; ++row) {
    const double y = (static_cast<double>(region.y + row) + 0.5) * scale_y - 0.5;
    const int y0 = static_cast<int>(std::floor(y));
    const int y1 = y0 + 1;
    const double wy = y - std::floor(y);
    const uint32_t sy0 = static_cast<uint32_t>(std::clamp(y0, 0, static_cast<int>(source_height) - 1));
    const uint32_t sy1 = static_cast<uint32_t>(std::clamp(y1, 0, static_cast<int>(source_height) - 1));
    for (uint32_t col = 0; col < region.width; ++col) {
      const double x = (static_cast<double>(region.x + col) + 0.5) * scale_x - 0.5;
      const int x0 = static_cast<int>(std::floor(x));
      const int x1 = x0 + 1;
      const double wx = x - std::floor(x);
      const uint32_t sx0 = static_cast<uint32_t>(std::clamp(x0, 0, static_cast<int>(source_width) - 1));
      const uint32_t sx1 = static_cast<uint32_t>(std::clamp(x1, 0, static_cast<int>(source_width) - 1));

      const double v00 = source[static_cast<size_t>(sy0) * source_width + sx0];
      const double v01 = source[static_cast<size_t>(sy0) * source_width + sx1];
      const double v10 = source[static_cast<size_t>(sy1) * source_width + sx0];
      const double v11 = source[static_cast<size_t>(sy1) * source_width + sx1];
      const double top = (1.0 - wx) * v00 + wx * v01;
      const double bottom = (1.0 - wx) * v10 + wx * v11;
      (*target)[static_cast<size_t>(row) * region.width + col] =
          (1.0 - wy) * top + wy * bottom;
    }
  }
}

struct BilinearAxisSample {
  uint32_t lower = 0;
  uint32_t upper = 0;
  double lower_weight = 1.0;
  double upper_weight = 0.0;
};

struct RegionMapSampler {
  const std::vector<double>* source = nullptr;
  uint32_t source_width = 0;
  std::vector<BilinearAxisSample> row_samples;
  std::vector<BilinearAxisSample> col_samples;
};

void BuildRegionBilinearSamples(uint32_t source_size,
                                uint32_t full_size,
                                uint32_t region_origin,
                                uint32_t region_extent,
                                std::vector<BilinearAxisSample>* samples) {
  if (samples == nullptr) {
    return;
  }
  samples->clear();

  if (source_size == 0 || full_size == 0 || region_extent == 0) {
    return;
  }

  samples->resize(region_extent);
  const double scale = static_cast<double>(source_size) / static_cast<double>(full_size);
  for (uint32_t index = 0; index < region_extent; ++index) {
    const double position = (static_cast<double>(region_origin + index) + 0.5) * scale - 0.5;
    const int lower = static_cast<int>(std::floor(position));
    const int upper = lower + 1;
    const double upper_weight = position - std::floor(position);

    BilinearAxisSample sample;
    sample.lower = static_cast<uint32_t>(std::clamp(lower, 0, static_cast<int>(source_size) - 1));
    sample.upper = static_cast<uint32_t>(std::clamp(upper, 0, static_cast<int>(source_size) - 1));
    sample.upper_weight = upper_weight;
    sample.lower_weight = 1.0 - upper_weight;
    (*samples)[index] = sample;
  }
}

RegionMapSampler MakeRegionMapSampler(const std::vector<double>& source,
                                      uint32_t source_width,
                                      uint32_t source_height,
                                      uint32_t full_width,
                                      uint32_t full_height,
                                      const CropRect& region) {
  RegionMapSampler sampler;
  if (source.empty() || source_width == 0 || source_height == 0 ||
      full_width == 0 || full_height == 0 ||
      region.width == 0 || region.height == 0) {
    return sampler;
  }

  sampler.source = &source;
  sampler.source_width = source_width;
  BuildRegionBilinearSamples(source_height,
                             full_height,
                             region.y,
                             region.height,
                             &sampler.row_samples);
  BuildRegionBilinearSamples(source_width,
                             full_width,
                             region.x,
                             region.width,
                             &sampler.col_samples);
  return sampler;
}

bool HasRegionMapSampler(const RegionMapSampler& sampler) {
  return sampler.source != nullptr &&
         !sampler.source->empty() &&
         sampler.source_width > 0 &&
         !sampler.row_samples.empty() &&
         !sampler.col_samples.empty();
}

double SampleRegionMap(const RegionMapSampler& sampler,
                       uint32_t row,
                       uint32_t col,
                       double fallback) {
  if (!HasRegionMapSampler(sampler)) {
    return fallback;
  }

  const BilinearAxisSample& row_sample = sampler.row_samples[row];
  const BilinearAxisSample& col_sample = sampler.col_samples[col];
  const size_t row0 = static_cast<size_t>(row_sample.lower) * sampler.source_width;
  const size_t row1 = static_cast<size_t>(row_sample.upper) * sampler.source_width;

  const double v00 = (*sampler.source)[row0 + col_sample.lower];
  const double v01 = (*sampler.source)[row0 + col_sample.upper];
  const double v10 = (*sampler.source)[row1 + col_sample.lower];
  const double v11 = (*sampler.source)[row1 + col_sample.upper];
  const double top = col_sample.lower_weight * v00 + col_sample.upper_weight * v01;
  const double bottom = col_sample.lower_weight * v10 + col_sample.upper_weight * v11;
  return row_sample.lower_weight * top + row_sample.upper_weight * bottom;
}

void ApplyLibRawOverrides(const LibRawOverrideSet& overrides, RenderSettings* settings) {
  if (overrides.user_qual.has_value()) {
    settings->user_qual = *overrides.user_qual;
  }
  if (overrides.four_color_rgb.has_value()) {
    settings->four_color_rgb = *overrides.four_color_rgb;
  }
  if (overrides.green_matching.has_value()) {
    settings->green_matching = *overrides.green_matching;
  }
  if (overrides.med_passes.has_value()) {
    settings->med_passes = *overrides.med_passes;
  }
  if (overrides.no_auto_scale.has_value()) {
    settings->no_auto_scale = *overrides.no_auto_scale;
  }
  if (overrides.no_auto_bright.has_value()) {
    settings->no_auto_bright = *overrides.no_auto_bright;
  }
  if (overrides.adjust_maximum_thr.has_value()) {
    settings->adjust_maximum_thr = *overrides.adjust_maximum_thr;
  }
}

RenderSettings BuildRawRenderSettings(const SourceLinearDngMetadata& metadata,
                                     const LibRawOverrideSet& libraw_overrides) {
  RenderSettings settings;
  settings.output_color = 1;
  settings.use_camera_wb = 1;
  settings.use_camera_matrix = 1;
  settings.no_auto_scale = 0;
  settings.no_auto_bright = 1;
  settings.user_flip = 0;
  settings.user_qual = -1;
  settings.four_color_rgb = 0;
  settings.green_matching = 0;
  settings.med_passes = 0;
  settings.adjust_maximum_thr = 0.75f;
  settings.gamma_power = 1.0f;
  settings.gamma_slope = 1.0f;

  if (IsOm3HighResMetadata(metadata)) {
    settings.user_qual = 12;
    settings.four_color_rgb = 0;
    settings.green_matching = 0;
  }

  ApplyLibRawOverrides(libraw_overrides, &settings);
  return settings;
}

RenderSettings BuildPreviewRenderSettings(const SourceLinearDngMetadata& metadata,
                                         const LibRawOverrideSet& libraw_overrides) {
  RenderSettings settings;
  settings.half_size = 1;
  settings.output_color = 1;
  settings.use_camera_wb = 1;
  settings.use_camera_matrix = 1;
  settings.no_auto_scale = 0;
  settings.no_auto_bright = 0;
  settings.user_flip = 0;
  settings.user_qual = -1;
  settings.four_color_rgb = 0;
  settings.green_matching = 0;
  settings.med_passes = 0;
  settings.adjust_maximum_thr = 0.75f;
  settings.gamma_power = 0.45f;
  settings.gamma_slope = 4.5f;

  if (IsOm3HighResMetadata(metadata)) {
    settings.user_qual = 12;
    settings.four_color_rgb = 0;
    settings.green_matching = 0;
  }

  ApplyLibRawOverrides(libraw_overrides, &settings);
  return settings;
}

RenderSettings BuildLinearPreviewProbeSettings(const SourceLinearDngMetadata& metadata,
                                               const LibRawOverrideSet& libraw_overrides) {
  RenderSettings settings = BuildPreviewRenderSettings(metadata, libraw_overrides);
  settings.no_auto_bright = 1;
  settings.gamma_power = 1.0f;
  settings.gamma_slope = 1.0f;
  return settings;
}

RenderSettings BuildCfaGuideRenderSettings(const SourceLinearDngMetadata& metadata,
                                          const LibRawOverrideSet& libraw_overrides) {
  return BuildRawRenderSettings(metadata, libraw_overrides);
}

bool ShouldApplyOm3SourceDrivenLinearTransform(const SourceLinearDngMetadata& metadata,
                                               const RasterImage& image) {
  if (!metadata.has_black_level || !metadata.has_as_shot_neutral) {
    return false;
  }

  const bool is_50mp_high_res = metadata.default_crop_origin_h == 6 &&
                                metadata.default_crop_origin_v == 6 &&
                                metadata.default_crop_width == 8160 &&
                                metadata.default_crop_height == 6120 &&
                                image.width == 8172 &&
                                image.height == 6132;
  const bool is_80mp_high_res = metadata.default_crop_origin_h == 8 &&
                                metadata.default_crop_origin_v == 8 &&
                                metadata.default_crop_width == 10368 &&
                                metadata.default_crop_height == 7776 &&
                                image.width == 10386 &&
                                image.height == 7792;
  const bool is_standard_20mp = metadata.default_crop_origin_h == 12 &&
                                metadata.default_crop_origin_v == 12 &&
                                metadata.default_crop_width == 5184 &&
                                metadata.default_crop_height == 3888 &&
                                image.width == 5220 &&
                                image.height == 3912;
  return is_50mp_high_res || is_80mp_high_res || is_standard_20mp;
}

bool IsOm3HighResRaster(const SourceLinearDngMetadata& metadata,
                        const RasterImage& image) {
  if (image.colors != 3) return false;
  // 50 MP hand-held
  if (metadata.default_crop_origin_h == 6 &&
      metadata.default_crop_origin_v == 6 &&
      metadata.default_crop_width == 8160 &&
      metadata.default_crop_height == 6120 &&
      image.width == 8172 &&
      image.height == 6132) return true;
  // 80 MP tripod
  if (metadata.default_crop_origin_h == 8 &&
      metadata.default_crop_origin_v == 8 &&
      metadata.default_crop_width == 10368 &&
      metadata.default_crop_height == 7776 &&
      image.width == 10386 &&
      image.height == 7792) return true;
  return false;
}

bool ShouldApplyPredictedDetailGain(const SourceLinearDngMetadata& metadata,
                                    const RasterImage& image) {
  return metadata.has_predicted_detail_gain &&
         metadata.predicted_detail_gain > 1.0001 &&
         image.colors == 3 &&
         IsOm3HighResMetadata(metadata);
}

bool ShouldUseOm3AdobeMetadata(const SourceLinearDngMetadata& metadata,
                               const RasterImage& image) {
  if (!metadata.has_black_level || !metadata.has_as_shot_neutral) {
    return false;
  }

  const bool is_50mp_high_res = metadata.default_crop_origin_h == 6 &&
                                metadata.default_crop_origin_v == 6 &&
                                metadata.default_crop_width == 8160 &&
                                metadata.default_crop_height == 6120 &&
                                image.width == 8172 &&
                                image.height == 6132;
  const bool is_80mp_high_res = metadata.default_crop_origin_h == 8 &&
                                metadata.default_crop_origin_v == 8 &&
                                metadata.default_crop_width == 10368 &&
                                metadata.default_crop_height == 7776 &&
                                image.width == 10386 &&
                                image.height == 7792;
  const bool is_standard_20mp = metadata.default_crop_origin_h == 12 &&
                                metadata.default_crop_origin_v == 12 &&
                                metadata.default_crop_width == 5184 &&
                                metadata.default_crop_height == 3888 &&
                                image.width == 5220 &&
                                image.height == 3912;
  return is_50mp_high_res || is_80mp_high_res || is_standard_20mp;
}

double SourceDrivenBaselineExposure(const SourceLinearDngMetadata& metadata,
                                    const RasterImage& image) {
  (void)metadata;
  (void)image;
  return 0.0;
}

void ApplySourceDrivenLinearPreviewTransform(const SourceLinearDngMetadata& metadata,
                                             RasterImage* image) {
  if (image == nullptr || image->colors < 3 || !metadata.has_black_level || !metadata.has_as_shot_neutral) {
    return;
  }

  const double pedestal = metadata.black_level;
  const double range = std::max(65535.0 - pedestal, 1.0);
  std::array<double, 9> rgb_cam = {1.0, 0.0, 0.0,
                                   0.0, 1.0, 0.0,
                                   0.0, 0.0, 1.0};
  if (metadata.has_rgb_cam) {
    for (size_t index = 0; index < 9; ++index) {
      rgb_cam[index] = metadata.rgb_cam[index];
    }
  }

  const auto multiply_rgb_cam = [&](const std::array<double, 3>& vector) {
    return std::array<double, 3>{
        rgb_cam[0] * vector[0] + rgb_cam[1] * vector[1] + rgb_cam[2] * vector[2],
        rgb_cam[3] * vector[0] + rgb_cam[4] * vector[1] + rgb_cam[5] * vector[2],
        rgb_cam[6] * vector[0] + rgb_cam[7] * vector[1] + rgb_cam[8] * vector[2],
    };
  };

  std::array<double, 3> neutral_white = multiply_rgb_cam({1.0, 1.0, 1.0});
  for (double& channel : neutral_white) {
    channel = std::max(channel, 1e-6);
  }

  const size_t pixel_count = static_cast<size_t>(image->width) * static_cast<size_t>(image->height);
  #pragma omp parallel for schedule(static) if(pixel_count > 100000)
  for (size_t index = 0; index < pixel_count; ++index) {
    const size_t sample_index = index * image->colors;
    const std::array<double, 3> camera = {
        std::max(static_cast<double>(image->pixels[sample_index + 0]) - pedestal, 0.0) / range,
        std::max(static_cast<double>(image->pixels[sample_index + 1]) - pedestal, 0.0) / range,
        std::max(static_cast<double>(image->pixels[sample_index + 2]) - pedestal, 0.0) / range,
    };
    const std::array<double, 3> balanced = {
        camera[0] / std::max(metadata.as_shot_neutral[0], 1e-6),
        camera[1] / std::max(metadata.as_shot_neutral[1], 1e-6),
        camera[2] / std::max(metadata.as_shot_neutral[2], 1e-6),
    };
    std::array<double, 3> linear_rgb = multiply_rgb_cam(balanced);
    linear_rgb[0] /= neutral_white[0];
    linear_rgb[1] /= neutral_white[1];
    linear_rgb[2] /= neutral_white[2];
    for (size_t channel = 0; channel < 3; ++channel) {
      image->pixels[sample_index + channel] = static_cast<uint16_t>(
          std::clamp(linear_rgb[channel] * 65535.0, 0.0, 65535.0));
    }
  }
}

void ApplyLinearSrgbGamma(RasterImage* image) {
  if (image == nullptr || image->colors < 3) {
    return;
  }

  const size_t pixel_count = static_cast<size_t>(image->width) * image->height;
  #pragma omp parallel for schedule(static) if(pixel_count > 100000)
  for (size_t index = 0; index < pixel_count; ++index) {
    const size_t sample_index = index * image->colors;
    for (size_t channel = 0; channel < 3; ++channel) {
      const double linear = static_cast<double>(image->pixels[sample_index + channel]) / 65535.0;
      const double srgb = linear <= 0.0031308
          ? linear * 12.92
          : 1.055 * std::pow(linear, 1.0 / 2.4) - 0.055;
      image->pixels[sample_index + channel] = static_cast<uint16_t>(
          std::clamp(srgb * 65535.0, 0.0, 65535.0));
    }
  }
}

void ApplyLinearExposureCompensation(double exposure_ev, RasterImage* image) {
  if (image == nullptr || image->colors < 3 || std::abs(exposure_ev) < 1e-9) {
    return;
  }

  const double multiplier = std::pow(2.0, exposure_ev);
  const size_t pixel_count = static_cast<size_t>(image->width) * image->height;
  #pragma omp parallel for schedule(static) if(pixel_count > 100000)
  for (size_t index = 0; index < pixel_count; ++index) {
    const size_t sample_index = index * image->colors;
    for (size_t channel = 0; channel < 3; ++channel) {
      image->pixels[sample_index + channel] = static_cast<uint16_t>(
          std::clamp(static_cast<double>(image->pixels[sample_index + channel]) * multiplier,
                     0.0,
                     65535.0));
    }
  }
}

void ApplyLinearGain(double gain, RasterImage* image) {
  if (image == nullptr || image->colors < 3 || std::abs(gain - 1.0) < 1e-9) {
    return;
  }

  const size_t pixel_count = static_cast<size_t>(image->width) * image->height;
  #pragma omp parallel for schedule(static) if(pixel_count > 100000)
  for (size_t index = 0; index < pixel_count; ++index) {
    const size_t sample_index = index * image->colors;
    for (size_t channel = 0; channel < 3; ++channel) {
      image->pixels[sample_index + channel] = static_cast<uint16_t>(
          std::clamp(static_cast<double>(image->pixels[sample_index + channel]) * gain,
                     0.0,
                     65535.0));
    }
  }
}

void ApplyLinearDngRasterTransform(const SourceLinearDngMetadata& metadata,
                                   RasterImage* image) {
  if (ShouldApplyOm3SourceDrivenLinearTransform(metadata, *image)) {
    ApplySourceDrivenLinearPreviewTransform(metadata, image);
  }
}

bool ApplyPredictedDetailGain(const SourceLinearDngMetadata& metadata,
                              const ResolvedStageSettings& settings,
                              const RasterImage* cfa_guide_image,
                              RasterImage* image,
                              ProgressCallback progress,
                              CancelCheck cancel,
                              std::string* error_message) {
  const hiraco::ScopedTimingLog enhance_timer("enhance", "Apply predicted detail gain");
  if (!ShouldApplyPredictedDetailGain(metadata, *image)) {
    return true;
  }

  const double base_gain = std::clamp(metadata.predicted_detail_gain, 1.0, 5.0);
  const uint32_t width = image->width;
  const uint32_t height = image->height;
  const uint32_t colors = image->colors;
  const size_t pixel_count = static_cast<size_t>(width) * height;
  const double stage1_nsr = settings.stage1_nsr;

  if (colors != 3) {
    return true;
  }

  ReportProgress(progress, "enhance", 0.05, "Preparing luma for enhancement");
  if (CheckCancelled(cancel, error_message)) {
    return false;
  }

  const auto stage0_start = std::chrono::steady_clock::now();

  // BT.709 luma coefficients for linear light.
  constexpr double kLumaR = 0.2126;
  constexpr double kLumaG = 0.7152;
  constexpr double kLumaB = 0.0722;

  // --- Stage 0: Extract luma channel (BT.709) ---
  std::vector<double> luma(pixel_count);
  #pragma omp parallel for schedule(static) if(pixel_count > 100000)
  for (size_t i = 0; i < pixel_count; ++i) {
    const size_t px = i * colors;
    luma[i] = kLumaR * image->pixels[px] +
              kLumaG * image->pixels[px + 1] +
              kLumaB * image->pixels[px + 2];
  }

  // Keep a pristine luma copy for Stage 1 blending. Stage 4 recomputes
  // original luma directly from the untouched RGB input.
  std::vector<double> base_luma(luma);

  const bool enable_stage1 = true;  // Wiener deconvolution (FFT-based);
  const bool enable_stage2 = true;  // Spatially varying gain modulation (stack stability/guide maps);
  const bool enable_stage3 = true;  // Spatially varying gain modulation (tensor maps);
  constexpr size_t kRobustStatSampleTarget = 1u << 16;

  std::vector<double> confidence(pixel_count);
  std::vector<double> signal_weight(pixel_count, 1.0);
  std::vector<double> enhancement_weight(pixel_count, 1.0);

  std::vector<uint32_t> row_prev1(height, 0);
  std::vector<uint32_t> row_next1(height, 0);
  for (uint32_t row = 0; row < height; ++row) {
    row_prev1[row] = row > 0 ? row - 1 : 0;
    row_next1[row] = row + 1 < height ? row + 1 : height - 1;
  }
  std::vector<uint32_t> col_prev1(width, 0);
  std::vector<uint32_t> col_next1(width, 0);
  for (uint32_t col = 0; col < width; ++col) {
    col_prev1[col] = col > 0 ? col - 1 : 0;
    col_next1[col] = col + 1 < width ? col + 1 : width - 1;
  }

  auto blur3 = [&](const std::vector<double>& src,
                   std::vector<double>& dst,
                   std::vector<double>* scratch) {
    if (scratch == nullptr) {
      return;
    }
    scratch->resize(pixel_count);
    std::vector<double>& tmp = *scratch;

    #pragma omp parallel for schedule(static) if(height > 100)
    for (uint32_t row = 0; row < height; ++row) {
      const size_t row_off = static_cast<size_t>(row) * width;
      for (uint32_t col = 0; col < width; ++col) {
        const uint32_t c0 = col_prev1[col];
        const uint32_t c2 = col_next1[col];
        tmp[row_off + col] = 0.25 * src[row_off + c0] +
                             0.50 * src[row_off + col] +
                             0.25 * src[row_off + c2];
      }
    }

    #pragma omp parallel for schedule(static) if(height > 100)
    for (uint32_t row = 0; row < height; ++row) {
      const uint32_t r0 = row_prev1[row];
      const uint32_t r2 = row_next1[row];
      const size_t row_off = static_cast<size_t>(row) * width;
      for (uint32_t col = 0; col < width; ++col) {
        dst[row_off + col] = 0.25 * tmp[static_cast<size_t>(r0) * width + col] +
                             0.50 * tmp[row_off + col] +
                             0.25 * tmp[static_cast<size_t>(r2) * width + col];
      }
    }
  };

  auto blur3_pair = [&](const std::vector<double>& src_a,
                        const std::vector<double>& src_b,
                        std::vector<double>& dst_a,
                        std::vector<double>& dst_b,
                        std::vector<double>* scratch_a,
                        std::vector<double>* scratch_b) {
    if (scratch_a == nullptr || scratch_b == nullptr) {
      return;
    }
    scratch_a->resize(pixel_count);
    scratch_b->resize(pixel_count);
    std::vector<double>& tmp_a = *scratch_a;
    std::vector<double>& tmp_b = *scratch_b;

    #pragma omp parallel for schedule(static) if(height > 100)
    for (uint32_t row = 0; row < height; ++row) {
      const size_t row_off = static_cast<size_t>(row) * width;
      for (uint32_t col = 0; col < width; ++col) {
        const uint32_t c0 = col_prev1[col];
        const uint32_t c2 = col_next1[col];
        tmp_a[row_off + col] = 0.25 * src_a[row_off + c0] +
                               0.50 * src_a[row_off + col] +
                               0.25 * src_a[row_off + c2];
        tmp_b[row_off + col] = 0.25 * src_b[row_off + c0] +
                               0.50 * src_b[row_off + col] +
                               0.25 * src_b[row_off + c2];
      }
    }

    #pragma omp parallel for schedule(static) if(height > 100)
    for (uint32_t row = 0; row < height; ++row) {
      const uint32_t r0 = row_prev1[row];
      const uint32_t r2 = row_next1[row];
      const size_t row_off = static_cast<size_t>(row) * width;
      const size_t row0_off = static_cast<size_t>(r0) * width;
      const size_t row2_off = static_cast<size_t>(r2) * width;
      for (uint32_t col = 0; col < width; ++col) {
        dst_a[row_off + col] = 0.25 * tmp_a[row0_off + col] +
                               0.50 * tmp_a[row_off + col] +
                               0.25 * tmp_a[row2_off + col];
        dst_b[row_off + col] = 0.25 * tmp_b[row0_off + col] +
                               0.50 * tmp_b[row_off + col] +
                               0.25 * tmp_b[row2_off + col];
      }
    }
  };

  auto approximate_median_from_samples = [&](size_t sample_target, const auto& sample_value) {
    const size_t desired_samples = std::min(pixel_count, std::max<size_t>(sample_target, 1));
    const size_t sample_stride = std::max<size_t>(1, pixel_count / desired_samples);
    std::vector<double> samples;
    samples.reserve((pixel_count + sample_stride - 1) / sample_stride);
    for (size_t i = 0; i < pixel_count; i += sample_stride) {
      samples.push_back(sample_value(i));
    }
    if (samples.empty()) {
      return 0.0;
    }

    auto middle = samples.begin() + static_cast<ptrdiff_t>(samples.size() / 2);
    std::nth_element(samples.begin(), middle, samples.end());
    return *middle;
  };

  bool cfa_ok = cfa_guide_image != nullptr &&
      cfa_guide_image->width == width &&
      cfa_guide_image->height == height &&
      cfa_guide_image->colors == 4;

  if (cfa_ok) {
    std::vector<double> green_split(pixel_count);
    std::vector<double> green_mean(pixel_count);
    #pragma omp parallel for schedule(static) if(pixel_count > 100000)
    for (size_t i = 0; i < pixel_count; ++i) {
      const size_t px = i * cfa_guide_image->colors;
      const double green_a = cfa_guide_image->pixels[px + 1];
      const double green_b = cfa_guide_image->pixels[px + 3];
      green_split[i] = green_a;
      green_mean[i] = green_b;
    }

    std::vector<double> smooth_split(pixel_count);
    std::vector<double> smooth_green(pixel_count);
    std::vector<double> blur_scratch_a(pixel_count);
    std::vector<double> blur_scratch_b(pixel_count);

    // Smooth the two green-phase estimates before differencing so the
    // confidence mask tracks regional disagreement, not interpolation-phase
    // stipple from the CFA guide construction.
    blur3_pair(green_split,
               green_mean,
               smooth_split,
               smooth_green,
               &blur_scratch_a,
               &blur_scratch_b);

    #pragma omp parallel for schedule(static) if(pixel_count > 100000)
    for (size_t i = 0; i < pixel_count; ++i) {
      const double phase_a = smooth_split[i];
      const double phase_b = smooth_green[i];
      green_split[i] = std::abs(phase_a - phase_b);
      green_mean[i] = 0.5 * (phase_a + phase_b);
    }

    blur3_pair(green_split,
               green_mean,
               smooth_split,
               smooth_green,
               &blur_scratch_a,
               &blur_scratch_b);

    const double median_residual = approximate_median_from_samples(
        kRobustStatSampleTarget,
        [&](size_t i) {
          const double split_hp = std::max(green_split[i] - smooth_split[i], 0.0);
          const double texture_scale = std::max(smooth_green[i], 256.0);
          return split_hp / texture_scale;
        });
    const double residual_scale = std::max(8.0 * median_residual, 0.0035);

    #pragma omp parallel for schedule(static) if(pixel_count > 100000)
    for (size_t i = 0; i < pixel_count; ++i) {
      const double split_hp = std::max(green_split[i] - smooth_split[i], 0.0);
      const double texture_scale = std::max(smooth_green[i], 256.0);
      const double norm = (split_hp / texture_scale) / residual_scale;
      const double atten = std::exp(-(norm * norm));
      confidence[i] = std::clamp(atten, 0.15, 1.0);
    }

    const double black_level = metadata.has_black_level ? metadata.black_level : 0.0;
    const double white_level = metadata.has_white_level ? metadata.white_level : 65535.0;
    const double signal_range = std::max(white_level - black_level, 1024.0);
    #pragma omp parallel for schedule(static) if(pixel_count > 100000)
    for (size_t i = 0; i < pixel_count; ++i) {
      const double normalized_signal = std::clamp(
          (smooth_green[i] - black_level) / signal_range,
          0.0,
          1.0);
      const double lifted_signal = std::clamp((normalized_signal - 0.02) / 0.10, 0.0, 1.0);
      signal_weight[i] = 0.35 + 0.65 * std::sqrt(lifted_signal);
    }

    // Keep the Stage 1 gate regional rather than phase-locked to CFA speckle.
    // A wider low-pass removes the faint dotted mask pattern that can
    // otherwise survive into blend_weight in smooth sky.
    constexpr int kConfidenceBlurPasses = 4;
    std::vector<double> smooth_conf;
    smooth_conf.swap(green_mean);
    smooth_conf.resize(pixel_count);
    for (int pass = 0; pass < kConfidenceBlurPasses; ++pass) {
      if ((pass & 1) == 0) {
        blur3(confidence, smooth_conf, &blur_scratch_a);
      } else {
        blur3(smooth_conf, confidence, &blur_scratch_a);
      }
    }
    if ((kConfidenceBlurPasses & 1) != 0) {
      confidence.swap(smooth_conf);
    }
  } else {
    std::fill(confidence.begin(), confidence.end(), 1.0);
  }

  #pragma omp parallel for schedule(static) if(pixel_count > 100000)
  for (size_t i = 0; i < pixel_count; ++i) {
    enhancement_weight[i] = confidence[i] * signal_weight[i];
  }

  hiraco::LogTiming("enhance", "Stage 0 prepare luma", std::chrono::steady_clock::now() - stage0_start);

  if (CheckCancelled(cancel, error_message)) {
    return false;
  }
  ReportProgress(progress, "enhance", 0.20, "Running Stage 1 detail recovery");

  // --- Stage 1: Wiener deconvolution (FFT-based) ---
  // The sensor-shift composite has a near-Gaussian PSF with no frequency-
  // domain zeros, making Wiener deconvolution well-posed.
  // Uses mirror/reflection padding to avoid boundary discontinuities that
  // produce periodic stipple artifacts with zero-padding.
  if (enable_stage1) {
    const hiraco::ScopedTimingLog timer("enhance", "Stage 1 detail recovery");
    const float psf_sigma = settings.stage1_psf_sigma;
    const float nsr = settings.stage1_nsr;

    // Mirror-pad margins — enough to cover the PSF support and avoid
    // wrap-around artefacts.  32 px border is ample for σ ≤ 2.
    constexpr uint32_t kPadMargin = 32;
    const uint32_t padded_w = width + 2 * kPadMargin;
    const uint32_t padded_h = height + 2 * kPadMargin;

    // Round up to efficient FFT size (next power of 2).
    uint32_t fft_w = 1;
    while (fft_w < padded_w) fft_w <<= 1;
    uint32_t fft_h = 1;
    while (fft_h < padded_h) fft_h <<= 1;

    const size_t fft_n = static_cast<size_t>(fft_w) * fft_h;
    const size_t complex_n = static_cast<size_t>(fft_h) * (fft_w / 2 + 1);

    double* fft_in = fftw_alloc_real(fft_n);
    fftw_complex* fft_out = fftw_alloc_complex(complex_n);

    if (fft_in && fft_out) {
      // Ensure FFTW threading is initialized and load cached wisdom.
      InitFftwThreads();
      LoadFftwWisdom();

      auto reflect_index = [](int index, int limit) {
        if (index < 0) index = -index;
        if (index >= limit) index = 2 * limit - 2 - index;
        return std::clamp(index, 0, limit - 1);
      };

      std::vector<uint32_t> reflected_rows(padded_h, 0);
      std::vector<uint32_t> reflected_cols(padded_w, 0);
      for (uint32_t pr = 0; pr < padded_h; ++pr) {
        reflected_rows[pr] = static_cast<uint32_t>(reflect_index(
            static_cast<int>(pr) - static_cast<int>(kPadMargin),
            static_cast<int>(height)));
      }
      for (uint32_t pc = 0; pc < padded_w; ++pc) {
        reflected_cols[pc] = static_cast<uint32_t>(reflect_index(
            static_cast<int>(pc) - static_cast<int>(kPadMargin),
            static_cast<int>(width)));
      }

      auto fill_fft_input = [&]() {
        std::fill(fft_in, fft_in + fft_n, 0.0);
        #pragma omp parallel for schedule(static) if(padded_h > 100)
        for (uint32_t pr = 0; pr < padded_h; ++pr) {
          const size_t dst_row = static_cast<size_t>(pr) * fft_w;
          const size_t src_row = static_cast<size_t>(reflected_rows[pr]) * width;
          for (uint32_t pc = 0; pc < padded_w; ++pc) {
            fft_in[dst_row + pc] = luma[src_row + reflected_cols[pc]];
          }
        }
      };

      fill_fft_input();

      fftw_plan plan_fwd = fftw_plan_dft_r2c_2d(
          static_cast<int>(fft_h), static_cast<int>(fft_w),
          fft_in, fft_out, FFTW_MEASURE);

      if (plan_fwd) {
        // Re-fill the buffer after FFTW_MEASURE (planning may clobber it).
        fill_fft_input();
        fftw_execute(plan_fwd);
        fftw_destroy_plan(plan_fwd);

// Apply Wiener filter in frequency domain.
          //
          // We apply the physical sensor diode integration limit models:
          // The high-res sensor shifts 8 times by half a pixel so it has a 
          // 2x oversampled integration box. In the frequency domain, a box
          // filter responds as a sinc(x) function that naturally zeroes
          // out the exact spatial frequencies where Bayer grid residuals clash,
          // safely preventing Wiener over-amplification at the Nyquist limit.
          const double two_pi2_sigma2 = 2.0 * M_PI * M_PI * psf_sigma * psf_sigma;
          const uint32_t half_w = fft_w / 2 + 1;

          #pragma omp parallel for schedule(static) if(fft_h > 100)
          for (uint32_t row = 0; row < fft_h; ++row) {
            double fy = static_cast<double>(row);
            if (fy > fft_h / 2.0) fy -= fft_h;
            fy /= fft_h;  // normalized: [-0.5, 0.5)

            for (uint32_t col = 0; col < half_w; ++col) {
              const double fx = static_cast<double>(col) / fft_w;

              // Wiener filter for PSF deconvolution + Diode integration
              auto sinc = [](double x) {
                if (std::abs(x) < 1e-6) return 1.0;
                const double px = M_PI * x;
                return std::sin(px) / px;
              };
              const double H_diode = sinc(2.0 * fx) * sinc(2.0 * fy);
              const double freq_sq = fx * fx + fy * fy;
              const double H_optics = std::exp(-two_pi2_sigma2 * freq_sq);
              const double H = H_optics * H_diode;

              const double H2 = H * H;
              const double wiener = H / (H2 + nsr);

              const size_t idx = static_cast<size_t>(row) * half_w + col;
              fft_out[idx][0] *= wiener;
              fft_out[idx][1] *= wiener;
          }
        }

        fftw_plan plan_inv = fftw_plan_dft_c2r_2d(
            static_cast<int>(fft_h), static_cast<int>(fft_w),
            fft_out, fft_in, FFTW_MEASURE);

        if (plan_inv) {
          fftw_execute(plan_inv);
          fftw_destroy_plan(plan_inv);

          // Save FFTW wisdom so subsequent runs skip the planning cost.
          SaveFftwWisdom();

          // Extract the central (un-padded) region, normalised.
          const double norm = 1.0 / static_cast<double>(fft_n);
          #pragma omp parallel for schedule(static) if(height > 100)
          for (uint32_t row = 0; row < height; ++row) {
            const size_t src_offset =
                static_cast<size_t>(row + kPadMargin) * fft_w + kPadMargin;
            const size_t dst_offset = static_cast<size_t>(row) * width;
            for (uint32_t col = 0; col < width; ++col) {
              luma[dst_offset + col] = fft_in[src_offset + col] * norm;
            }
          }

          if (cfa_ok) {
            std::vector<double> residual(pixel_count, 0.0);
            #pragma omp parallel for schedule(static) if(pixel_count > 100000)
            for (size_t i = 0; i < pixel_count; ++i) {
              residual[i] = luma[i] - base_luma[i];
            }

            std::vector<double> smooth_residual(pixel_count, 0.0);
            std::vector<double> residual_scratch(pixel_count, 0.0);
            blur3(residual, smooth_residual, &residual_scratch);

            const double residual_smoothing = std::clamp(3.0 * stage1_nsr, 0.0, 0.6);
            #pragma omp parallel for schedule(static) if(pixel_count > 100000)
            for (size_t i = 0; i < pixel_count; ++i) {
              const double blend_confidence = enhancement_weight[i];
              const double suspicious = 1.0 - blend_confidence;
              const double mix = std::clamp(residual_smoothing * suspicious, 0.0, 1.0);
              const double high_frequency_residual = residual[i] - smooth_residual[i];
              const double filtered_residual =
                  (1.0 - mix) * residual[i] + mix * smooth_residual[i];
              luma[i] = base_luma[i] + blend_confidence * filtered_residual;
            }
          } else {
            #pragma omp parallel for schedule(static) if(pixel_count > 100000)
            for (size_t i = 0; i < pixel_count; ++i) {
              luma[i] = base_luma[i] + confidence[i] * (luma[i] - base_luma[i]);
            }
          }
        }
      }
    }

    if (fft_out) fftw_free(fft_out);
    if (fft_in) fftw_free(fft_in);
  }

  if (CheckCancelled(cancel, error_message)) {
    return false;
  }
  ReportProgress(progress, "enhance", 0.55, "Running Stage 2 wavelet refinement");

  // --- Stage 2: Multi-scale à trous wavelet detail enhancement ---
  // Operates at full resolution at every scale using dilated B3-spline
  // convolution.  Each scale captures a different frequency band and
  // receives an independent gain.
  if (enable_stage2) {
    const hiraco::ScopedTimingLog timer("enhance", "Stage 2 wavelet refinement");
    constexpr int kNumScales = 4;
    const float denoise = settings.stage2_denoise;
    const float gain0 = DeriveFineScaleGainFromSmall(settings.stage2_gain1);
    const float gain1 = settings.stage2_gain1;
    const float gain2 = settings.stage2_gain2;
    const float gain3 = settings.stage2_gain3;
    double user_scale_gains[kNumScales] = {gain0, gain1, gain2, gain3};
    double top_down_gains[kNumScales] = {};
    double bottom_up_gains[kNumScales] = {};
    double effective_scale_gains[kNumScales] = {};

    // Blend gains in both directions so neighboring wavelet bands overlap
    // more like practical detail controls instead of isolated narrow bins.
    top_down_gains[0] = user_scale_gains[0];
    for (int scale = 1; scale < kNumScales; ++scale) {
      top_down_gains[scale] =
          0.68 * user_scale_gains[scale] + 0.32 * top_down_gains[scale - 1];
    }
    bottom_up_gains[kNumScales - 1] = user_scale_gains[kNumScales - 1];
    for (int scale = kNumScales - 2; scale >= 0; --scale) {
      bottom_up_gains[scale] =
          0.68 * user_scale_gains[scale] + 0.32 * bottom_up_gains[scale + 1];
    }
    for (int scale = 0; scale < kNumScales; ++scale) {
      effective_scale_gains[scale] = std::clamp(
          0.50 * user_scale_gains[scale] +
              0.25 * top_down_gains[scale] +
              0.25 * bottom_up_gains[scale],
          0.25,
          4.0);
    }

    std::vector<double> approx_prev(luma);
    std::vector<double> detail_accum;
    detail_accum.swap(base_luma);
    std::fill(detail_accum.begin(), detail_accum.end(), 0.0);
    std::vector<double> approx_cur(pixel_count);

    for (int scale = 0; scale < kNumScales; ++scale) {
      const int step = 1 << scale;  // dilation: 1, 2, 4, 8

      // Halide AOT: separable B3-spline convolution with dilation.
      Halide::Runtime::Buffer<double> in_buf(approx_prev.data(), width, height);
      Halide::Runtime::Buffer<double> out_buf(approx_cur.data(), width, height);
      hiraco_atrous_wavelet(in_buf, step, out_buf);

      // Detail = previous_approx - current_approx.
      // Apply capped adaptive thresholding so the finest bands stay
      // responsive instead of being wiped out when BayesShrink spikes.
      const double extra_gain = effective_scale_gains[scale] - 1.0;
      if (std::abs(extra_gain) > 1e-6) {
        // Estimate noise σ at this scale using robust MAD estimator.
        // σ_noise ≈ median(|detail|) / 0.6745
        const double median_abs = approximate_median_from_samples(
            kRobustStatSampleTarget,
            [&](size_t i) {
              return std::abs(approx_prev[i] - approx_cur[i]);
            });
        const double sigma_noise = median_abs / 0.6745;
        // Soft threshold = σ²_noise / σ_signal
        // with σ_signal estimated from the detail coefficients.
        double sum_sq = 0.0;
        #pragma omp parallel for reduction(+:sum_sq) schedule(static) if(pixel_count > 100000)
        for (size_t i = 0; i < pixel_count; ++i) {
          const double d = approx_prev[i] - approx_cur[i];
          sum_sq += d * d;
        }
        const double sigma_sq_total =
            sum_sq / static_cast<double>(pixel_count);
        const double sigma_sq_noise = sigma_noise * sigma_noise;
        const double sigma_sq_signal =
            std::max(sigma_sq_total - sigma_sq_noise, 1e-10);
        const double threshold_floor =
          sigma_noise * (0.75 + 0.20 * static_cast<double>(scale));
        const double threshold_ceiling = threshold_floor * 2.5;
        double threshold = 0.0;
        if (sigma_noise > 1e-12) {
          const double bayes_threshold =
            sigma_sq_noise / std::max(std::sqrt(sigma_sq_signal), 0.35 * sigma_noise);
          threshold = std::clamp(bayes_threshold, threshold_floor, threshold_ceiling);
        }
        threshold *= denoise;
        threshold /= 1.0 + 0.35 * std::max(extra_gain, 0.0);

        #pragma omp parallel for schedule(static) if(pixel_count > 100000)
        for (size_t i = 0; i < pixel_count; ++i) {
          const double raw_detail = approx_prev[i] - approx_cur[i];
          double detail = raw_detail;
          if (extra_gain >= 0.0) {
            // Soft-threshold: shrink toward zero when boosting detail.
            if (detail > threshold)
              detail -= threshold;
            else if (detail < -threshold)
              detail += threshold;
            else
              detail = 0.0;
          }

          const double signal_floor = 0.15 + 0.05 * static_cast<double>(kNumScales - 1 - scale);
          const double scale_weight =
              confidence[i] * (signal_floor + (1.0 - signal_floor) * signal_weight[i]);
          const double activity =
              std::abs(raw_detail) / (std::abs(raw_detail) + threshold + 1e-6);
          const double band_weight = scale_weight * (0.55 + 0.45 * activity);
          detail_accum[i] += extra_gain * band_weight * detail;
        }
      }

      approx_prev.swap(approx_cur);
    }

    // Add accumulated wavelet detail to the (Wiener-deconvolved) luma.
    #pragma omp parallel for schedule(static) if(pixel_count > 100000)
    for (size_t i = 0; i < pixel_count; ++i) {
      luma[i] += detail_accum[i];
    }
  }

  if (CheckCancelled(cancel, error_message)) {
    return false;
  }
  ReportProgress(progress, "enhance", 0.80, "Running Stage 3 guided refinement");

  // --- Stage 3: Guided-filter based edge-aware refinement ---
  // Applies a final sharpening pass using a guided filter (self-guided)
  // as the smoothing base, avoiding halos at strong edges.
  if (enable_stage3) {
    const hiraco::ScopedTimingLog timer("enhance", "Stage 3 guided refinement");
    const int gf_radius = settings.stage3_radius;
    const float user_gf_gain = settings.stage3_gain;

    constexpr double kGfEps = 0.001 * 65535.0 * 65535.0;
    const double gf_gain = user_gf_gain * (base_gain - 1.0);

    // Halide AOT: guided filter with adaptive gain.
    Halide::Runtime::Buffer<double> luma_buf(luma.data(), width, height);
    Halide::Runtime::Buffer<double> conf_buf(enhancement_weight.data(), width, height);
    std::vector<double> gf_output(pixel_count);
    Halide::Runtime::Buffer<double> result_buf(gf_output.data(), width, height);
    hiraco_guided_filter(luma_buf, conf_buf, gf_radius, kGfEps, gf_gain,
                         result_buf);
    luma.swap(gf_output);
  }

  // --- Stage 4: Multiplicative ratio transfer to RGB ---
  // CFA residual has been suppressed in Stage 1 (frequency-domain notch),
  // so the per-pixel ratio is clean.
  constexpr double kMinLuma = 1.0;
  constexpr double kMinRatio = 0.3;
  constexpr double kMaxRatio = 4.0;

  const auto stage4_start = std::chrono::steady_clock::now();

  #pragma omp parallel for schedule(static) if(pixel_count > 100000)
  for (size_t i = 0; i < pixel_count; ++i) {
    const size_t px = i * colors;
    const double orig_y = std::max(
        kLumaR * image->pixels[px] +
        kLumaG * image->pixels[px + 1] +
        kLumaB * image->pixels[px + 2],
        kMinLuma);
    const double enhanced_y = std::max(luma[i], 0.0);
    double ratio = enhanced_y / orig_y;
    ratio = std::clamp(ratio, kMinRatio, kMaxRatio);

    for (uint32_t ch = 0; ch < colors; ++ch) {
      const double val = static_cast<double>(image->pixels[px + ch]) * ratio;
      image->pixels[px + ch] = static_cast<uint16_t>(std::clamp(val, 0.0, 65535.0));
    }
  }

  hiraco::LogTiming("enhance", "Stage 4 ratio transfer", std::chrono::steady_clock::now() - stage4_start);

  ReportProgress(progress, "enhance", 1.0, "Enhancement complete");
  return true;
}


uint32_t DngVersionForCompression(const std::string& compression) {
  if (compression == "jpeg-xl") {
    return dngVersion_SaveDefault;
  }
  return dngVersion_1_6_0_0;
}

void ConfigureHost(dng_host& host, const std::string& compression) {
  host.SetSaveDNGVersion(DngVersionForCompression(compression));
  host.SetSaveLinearDNG(true);
  host.SetLosslessJXL(compression == "jpeg-xl");
  host.SetLossyMosaicJXL(false);
}

bool IsSupportedWriteCompression(const std::string& compression) {
  return compression == "uncompressed" || compression == "deflate" || compression == "jpeg-xl";
}

std::string UnsupportedCompressionMessage(const std::string& compression) {
  return "Unsupported compression requested for the current DNG writer path";
}

bool ExtractCfaGuideImage(const std::string& source_path,
                          const SourceLinearDngMetadata& metadata,
                          RasterImage* guide,
                          std::string* error_message) {
  guide->width = 0;
  guide->height = 0;
  guide->colors = 0;
  guide->bits = 0;
  guide->pixels.clear();

  auto processor = std::make_unique<LibRaw>();
  int result = processor->open_file(source_path.c_str());
  if (result != LIBRAW_SUCCESS) {
    *error_message = std::string("LibRaw open_file failed: ") + libraw_strerror(result);
    return false;
  }

  result = processor->unpack();
  if (result != LIBRAW_SUCCESS) {
    *error_message = std::string("LibRaw unpack failed: ") + libraw_strerror(result);
    processor->recycle();
    return false;
  }

  const ushort* raw = processor->imgdata.rawdata.raw_image;
  const uint32_t width = processor->imgdata.sizes.raw_width;
  const uint32_t height = processor->imgdata.sizes.raw_height;
  if (raw == nullptr || width == 0 || height == 0) {
    *error_message = "LibRaw raw mosaic not available for CFA guide extraction";
    processor->recycle();
    return false;
  }

  guide->width = width;
  guide->height = height;
  guide->colors = 4;
  guide->bits = 16;
  guide->pixels.resize(static_cast<size_t>(width) * height * guide->colors, 0);

  const double black_level = RawDomainBlackLevel(*processor);

  auto raw_sample = [&](int row, int col, int target_color) -> double {
    row = std::clamp(row, 0, static_cast<int>(height) - 1);
    col = std::clamp(col, 0, static_cast<int>(width) - 1);
    if (processor->COLOR(row, col) != target_color) {
      return -1.0;
    }
    const size_t idx = static_cast<size_t>(row) * width + col;
    return std::max(static_cast<double>(raw[idx]) - black_level, 0.0);
  };

  #pragma omp parallel for schedule(static) if(height > 100)
  for (uint32_t row = 0; row < height; ++row) {
    for (uint32_t col = 0; col < width; ++col) {
      const size_t px = (static_cast<size_t>(row) * width + col) * guide->colors;

      double green_a = 0.0;
      double green_b = 0.0;

      const int color = processor->COLOR(static_cast<int>(row), static_cast<int>(col));

      if (color == 1) {
        green_a = raw_sample(static_cast<int>(row), static_cast<int>(col), 1);
        const double d1 = raw_sample(static_cast<int>(row) - 1, static_cast<int>(col) - 1, 3);
        const double d2 = raw_sample(static_cast<int>(row) - 1, static_cast<int>(col) + 1, 3);
        const double d3 = raw_sample(static_cast<int>(row) + 1, static_cast<int>(col) - 1, 3);
        const double d4 = raw_sample(static_cast<int>(row) + 1, static_cast<int>(col) + 1, 3);
        green_b = 0.25 * (std::max(d1, 0.0) + std::max(d2, 0.0) + std::max(d3, 0.0) + std::max(d4, 0.0));
      } else if (color == 3) {
        green_b = raw_sample(static_cast<int>(row), static_cast<int>(col), 3);
        const double d1 = raw_sample(static_cast<int>(row) - 1, static_cast<int>(col) - 1, 1);
        const double d2 = raw_sample(static_cast<int>(row) - 1, static_cast<int>(col) + 1, 1);
        const double d3 = raw_sample(static_cast<int>(row) + 1, static_cast<int>(col) - 1, 1);
        const double d4 = raw_sample(static_cast<int>(row) + 1, static_cast<int>(col) + 1, 1);
        green_a = 0.25 * (std::max(d1, 0.0) + std::max(d2, 0.0) + std::max(d3, 0.0) + std::max(d4, 0.0));
      } else if (color == 0) {
        const double g1l = raw_sample(static_cast<int>(row), static_cast<int>(col) - 1, 1);
        const double g1r = raw_sample(static_cast<int>(row), static_cast<int>(col) + 1, 1);
        const double g2u = raw_sample(static_cast<int>(row) - 1, static_cast<int>(col), 3);
        const double g2d = raw_sample(static_cast<int>(row) + 1, static_cast<int>(col), 3);
        green_a = 0.5 * (std::max(g1l, 0.0) + std::max(g1r, 0.0));
        green_b = 0.5 * (std::max(g2u, 0.0) + std::max(g2d, 0.0));
      } else {
        const double g1u = raw_sample(static_cast<int>(row) - 1, static_cast<int>(col), 1);
        const double g1d = raw_sample(static_cast<int>(row) + 1, static_cast<int>(col), 1);
        const double g2l = raw_sample(static_cast<int>(row), static_cast<int>(col) - 1, 3);
        const double g2r = raw_sample(static_cast<int>(row), static_cast<int>(col) + 1, 3);
        green_a = 0.5 * (std::max(g1u, 0.0) + std::max(g1d, 0.0));
        green_b = 0.5 * (std::max(g2l, 0.0) + std::max(g2r, 0.0));
      }

      guide->pixels[px + 1] = static_cast<uint16_t>(std::clamp(green_a, 0.0, 65535.0));
      guide->pixels[px + 3] = static_cast<uint16_t>(std::clamp(green_b, 0.0, 65535.0));
    }
  }

  processor->recycle();
  return true;
}

void ApplyOm3HighResCfaPrecondition(const SourceLinearDngMetadata& metadata,
                                    LibRaw* processor) {
  if (!IsOm3HighResMetadata(metadata)) {
    return;
  }

  ushort* raw = processor->imgdata.rawdata.raw_image;
  const uint32_t width = processor->imgdata.sizes.raw_width;
  const uint32_t height = processor->imgdata.sizes.raw_height;
  if (raw == nullptr || width == 0 || height == 0) {
    return;
  }

  const double black_level = RawDomainBlackLevel(*processor);
  std::vector<uint16_t> updated(raw, raw + static_cast<size_t>(width) * height);

  auto raw_sample = [&](int row, int col, int target_color) -> double {
    row = std::clamp(row, 0, static_cast<int>(height) - 1);
    col = std::clamp(col, 0, static_cast<int>(width) - 1);
    if (processor->COLOR(row, col) != target_color) {
      return -1.0;
    }
    const size_t idx = static_cast<size_t>(row) * width + col;
    return std::max(static_cast<double>(raw[idx]) - black_level, 0.0);
  };

  #pragma omp parallel for schedule(static) if(height > 100)
  for (uint32_t row = 0; row < height; ++row) {
    for (uint32_t col = 0; col < width; ++col) {
      const int color = processor->COLOR(static_cast<int>(row), static_cast<int>(col));
      if (color != 1 && color != 3) {
        continue;
      }

      const size_t idx = static_cast<size_t>(row) * width + col;
      const double current = std::max(static_cast<double>(raw[idx]) - black_level, 0.0);

      double opposite_estimate = -1.0;
      if (color == 1) {
        const double d1 = raw_sample(static_cast<int>(row) - 1, static_cast<int>(col) - 1, 3);
        const double d2 = raw_sample(static_cast<int>(row) - 1, static_cast<int>(col) + 1, 3);
        const double d3 = raw_sample(static_cast<int>(row) + 1, static_cast<int>(col) - 1, 3);
        const double d4 = raw_sample(static_cast<int>(row) + 1, static_cast<int>(col) + 1, 3);
        opposite_estimate = 0.25 * (std::max(d1, 0.0) + std::max(d2, 0.0) +
                                    std::max(d3, 0.0) + std::max(d4, 0.0));
      } else {
        const double d1 = raw_sample(static_cast<int>(row) - 1, static_cast<int>(col) - 1, 1);
        const double d2 = raw_sample(static_cast<int>(row) - 1, static_cast<int>(col) + 1, 1);
        const double d3 = raw_sample(static_cast<int>(row) + 1, static_cast<int>(col) - 1, 1);
        const double d4 = raw_sample(static_cast<int>(row) + 1, static_cast<int>(col) + 1, 1);
        opposite_estimate = 0.25 * (std::max(d1, 0.0) + std::max(d2, 0.0) +
                                    std::max(d3, 0.0) + std::max(d4, 0.0));
      }

      if (opposite_estimate < 0.0) {
        continue;
      }

      const double local_scale = std::max({current, opposite_estimate, 256.0});
      const double split = current - opposite_estimate;
      const double threshold = 0.012 * local_scale + 24.0;
      if (std::abs(split) <= threshold) {
        continue;
      }

      const double norm = std::abs(split) / threshold;
      const double alpha = std::clamp(1.0 / (1.0 + 0.35 * norm * norm), 0.35, 1.0);
      const double fused = opposite_estimate + alpha * split;
      updated[idx] = static_cast<uint16_t>(std::clamp(fused + black_level, 0.0, 65535.0));
    }
  }

  std::copy(updated.begin(), updated.end(), raw);
}

RasterImage MakeSyntheticRgbImage() {
  RasterImage image;
  image.width = 48;
  image.height = 32;
  image.colors = 3;
  image.bits = 16;
  image.pixels.resize(static_cast<size_t>(image.width) * image.height * image.colors);

  for (uint32_t row = 0; row < image.height; ++row) {
    for (uint32_t col = 0; col < image.width; ++col) {
      const size_t index = (static_cast<size_t>(row) * image.width + col) * image.colors;
      image.pixels[index + 0] = static_cast<uint16_t>((col * 65535u) / (image.width - 1));
      image.pixels[index + 1] = static_cast<uint16_t>((row * 65535u) / (image.height - 1));
      image.pixels[index + 2] = static_cast<uint16_t>(((row + col) * 65535u) / (image.width + image.height - 2));
    }
  }

  return image;
}

bool RenderLibRawImage(const std::string& source_path,
                       const SourceLinearDngMetadata& metadata,
                       const RenderSettings& settings,
                       RasterImage* output,
                       std::string* error_message,
                       int expected_colors = 3) {
  const hiraco::ScopedTimingLog timer("libraw", "Render image");
  auto processor = std::make_unique<LibRaw>();

  int result = LIBRAW_SUCCESS;
  {
    const hiraco::ScopedTimingLog step_timer("libraw", "open_file");
    result = processor->open_file(source_path.c_str());
    if (result != LIBRAW_SUCCESS) {
      *error_message = std::string("LibRaw open_file failed: ") + libraw_strerror(result);
      return false;
    }
  }

  {
    const hiraco::ScopedTimingLog step_timer("libraw", "unpack");
    result = processor->unpack();
    if (result != LIBRAW_SUCCESS) {
      *error_message = std::string("LibRaw unpack failed: ") + libraw_strerror(result);
      processor->recycle();
      return false;
    }
  }

  ApplyOm3HighResCfaPrecondition(metadata, processor.get());

  libraw_set_output_bps(&processor->imgdata, 16);
  libraw_set_output_color(&processor->imgdata, settings.output_color);
  libraw_set_no_auto_bright(&processor->imgdata, settings.no_auto_bright);
  libraw_set_gamma(&processor->imgdata, 0, settings.gamma_power);
  libraw_set_gamma(&processor->imgdata, 1, settings.gamma_slope);
  processor->imgdata.params.use_camera_wb = settings.use_camera_wb;
  processor->imgdata.params.use_camera_matrix = settings.use_camera_matrix;
  processor->imgdata.params.no_auto_scale = settings.no_auto_scale;
  processor->imgdata.params.user_flip = settings.user_flip;
  if (settings.user_qual >= 0) {
    processor->imgdata.params.user_qual = settings.user_qual;
  }
  processor->imgdata.params.half_size = settings.half_size;
  processor->imgdata.params.four_color_rgb = settings.four_color_rgb;
  processor->imgdata.params.green_matching = settings.green_matching;
  processor->imgdata.params.med_passes = settings.med_passes;
  processor->imgdata.params.adjust_maximum_thr = settings.adjust_maximum_thr;

  {
    const hiraco::ScopedTimingLog step_timer("libraw", "dcraw_process");
    result = processor->dcraw_process();
    if (result != LIBRAW_SUCCESS) {
      *error_message = std::string("LibRaw dcraw_process failed: ") + libraw_strerror(result);
      processor->recycle();
      return false;
    }
  }

  int mem_error = LIBRAW_SUCCESS;
  libraw_processed_image_t* processed = nullptr;
  {
    const hiraco::ScopedTimingLog step_timer("libraw", "dcraw_make_mem_image");
    processed = processor->dcraw_make_mem_image(&mem_error);
  }
  if (processed == nullptr || mem_error != LIBRAW_SUCCESS) {
    *error_message = std::string("LibRaw dcraw_make_mem_image failed: ") + libraw_strerror(mem_error);
    LibRaw::dcraw_clear_mem(processed);
    processor->recycle();
    return false;
  }

  if (processed->bits != 16 ||
      (expected_colors > 0 && processed->colors != expected_colors)) {
    *error_message = "LibRaw produced an unexpected processed image format";
    LibRaw::dcraw_clear_mem(processed);
    processor->recycle();
    return false;
  }

  output->width = processed->width;
  output->height = processed->height;
  output->colors = processed->colors;
  output->bits = processed->bits;

  const size_t sample_count = static_cast<size_t>(processed->width) *
                              static_cast<size_t>(processed->height) *
                              static_cast<size_t>(processed->colors);
  output->pixels.resize(sample_count);

  {
    const hiraco::ScopedTimingLog step_timer("libraw", "copy processed pixels");
    const auto* source_pixels = reinterpret_cast<const uint16_t*>(processed->data);
    std::copy(source_pixels, source_pixels + sample_count, output->pixels.begin());
  }

  LibRaw::dcraw_clear_mem(processed);
  processor->recycle();
  return true;
}

std::array<double, 9> IdentityMatrix3x3() {
  return {1.0, 0.0, 0.0,
          0.0, 1.0, 0.0,
          0.0, 0.0, 1.0};
}

bool InvertMatrix3x3(const std::array<double, 9>& input,
                     std::array<double, 9>* inverse) {
  const double a = input[0];
  const double b = input[1];
  const double c = input[2];
  const double d = input[3];
  const double e = input[4];
  const double f = input[5];
  const double g = input[6];
  const double h = input[7];
  const double i = input[8];

  const double A = e * i - f * h;
  const double B = -(d * i - f * g);
  const double C = d * h - e * g;
  const double D = -(b * i - c * h);
  const double E = a * i - c * g;
  const double F = -(a * h - b * g);
  const double G = b * f - c * e;
  const double H = -(a * f - c * d);
  const double I = a * e - b * d;

  const double det = a * A + b * B + c * C;
  if (std::abs(det) < 1e-9) {
    return false;
  }

  const double inv_det = 1.0 / det;
  *inverse = {A * inv_det, D * inv_det, G * inv_det,
              B * inv_det, E * inv_det, H * inv_det,
              C * inv_det, F * inv_det, I * inv_det};
  return true;
}

std::array<double, 3> MultiplyMatrix3x3Vector(const std::array<double, 9>& matrix,
                                              const std::array<double, 3>& vector) {
  return {
      matrix[0] * vector[0] + matrix[1] * vector[1] + matrix[2] * vector[2],
      matrix[3] * vector[0] + matrix[4] * vector[1] + matrix[5] * vector[2],
      matrix[6] * vector[0] + matrix[7] * vector[1] + matrix[8] * vector[2],
  };
}

bool RenderOm3RawDomainImage(const std::string& source_path,
                             const SourceLinearDngMetadata& metadata,
                             const CropRect* region,
                             RasterImage* output,
                             std::string* error_message) {
  const hiraco::ScopedTimingLog om3_timer("raw", "Render raw domain image");
  using WorkingValue = double;
  if (output == nullptr) {
    *error_message = "Null output raster for raw-domain render";
    return false;
  }

  const auto mosaic_start = std::chrono::steady_clock::now();
  auto processor = std::make_unique<LibRaw>();
  int result = processor->open_file(source_path.c_str());
  if (result != LIBRAW_SUCCESS) {
    *error_message = std::string("LibRaw open_file failed: ") + libraw_strerror(result);
    return false;
  }

  result = processor->unpack();
  if (result != LIBRAW_SUCCESS) {
    *error_message = std::string("LibRaw unpack failed: ") + libraw_strerror(result);
    processor->recycle();
    return false;
  }

  ApplyOm3HighResCfaPrecondition(metadata, processor.get());

  const ushort* raw = processor->imgdata.rawdata.raw_image;
  const uint32_t source_width = processor->imgdata.sizes.raw_width;
  const uint32_t source_height = processor->imgdata.sizes.raw_height;
  if (raw == nullptr || source_width == 0 || source_height == 0) {
    *error_message = "LibRaw raw mosaic not available for raw-domain render";
    processor->recycle();
    return false;
  }

  CropRect render_region;
  render_region.x = 0;
  render_region.y = 0;
  render_region.width = source_width;
  render_region.height = source_height;
  if (region != nullptr) {
    render_region.x = std::min(region->x, source_width - 1);
    render_region.y = std::min(region->y, source_height - 1);
    render_region.width = std::max(1u, std::min(region->width, source_width - render_region.x));
    render_region.height = std::max(1u, std::min(region->height, source_height - render_region.y));
  }

  const uint32_t width = render_region.width;
  const uint32_t height = render_region.height;

  const size_t pixel_count = static_cast<size_t>(width) * height;
  const double raw_black_level = RawDomainBlackLevel(*processor);
  const double raw_white_level = std::max(RawDomainWhiteLevel(*processor), raw_black_level + 1.0);
  const double pedestal = metadata.has_black_level ? metadata.black_level : 0.0;
  const double max_signal = 65535.0 - pedestal;
  const double scale = max_signal / std::max(raw_white_level - raw_black_level, 1.0);

  std::vector<WorkingValue> mosaic(pixel_count);
  std::vector<uint8_t> cfa(pixel_count);
  std::vector<size_t> source_row_offsets(height, 0);
  for (uint32_t row = 0; row < height; ++row) {
    source_row_offsets[row] =
        static_cast<size_t>(render_region.y + row) * source_width + render_region.x;
  }
  #pragma omp parallel for schedule(static) if(height > 100)
  for (uint32_t row = 0; row < height; ++row) {
    const uint32_t source_row = render_region.y + row;
    const size_t source_row_offset = source_row_offsets[row];
    const size_t row_offset = static_cast<size_t>(row) * width;
    for (uint32_t col = 0; col < width; ++col) {
      const size_t idx = row_offset + col;
      const uint32_t source_col = render_region.x + col;
      const size_t source_idx = source_row_offset + col;
      cfa[idx] = static_cast<uint8_t>(processor->COLOR(static_cast<int>(source_row), static_cast<int>(source_col)));
      mosaic[idx] = static_cast<WorkingValue>(std::clamp(
          (std::max(static_cast<double>(raw[source_idx]) - raw_black_level, 0.0)) * scale,
          0.0,
          max_signal));
    }
  }

  hiraco::LogTiming("raw", "Load mosaic and CFA", std::chrono::steady_clock::now() - mosaic_start);

  processor->recycle();

  output->width = width;
  output->height = height;
  output->colors = 3;
  output->bits = 16;
  output->pixels.resize(pixel_count * output->colors);

  auto clamp_channel = [&](double value) -> uint16_t {
    return static_cast<uint16_t>(std::clamp(pedestal + value, 0.0, 65535.0));
  };

  auto clamp_row = [&](int row) -> uint32_t {
    return static_cast<uint32_t>(std::clamp(row, 0, static_cast<int>(height) - 1));
  };
  auto clamp_col = [&](int col) -> uint32_t {
    return static_cast<uint32_t>(std::clamp(col, 0, static_cast<int>(width) - 1));
  };
  std::vector<uint32_t> row_prev2(height, 0);
  std::vector<uint32_t> row_prev1(height, 0);
  std::vector<uint32_t> row_next1(height, 0);
  std::vector<uint32_t> row_next2(height, 0);
  for (uint32_t row = 0; row < height; ++row) {
    row_prev2[row] = clamp_row(static_cast<int>(row) - 2);
    row_prev1[row] = clamp_row(static_cast<int>(row) - 1);
    row_next1[row] = clamp_row(static_cast<int>(row) + 1);
    row_next2[row] = clamp_row(static_cast<int>(row) + 2);
  }
  std::vector<uint32_t> col_prev2(width, 0);
  std::vector<uint32_t> col_prev1(width, 0);
  std::vector<uint32_t> col_next1(width, 0);
  std::vector<uint32_t> col_next2(width, 0);
  for (uint32_t col = 0; col < width; ++col) {
    col_prev2[col] = clamp_col(static_cast<int>(col) - 2);
    col_prev1[col] = clamp_col(static_cast<int>(col) - 1);
    col_next1[col] = clamp_col(static_cast<int>(col) + 1);
    col_next2[col] = clamp_col(static_cast<int>(col) + 2);
  }
  auto sample_green_site = [&](size_t idx) -> double {
    return (cfa[idx] == 1 || cfa[idx] == 3) ? static_cast<double>(mosaic[idx]) : 0.0;
  };

  const auto map_start = std::chrono::steady_clock::now();
  std::vector<WorkingValue> stack_stability;
  if (!LoadStackStabilityMap(metadata, &stack_stability, error_message)) {
    return false;
  }

  std::vector<WorkingValue> stack_guide;
  if (!LoadStackGuideMap(metadata, &stack_guide, error_message)) {
    return false;
  }

  std::vector<WorkingValue> upsampled_stack_guide;

  std::vector<WorkingValue> stack_alias;
  if (!LoadStackAliasMap(metadata, &stack_alias, error_message)) {
    return false;
  }

  std::vector<WorkingValue> stack_tensor_x;
  if (!LoadStackTensorXMap(metadata, &stack_tensor_x, error_message)) {
    return false;
  }

  std::vector<WorkingValue> stack_tensor_y;
  if (!LoadStackTensorYMap(metadata, &stack_tensor_y, error_message)) {
    return false;
  }

  std::vector<WorkingValue> stack_tensor_coherence;
  if (!LoadStackTensorCoherenceMap(metadata, &stack_tensor_coherence, error_message)) {
    return false;
  }

  const RegionMapSampler stability_sampler = MakeRegionMapSampler(stack_stability,
                                                                  metadata.stack_stability_width,
                                                                  metadata.stack_stability_height,
                                                                  source_width,
                                                                  source_height,
                                                                  render_region);
  const RegionMapSampler alias_sampler = MakeRegionMapSampler(stack_alias,
                                                              metadata.stack_alias_width,
                                                              metadata.stack_alias_height,
                                                              source_width,
                                                              source_height,
                                                              render_region);
  const RegionMapSampler tensor_x_sampler = MakeRegionMapSampler(stack_tensor_x,
                                                                 metadata.stack_tensor_x_width,
                                                                 metadata.stack_tensor_x_height,
                                                                 source_width,
                                                                 source_height,
                                                                 render_region);
  const RegionMapSampler tensor_y_sampler = MakeRegionMapSampler(stack_tensor_y,
                                                                 metadata.stack_tensor_y_width,
                                                                 metadata.stack_tensor_y_height,
                                                                 source_width,
                                                                 source_height,
                                                                 render_region);
  const RegionMapSampler tensor_coherence_sampler = MakeRegionMapSampler(stack_tensor_coherence,
                                                                         metadata.stack_tensor_coherence_width,
                                                                         metadata.stack_tensor_coherence_height,
                                                                         source_width,
                                                                         source_height,
                                                                         render_region);

  hiraco::LogTiming("raw", "Load and upsample guide maps", std::chrono::steady_clock::now() - map_start);

  const auto green_start = std::chrono::steady_clock::now();
  std::vector<WorkingValue> green(pixel_count, 0.0f);
  #pragma omp parallel for schedule(static) if(height > 100)
  for (uint32_t row = 0; row < height; ++row) {
    const uint32_t row_up1 = row_prev1[row];
    const uint32_t row_down1 = row_next1[row];
    const uint32_t row_up2 = row_prev2[row];
    const uint32_t row_down2 = row_next2[row];
    const size_t row_offset = static_cast<size_t>(row) * width;
    const size_t row_up1_offset = static_cast<size_t>(row_up1) * width;
    const size_t row_down1_offset = static_cast<size_t>(row_down1) * width;
    const size_t row_up2_offset = static_cast<size_t>(row_up2) * width;
    const size_t row_down2_offset = static_cast<size_t>(row_down2) * width;
    for (uint32_t col = 0; col < width; ++col) {
      const uint32_t col_left1 = col_prev1[col];
      const uint32_t col_right1 = col_next1[col];
      const uint32_t col_left2 = col_prev2[col];
      const uint32_t col_right2 = col_next2[col];
      const size_t idx = row_offset + col;
      const size_t idx_left = row_offset + col_left1;
      const size_t idx_right = row_offset + col_right1;
      const size_t idx_up = row_up1_offset + col;
      const size_t idx_down = row_down1_offset + col;
      const size_t idx_same_left = row_offset + col_left2;
      const size_t idx_same_right = row_offset + col_right2;
      const size_t idx_same_up = row_up2_offset + col;
      const size_t idx_same_down = row_down2_offset + col;
      const int color = cfa[idx];
      if (color == 1 || color == 3) {
        green[idx] = mosaic[idx];
        continue;
      }

      const double center = mosaic[idx];
      const double g_left = sample_green_site(idx_left);
      const double g_right = sample_green_site(idx_right);
      const double g_up = sample_green_site(idx_up);
      const double g_down = sample_green_site(idx_down);
      const double same_left = mosaic[idx_same_left];
      const double same_right = mosaic[idx_same_right];
      const double same_up = mosaic[idx_same_up];
      const double same_down = mosaic[idx_same_down];

      const double horiz = 0.5 * (g_left + g_right) +
                           0.25 * (2.0 * center - same_left - same_right);
      const double vert = 0.5 * (g_up + g_down) +
                          0.25 * (2.0 * center - same_up - same_down);
      const double grad_h = std::abs(g_left - g_right) +
                            0.5 * std::abs(2.0 * center - same_left - same_right);
      const double grad_v = std::abs(g_up - g_down) +
                            0.5 * std::abs(2.0 * center - same_up - same_down);
      const double weight_h = 1.0 / (grad_h + 1.0);
      const double weight_v = 1.0 / (grad_v + 1.0);

      if (std::max(weight_h, weight_v) > 1.8 * std::min(weight_h, weight_v)) {
        green[idx] = static_cast<WorkingValue>(weight_h > weight_v ? horiz : vert);
      } else {
        green[idx] = static_cast<WorkingValue>((weight_h * horiz + weight_v * vert) / (weight_h + weight_v));
      }
      green[idx] = static_cast<WorkingValue>(std::clamp(static_cast<double>(green[idx]), 0.0, max_signal));
    }
  }

  hiraco::LogTiming("raw", "Reconstruct green base plane", std::chrono::steady_clock::now() - green_start);

  auto sample_buffer = [&](const std::vector<WorkingValue>& buffer, int row, int col) -> double {
    return static_cast<double>(buffer[static_cast<size_t>(clamp_row(row)) * width + clamp_col(col)]);
  };

  auto bilinear_sample = [&](const std::vector<WorkingValue>& buffer, double row, double col) -> double {
    const int y0 = static_cast<int>(std::floor(row));
    const int x0 = static_cast<int>(std::floor(col));
    const int y1 = y0 + 1;
    const int x1 = x0 + 1;
    const double wy = row - std::floor(row);
    const double wx = col - std::floor(col);
    const double v00 = sample_buffer(buffer, y0, x0);
    const double v01 = sample_buffer(buffer, y0, x1);
    const double v10 = sample_buffer(buffer, y1, x0);
    const double v11 = sample_buffer(buffer, y1, x1);
    const double top = (1.0 - wx) * v00 + wx * v01;
    const double bottom = (1.0 - wx) * v10 + wx * v11;
    return (1.0 - wy) * top + wy * bottom;
  };

  const auto guide_refine_start = std::chrono::steady_clock::now();
  if (!stack_guide.empty()) {
    const uint32_t guide_width = metadata.has_stack_guide_map ? metadata.stack_guide_width : metadata.stack_mean_width;
    const uint32_t guide_height = metadata.has_stack_guide_map ? metadata.stack_guide_height : metadata.stack_mean_height;

    const uint32_t working_width = metadata.has_working_geometry ? metadata.working_width : source_width;
    const uint32_t working_height = metadata.has_working_geometry ? metadata.working_height : source_height;
    const double working_offset_x = (static_cast<double>(working_width) - static_cast<double>(source_width)) / 2.0;
    const double working_offset_y = (static_cast<double>(working_height) - static_cast<double>(source_height)) / 2.0;
    std::vector<WorkingValue> low_res_green(static_cast<size_t>(guide_width) * guide_height, 0.0f);

    double sum_guide = 0.0;
    double sum_green = 0.0;
    double sum_guide_sq = 0.0;
    double sum_guide_green = 0.0;
    size_t count = 0;

    for (uint32_t row = 0; row < guide_height; ++row) {
      const double full_row =
          (static_cast<double>(row) + 0.5) * static_cast<double>(working_height) / static_cast<double>(guide_height) - 0.5 - working_offset_y;
      for (uint32_t col = 0; col < guide_width; ++col) {
        const double full_col =
            (static_cast<double>(col) + 0.5) * static_cast<double>(working_width) / static_cast<double>(guide_width) - 0.5 - working_offset_x;
        const size_t idx = static_cast<size_t>(row) * guide_width + col;
        const double sampled_green = bilinear_sample(green,
                                                     full_row - static_cast<double>(render_region.y),
                                                     full_col - static_cast<double>(render_region.x));
        low_res_green[idx] = static_cast<WorkingValue>(sampled_green);
        if (full_row >= static_cast<double>(render_region.y) - 0.5 &&
            full_row <= static_cast<double>(render_region.y + height) - 0.5 &&
            full_col >= static_cast<double>(render_region.x) - 0.5 &&
            full_col <= static_cast<double>(render_region.x + width) - 0.5) {
          const double guide_value = stack_guide[idx];
          sum_guide += guide_value;
          sum_green += sampled_green;
          sum_guide_sq += guide_value * guide_value;
          sum_guide_green += guide_value * sampled_green;
          ++count;
        }
      }
    }

    if (count == 0) {
      for (size_t idx = 0; idx < stack_guide.size(); ++idx) {
        const double guide_value = stack_guide[idx];
        const double green_value = low_res_green[idx];
        sum_guide += guide_value;
        sum_green += green_value;
        sum_guide_sq += guide_value * guide_value;
        sum_guide_green += guide_value * green_value;
      }
      count = stack_guide.size();
    }

    const double sample_count = static_cast<double>(count);
    const double mean_guide = sum_guide / sample_count;
    const double mean_green = sum_green / sample_count;
    const double var_guide = sum_guide_sq / sample_count - mean_guide * mean_guide;
    const double cov_guide_green = sum_guide_green / sample_count - mean_guide * mean_green;

    double gain = 1.0;
    if (var_guide > 1e-6) {
      gain = cov_guide_green / var_guide;
    }
    if (!std::isfinite(gain) || gain <= 0.0) {
      gain = 1.0;
    }
    gain = std::clamp(gain, 0.05, 64.0);
    const double bias = mean_green - gain * mean_guide;

    std::vector<WorkingValue> stack_guide_scaled(stack_guide.size(), 0.0f);
    for (size_t idx = 0; idx < stack_guide.size(); ++idx) {
      stack_guide_scaled[idx] = static_cast<WorkingValue>(
          std::clamp(gain * static_cast<double>(stack_guide[idx]) + bias, 0.0, 65535.0 - pedestal));
    }

    upsampled_stack_guide.resize(pixel_count, 0.0f);
    #pragma omp parallel for schedule(static) if(height > 100)
    for (uint32_t row = 0; row < height; ++row) {
      const double guide_row =
          (static_cast<double>(render_region.y + row) + working_offset_y + 0.5) *
              static_cast<double>(guide_height) / static_cast<double>(working_height) - 0.5;
      const int y0 = static_cast<int>(std::floor(guide_row));
      const int y1 = y0 + 1;
      const double wy = guide_row - std::floor(guide_row);
      const uint32_t sy0 = static_cast<uint32_t>(std::clamp(y0, 0, static_cast<int>(guide_height) - 1));
      const uint32_t sy1 = static_cast<uint32_t>(std::clamp(y1, 0, static_cast<int>(guide_height) - 1));
      for (uint32_t col = 0; col < width; ++col) {
        const size_t out_idx = static_cast<size_t>(row) * width + col;
        const double guide_col =
            (static_cast<double>(render_region.x + col) + working_offset_x + 0.5) *
                static_cast<double>(guide_width) / static_cast<double>(working_width) - 0.5;
        const int x0 = static_cast<int>(std::floor(guide_col));
        const int x1 = x0 + 1;
        const double wx = guide_col - std::floor(guide_col);
        const uint32_t sx0 = static_cast<uint32_t>(std::clamp(x0, 0, static_cast<int>(guide_width) - 1));
        const uint32_t sx1 = static_cast<uint32_t>(std::clamp(x1, 0, static_cast<int>(guide_width) - 1));

        const double base_w00 = (1.0 - wx) * (1.0 - wy);
        const double base_w01 = wx * (1.0 - wy);
        const double base_w10 = (1.0 - wx) * wy;
        const double base_w11 = wx * wy;
        const double green_center = green[out_idx];
        const double sigma = 0.05 * std::max(green_center, 512.0) + 48.0;
        const double inv_sigma_sq = 1.0 / (2.0 * sigma * sigma);

        const double low_g00 = low_res_green[static_cast<size_t>(sy0) * guide_width + sx0];
        const double low_g01 = low_res_green[static_cast<size_t>(sy0) * guide_width + sx1];
        const double low_g10 = low_res_green[static_cast<size_t>(sy1) * guide_width + sx0];
        const double low_g11 = low_res_green[static_cast<size_t>(sy1) * guide_width + sx1];

        const double w00 = base_w00 * std::exp(-(low_g00 - green_center) * (low_g00 - green_center) * inv_sigma_sq);
        const double w01 = base_w01 * std::exp(-(low_g01 - green_center) * (low_g01 - green_center) * inv_sigma_sq);
        const double w10 = base_w10 * std::exp(-(low_g10 - green_center) * (low_g10 - green_center) * inv_sigma_sq);
        const double w11 = base_w11 * std::exp(-(low_g11 - green_center) * (low_g11 - green_center) * inv_sigma_sq);
        const double weight_sum = w00 + w01 + w10 + w11;

        const double s00 = stack_guide_scaled[static_cast<size_t>(sy0) * guide_width + sx0];
        const double s01 = stack_guide_scaled[static_cast<size_t>(sy0) * guide_width + sx1];
        const double s10 = stack_guide_scaled[static_cast<size_t>(sy1) * guide_width + sx0];
        const double s11 = stack_guide_scaled[static_cast<size_t>(sy1) * guide_width + sx1];
        if (weight_sum > 1e-12) {
          upsampled_stack_guide[out_idx] = static_cast<WorkingValue>(
              (w00 * s00 + w01 * s01 + w10 * s10 + w11 * s11) / weight_sum);
        } else {
          upsampled_stack_guide[out_idx] = static_cast<WorkingValue>(
              base_w00 * s00 + base_w01 * s01 + base_w10 * s10 + base_w11 * s11);
        }
      }
    }

    std::vector<WorkingValue> refined_green(green);
    #pragma omp parallel for schedule(static) if(height > 100)
    for (uint32_t row = 0; row < height; ++row) {
      const uint32_t row_up1 = row_prev1[row];
      const uint32_t row_down1 = row_next1[row];
      const uint32_t row_up2 = row_prev2[row];
      const uint32_t row_down2 = row_next2[row];
      const size_t row_offset = static_cast<size_t>(row) * width;
      const size_t row_up1_offset = static_cast<size_t>(row_up1) * width;
      const size_t row_down1_offset = static_cast<size_t>(row_down1) * width;
      const size_t row_up2_offset = static_cast<size_t>(row_up2) * width;
      const size_t row_down2_offset = static_cast<size_t>(row_down2) * width;
      for (uint32_t col = 0; col < width; ++col) {
        const uint32_t col_left1 = col_prev1[col];
        const uint32_t col_right1 = col_next1[col];
        const uint32_t col_left2 = col_prev2[col];
        const uint32_t col_right2 = col_next2[col];
        const size_t idx = row_offset + col;
        const size_t idx_left = row_offset + col_left1;
        const size_t idx_right = row_offset + col_right1;
        const size_t idx_up = row_up1_offset + col;
        const size_t idx_down = row_down1_offset + col;
        const size_t idx_same_left = row_offset + col_left2;
        const size_t idx_same_right = row_offset + col_right2;
        const size_t idx_same_up = row_up2_offset + col;
        const size_t idx_same_down = row_down2_offset + col;
        const int color = cfa[idx];
        if (color == 1 || color == 3) {
          continue;
        }

        const double center = mosaic[idx];
        const double g_left = green[idx_left];
        const double g_right = green[idx_right];
        const double g_up = green[idx_up];
        const double g_down = green[idx_down];
        const double same_left = mosaic[idx_same_left];
        const double same_right = mosaic[idx_same_right];
        const double same_up = mosaic[idx_same_up];
        const double same_down = mosaic[idx_same_down];

        const double horiz = 0.5 * (g_left + g_right) +
                             0.25 * (2.0 * center - same_left - same_right);
        const double vert = 0.5 * (g_up + g_down) +
                            0.25 * (2.0 * center - same_up - same_down);
        const double grad_h = std::abs(g_left - g_right) +
                              0.5 * std::abs(2.0 * center - same_left - same_right);
        const double grad_v = std::abs(g_up - g_down) +
                              0.5 * std::abs(2.0 * center - same_up - same_down);

        const double guide_center = upsampled_stack_guide[idx];
        const double guide_left = upsampled_stack_guide[idx_left];
        const double guide_right = upsampled_stack_guide[idx_right];
        const double guide_up = upsampled_stack_guide[idx_up];
        const double guide_down = upsampled_stack_guide[idx_down];
        const double guide_same_left = upsampled_stack_guide[idx_same_left];
        const double guide_same_right = upsampled_stack_guide[idx_same_right];
        const double guide_same_up = upsampled_stack_guide[idx_same_up];
        const double guide_same_down = upsampled_stack_guide[idx_same_down];

        const double guide_grad_h =
            std::abs(guide_left - guide_right) +
            0.5 * std::abs(2.0 * guide_center - guide_same_left - guide_same_right);
        const double guide_grad_v =
            std::abs(guide_up - guide_down) +
            0.5 * std::abs(2.0 * guide_center - guide_same_up - guide_same_down);

        const double guide_conf = SampleRegionMap(stability_sampler, row, col, 1.0);
        const double alias_conf = SampleRegionMap(alias_sampler, row, col, 0.0);
        const double guide_mix =
            std::clamp(0.08 + 0.12 * guide_conf + 0.10 * alias_conf, 0.08, 0.30);
        const double combined_grad_h = (1.0 - guide_mix) * grad_h + guide_mix * guide_grad_h;
        const double combined_grad_v = (1.0 - guide_mix) * grad_v + guide_mix * guide_grad_v;
        const double tensor_h = SampleRegionMap(tensor_x_sampler, row, col, 0.5);
        const double tensor_v = SampleRegionMap(tensor_y_sampler, row, col, 0.5);
        const double tensor_coh = SampleRegionMap(tensor_coherence_sampler, row, col, 0.0);
        const double tensor_penalty_h = 1.0 + 0.85 * tensor_coh * tensor_h;
        const double tensor_penalty_v = 1.0 + 0.85 * tensor_coh * tensor_v;
        const double weight_h = 1.0 / ((combined_grad_h + 1.0) * tensor_penalty_h);
        const double weight_v = 1.0 / ((combined_grad_v + 1.0) * tensor_penalty_v);

        double estimate = 0.0;
        if (std::max(weight_h, weight_v) > 1.8 * std::min(weight_h, weight_v)) {
          estimate = weight_h > weight_v ? horiz : vert;
        } else {
          estimate = (weight_h * horiz + weight_v * vert) / (weight_h + weight_v);
        }

        const double guide_anisotropy =
            std::abs(guide_grad_h - guide_grad_v) / (guide_grad_h + guide_grad_v + 1.0);
        const double blend_weight =
            std::clamp(0.04 +
                           0.08 * guide_conf * guide_anisotropy +
                           0.10 * alias_conf * guide_anisotropy,
                       0.04,
                       0.18);
        refined_green[idx] = static_cast<WorkingValue>(std::clamp(
          (1.0 - blend_weight) * static_cast<double>(green[idx]) + blend_weight * estimate,
          0.0,
          65535.0 - pedestal));
      }
    }
    green.swap(refined_green);
  }

  hiraco::LogTiming("raw", "Guide-aware green refinement", std::chrono::steady_clock::now() - guide_refine_start);

  auto blur5 = [&](const std::vector<WorkingValue>& src, std::vector<WorkingValue>* dst) {
    dst->assign(pixel_count, 0.0f);
    std::vector<WorkingValue> temp(pixel_count, 0.0f);
    constexpr double kNorm = 1.0 / 16.0;

    #pragma omp parallel for schedule(static) if(height > 100)
    for (uint32_t row = 0; row < height; ++row) {
      const size_t row_offset = static_cast<size_t>(row) * width;
      for (uint32_t col = 0; col < width; ++col) {
        const uint32_t col_left2 = col_prev2[col];
        const uint32_t col_left1 = col_prev1[col];
        const uint32_t col_right1 = col_next1[col];
        const uint32_t col_right2 = col_next2[col];
        const double sum = static_cast<double>(src[row_offset + col_left2]) +
                           4.0 * static_cast<double>(src[row_offset + col_left1]) +
                           6.0 * static_cast<double>(src[row_offset + col]) +
                           4.0 * static_cast<double>(src[row_offset + col_right1]) +
                           static_cast<double>(src[row_offset + col_right2]);
        temp[row_offset + col] = static_cast<WorkingValue>(sum * kNorm);
      }
    }

    #pragma omp parallel for schedule(static) if(height > 100)
    for (uint32_t row = 0; row < height; ++row) {
      const size_t row_offset = static_cast<size_t>(row) * width;
      const size_t row_up2_offset = static_cast<size_t>(row_prev2[row]) * width;
      const size_t row_up1_offset = static_cast<size_t>(row_prev1[row]) * width;
      const size_t row_down1_offset = static_cast<size_t>(row_next1[row]) * width;
      const size_t row_down2_offset = static_cast<size_t>(row_next2[row]) * width;
      for (uint32_t col = 0; col < width; ++col) {
        const double sum = static_cast<double>(temp[row_up2_offset + col]) +
                           4.0 * static_cast<double>(temp[row_up1_offset + col]) +
                           6.0 * static_cast<double>(temp[row_offset + col]) +
                           4.0 * static_cast<double>(temp[row_down1_offset + col]) +
                           static_cast<double>(temp[row_down2_offset + col]);
        (*dst)[row_offset + col] = static_cast<WorkingValue>(sum * kNorm);
      }
    }
  };

  const auto detail_lift_start = std::chrono::steady_clock::now();
  if (HasRegionMapSampler(stability_sampler)) {
    std::vector<WorkingValue> green_blur;
    blur5(green, &green_blur);
    const double detail_strength =
        std::clamp(1.50 + 0.50 * std::max(metadata.predicted_detail_gain - 1.0, 0.0), 1.20, 2.50);

    #pragma omp parallel for schedule(static) if(height > 100)
    for (uint32_t row = 0; row < height; ++row) {
      const uint32_t row_up1 = row_prev1[row];
      const uint32_t row_down1 = row_next1[row];
      const size_t row_offset = static_cast<size_t>(row) * width;
      const size_t row_up1_offset = static_cast<size_t>(row_up1) * width;
      const size_t row_down1_offset = static_cast<size_t>(row_down1) * width;
      for (uint32_t col = 0; col < width; ++col) {
        const uint32_t col_left1 = col_prev1[col];
        const uint32_t col_right1 = col_next1[col];
        const size_t idx = row_offset + col;
        const size_t idx_left = row_offset + col_left1;
        const size_t idx_right = row_offset + col_right1;
        const size_t idx_up = row_up1_offset + col;
        const size_t idx_down = row_down1_offset + col;
        const double green_detail = green[idx] - green_blur[idx];
        const double g_left = sample_green_site(idx_left);
        const double g_right = sample_green_site(idx_right);
        const double g_up = sample_green_site(idx_up);
        const double g_down = sample_green_site(idx_down);

        const double split_hv = std::abs(0.5 * (g_left + g_right) - 0.5 * (g_up + g_down));
        const double local_scale = std::max(static_cast<double>(green_blur[idx]), 512.0);
        const double split_confidence = std::exp(-std::pow(split_hv / (0.025 * local_scale + 16.0), 2.0));
        const double alias_conf = SampleRegionMap(alias_sampler, row, col, 0.0);
        double mask = SampleRegionMap(stability_sampler, row, col, 1.0) *
                std::clamp(split_confidence, 0.20, 1.0);
        double detail_boost = 1.0 + 0.28 * alias_conf;
        double cap_scale = 1.0 + 0.18 * alias_conf;
        if (!upsampled_stack_guide.empty()) {
          const double guide_left = upsampled_stack_guide[idx_left];
          const double guide_right = upsampled_stack_guide[idx_right];
          const double guide_up = upsampled_stack_guide[idx_up];
          const double guide_down = upsampled_stack_guide[idx_down];
          const double guide_edge =
              std::max(std::abs(guide_left - guide_right), std::abs(guide_up - guide_down));
          const double guide_edge_conf =
              std::clamp(guide_edge / (0.035 * local_scale + 24.0), 0.0, 1.0);
          mask *= 0.95 + 0.05 * guide_edge_conf;
          detail_boost *= 1.0 + 0.18 * alias_conf * guide_edge_conf;
          cap_scale *= 1.0 + 0.12 * alias_conf * guide_edge_conf;
        }
        const double capped_detail =
            std::clamp(green_detail,
                       -(0.45 * cap_scale) * local_scale - 320.0,
                        (0.45 * cap_scale) * local_scale + 320.0);
        green[idx] = static_cast<WorkingValue>(std::clamp(
            static_cast<double>(green[idx]) + detail_strength * detail_boost * mask * capped_detail,
            0.0,
            65535.0 - pedestal));
      }
    }
  }

  hiraco::LogTiming("raw", "Stability-driven green detail lift", std::chrono::steady_clock::now() - detail_lift_start);

  const auto rgb_start = std::chrono::steady_clock::now();
  std::vector<WorkingValue> red_diff(pixel_count, 0.0f);
  std::vector<WorkingValue> blue_diff(pixel_count, 0.0f);
  #pragma omp parallel for schedule(static) if(pixel_count > 100000)
  for (size_t idx = 0; idx < pixel_count; ++idx) {
    if (cfa[idx] == 0) {
      red_diff[idx] = static_cast<WorkingValue>(static_cast<double>(mosaic[idx]) - static_cast<double>(green[idx]));
    } else if (cfa[idx] == 2) {
      blue_diff[idx] = static_cast<WorkingValue>(static_cast<double>(mosaic[idx]) - static_cast<double>(green[idx]));
    }
  }

  auto interp_axis_diff = [&](uint32_t row,
                              uint32_t col,
                              int target_color,
                              bool horizontal,
                              double guide_weight) -> double {
    const uint32_t r0 = horizontal ? row : row_prev1[row];
    const uint32_t c0 = horizontal ? col_prev1[col] : col;
    const uint32_t r1 = horizontal ? row : row_next1[row];
    const uint32_t c1 = horizontal ? col_next1[col] : col;
    const size_t idx0 = static_cast<size_t>(r0) * width + c0;
    const size_t idx1 = static_cast<size_t>(r1) * width + c1;
    const size_t idxc = static_cast<size_t>(row) * width + col;
    const double d0 = target_color == 0 ? red_diff[idx0] : blue_diff[idx0];
    const double d1 = target_color == 0 ? red_diff[idx1] : blue_diff[idx1];
    const double g0 = green[idx0];
    const double g1 = green[idx1];
    const double gc = green[idxc];
    double guide_term0 = 0.0;
    double guide_term1 = 0.0;
    if (!upsampled_stack_guide.empty()) {
      const double guide_center = upsampled_stack_guide[idxc];
      guide_term0 = std::abs(upsampled_stack_guide[idx0] - guide_center);
      guide_term1 = std::abs(upsampled_stack_guide[idx1] - guide_center);
    }
    const double w0 = 1.0 / (std::abs(g0 - gc) + guide_weight * guide_term0 + 1.0);
    const double w1 = 1.0 / (std::abs(g1 - gc) + guide_weight * guide_term1 + 1.0);
    return (w0 * d0 + w1 * d1) / (w0 + w1);
  };

  auto interp_diag_diff = [&](uint32_t row,
                              uint32_t col,
                              int target_color,
                              double guide_weight) -> double {
    const uint32_t r_n = row_prev1[row];
    const uint32_t r_s = row_next1[row];
    const uint32_t c_w = col_prev1[col];
    const uint32_t c_e = col_next1[col];

    const size_t idx_nw = static_cast<size_t>(r_n) * width + c_w;
    const size_t idx_ne = static_cast<size_t>(r_n) * width + c_e;
    const size_t idx_sw = static_cast<size_t>(r_s) * width + c_w;
    const size_t idx_se = static_cast<size_t>(r_s) * width + c_e;

    const double d_nw = target_color == 0 ? red_diff[idx_nw] : blue_diff[idx_nw];
    const double d_ne = target_color == 0 ? red_diff[idx_ne] : blue_diff[idx_ne];
    const double d_sw = target_color == 0 ? red_diff[idx_sw] : blue_diff[idx_sw];
    const double d_se = target_color == 0 ? red_diff[idx_se] : blue_diff[idx_se];

    const double g_diag_a = std::abs(green[idx_nw] - green[idx_se]);
    const double g_diag_b = std::abs(green[idx_ne] - green[idx_sw]);
    double guide_diag_a = 0.0;
    double guide_diag_b = 0.0;
    if (!upsampled_stack_guide.empty()) {
      guide_diag_a = std::abs(upsampled_stack_guide[idx_nw] - upsampled_stack_guide[idx_se]);
      guide_diag_b = std::abs(upsampled_stack_guide[idx_ne] - upsampled_stack_guide[idx_sw]);
    }
    const double w_a = 1.0 / (g_diag_a + guide_weight * guide_diag_a + 1.0);
    const double w_b = 1.0 / (g_diag_b + guide_weight * guide_diag_b + 1.0);
    const double avg_a = 0.5 * (d_nw + d_se);
    const double avg_b = 0.5 * (d_ne + d_sw);
    return (w_a * avg_a + w_b * avg_b) / (w_a + w_b);
  };

  #pragma omp parallel for schedule(static) if(height > 100)
  for (uint32_t row = 0; row < height; ++row) {
    for (uint32_t col = 0; col < width; ++col) {
      const size_t idx = static_cast<size_t>(row) * width + col;
      const int color = cfa[idx];
      const double guide_weight = !upsampled_stack_guide.empty()
                                      ? 0.10 * SampleRegionMap(stability_sampler, row, col, 1.0) +
                                            0.08 * SampleRegionMap(alias_sampler, row, col, 0.0)
                                      : 0.0;

      double red = 0.0;
      double green_value = green[idx];
      double blue = 0.0;

      if (color == 0) {
        red = mosaic[idx];
        blue = green_value + interp_diag_diff(row, col, 2, guide_weight);
      } else if (color == 2) {
        red = green_value + interp_diag_diff(row, col, 0, guide_weight);
        blue = mosaic[idx];
      } else if (color == 1) {
        red = green_value + interp_axis_diff(row, col, 0, true, guide_weight);
        blue = green_value + interp_axis_diff(row, col, 2, false, guide_weight);
      } else {
        red = green_value + interp_axis_diff(row, col, 0, false, guide_weight);
        blue = green_value + interp_axis_diff(row, col, 2, true, guide_weight);
      }

      red = std::clamp(red, 0.0, 65535.0 - pedestal);
      green_value = std::clamp(green_value, 0.0, 65535.0 - pedestal);
      blue = std::clamp(blue, 0.0, 65535.0 - pedestal);

      const size_t px = idx * 3;
      output->pixels[px + 0] = clamp_channel(red);
      output->pixels[px + 1] = clamp_channel(green_value);
      output->pixels[px + 2] = clamp_channel(blue);
    }
  }

  hiraco::LogTiming("raw", "Reconstruct RGB output", std::chrono::steady_clock::now() - rgb_start);

  return true;
}

bool BuildLinearDngPayload(const std::string& source_path,
                           const SourceLinearDngMetadata& metadata,
                           const LibRawOverrideSet& libraw_overrides,
                           LinearDngPayload* payload,
                           std::string* error_message) {
  const hiraco::ScopedTimingLog payload_timer("convert", "Build linear DNG payload");
  const RenderSettings raw_settings = BuildRawRenderSettings(metadata, libraw_overrides);
  const bool needs_cfa_guide = metadata.has_predicted_detail_gain &&
                               metadata.predicted_detail_gain > 1.0001 &&
                               IsOm3HighResMetadata(metadata);

  std::future<GuideExtractionResult> guide_future;
  if (needs_cfa_guide) {
    guide_future = std::async(std::launch::async,
                              [&source_path, &metadata]() {
                                GuideExtractionResult result;
                                result.ok = ExtractCfaGuideImage(source_path,
                                                                 metadata,
                                                                 &result.guide_image,
                                                                 &result.error);
                                return result;
                              });
  }

  payload->raw_image_is_camera_space = false;
  if (IsOm3HighResMetadata(metadata)) {
    const hiraco::ScopedTimingLog timer("convert", "Render raw payload");
    if (!RenderOm3RawDomainImage(source_path, metadata, nullptr, &payload->raw_image, error_message)) {
      return false;
    }
    payload->raw_image_is_camera_space = true;
  } else {
    const hiraco::ScopedTimingLog timer("convert", "Render LibRaw payload");
    if (!RenderLibRawImage(source_path, metadata, raw_settings, &payload->raw_image, error_message)) {
      return false;
    }
  }

  if (needs_cfa_guide) {
    const hiraco::ScopedTimingLog timer("convert", "Finalize CFA guide extraction");
    GuideExtractionResult guide_result = guide_future.get();
    if (guide_result.ok) {
      payload->cfa_guide_image = std::move(guide_result.guide_image);
    } else {
      payload->cfa_guide_image = RasterImage();
    }
  } else {
    payload->cfa_guide_image = RasterImage();
  }

  return true;
}

PreviewImage BuildPreviewImage(const RasterImage& source, uint32_t max_dimension) {
  PreviewImage preview;
  preview.colors = source.colors;

  if (source.width == 0 || source.height == 0 || source.colors == 0) {
    return preview;
  }

  const uint32_t longest_edge = std::max(source.width, source.height);
  if (max_dimension == 0 || longest_edge <= max_dimension) {
    preview.width = source.width;
    preview.height = source.height;
  } else if (source.width >= source.height) {
    preview.width = max_dimension;
    preview.height = std::max<uint32_t>(1, static_cast<uint32_t>((static_cast<uint64_t>(source.height) * max_dimension) / source.width));
  } else {
    preview.height = max_dimension;
    preview.width = std::max<uint32_t>(1, static_cast<uint32_t>((static_cast<uint64_t>(source.width) * max_dimension) / source.height));
  }

  preview.pixels.resize(static_cast<size_t>(preview.width) * preview.height * preview.colors);

  for (uint32_t row = 0; row < preview.height; ++row) {
    const uint32_t source_row = std::min<uint32_t>(source.height - 1,
                                                   static_cast<uint32_t>((static_cast<uint64_t>(row) * source.height) / preview.height));
    for (uint32_t col = 0; col < preview.width; ++col) {
      const uint32_t source_col = std::min<uint32_t>(source.width - 1,
                                                     static_cast<uint32_t>((static_cast<uint64_t>(col) * source.width) / preview.width));
      const size_t source_index = (static_cast<size_t>(source_row) * source.width + source_col) * source.colors;
      const size_t preview_index = (static_cast<size_t>(row) * preview.width + col) * preview.colors;
      for (uint32_t channel = 0; channel < preview.colors; ++channel) {
        preview.pixels[preview_index + channel] = static_cast<uint8_t>(source.pixels[source_index + channel] >> 8);
      }
    }
  }

  return preview;
}

PreviewImage BuildPreviewImage(const PreviewImage& source, uint32_t max_dimension) {
  PreviewImage preview;
  preview.colors = source.colors;
  preview.bits = source.bits;

  if (source.width == 0 || source.height == 0 || source.colors == 0) {
    return preview;
  }

  const uint32_t longest_edge = std::max(source.width, source.height);
  if (max_dimension == 0 || longest_edge <= max_dimension) {
    preview.width = source.width;
    preview.height = source.height;
  } else if (source.width >= source.height) {
    preview.width = max_dimension;
    preview.height = std::max<uint32_t>(1, static_cast<uint32_t>((static_cast<uint64_t>(source.height) * max_dimension) / source.width));
  } else {
    preview.height = max_dimension;
    preview.width = std::max<uint32_t>(1, static_cast<uint32_t>((static_cast<uint64_t>(source.width) * max_dimension) / source.height));
  }

  preview.pixels.resize(static_cast<size_t>(preview.width) * preview.height * preview.colors);

  for (uint32_t row = 0; row < preview.height; ++row) {
    const uint32_t source_row = std::min<uint32_t>(source.height - 1,
                                                   static_cast<uint32_t>((static_cast<uint64_t>(row) * source.height) / preview.height));
    for (uint32_t col = 0; col < preview.width; ++col) {
      const uint32_t source_col = std::min<uint32_t>(source.width - 1,
                                                     static_cast<uint32_t>((static_cast<uint64_t>(col) * source.width) / preview.width));
      const size_t source_index = (static_cast<size_t>(source_row) * source.width + source_col) * source.colors;
      const size_t preview_index = (static_cast<size_t>(row) * preview.width + col) * preview.colors;
      for (uint32_t channel = 0; channel < preview.colors; ++channel) {
        preview.pixels[preview_index + channel] = source.pixels[source_index + channel];
      }
    }
  }

  return preview;
}

RasterImage BuildScaledRasterImage(const RasterImage& source, uint32_t max_dimension) {
  RasterImage scaled;
  scaled.colors = source.colors;
  scaled.bits = source.bits;

  if (source.width == 0 || source.height == 0 || source.colors == 0) {
    return scaled;
  }

  const uint32_t longest_edge = std::max(source.width, source.height);
  if (max_dimension == 0 || longest_edge <= max_dimension) {
    scaled.width = source.width;
    scaled.height = source.height;
  } else if (source.width >= source.height) {
    scaled.width = max_dimension;
    scaled.height = std::max<uint32_t>(1, static_cast<uint32_t>((static_cast<uint64_t>(source.height) * max_dimension) / source.width));
  } else {
    scaled.height = max_dimension;
    scaled.width = std::max<uint32_t>(1, static_cast<uint32_t>((static_cast<uint64_t>(source.width) * max_dimension) / source.height));
  }

  scaled.pixels.resize(static_cast<size_t>(scaled.width) * scaled.height * scaled.colors);

  for (uint32_t row = 0; row < scaled.height; ++row) {
    const uint32_t source_row = std::min<uint32_t>(source.height - 1,
                                                   static_cast<uint32_t>((static_cast<uint64_t>(row) * source.height) / scaled.height));
    for (uint32_t col = 0; col < scaled.width; ++col) {
      const uint32_t source_col = std::min<uint32_t>(source.width - 1,
                                                     static_cast<uint32_t>((static_cast<uint64_t>(col) * source.width) / scaled.width));
      const size_t source_index = (static_cast<size_t>(source_row) * source.width + source_col) * source.colors;
      const size_t scaled_index = (static_cast<size_t>(row) * scaled.width + col) * scaled.colors;
      for (uint32_t channel = 0; channel < scaled.colors; ++channel) {
        scaled.pixels[scaled_index + channel] = source.pixels[source_index + channel];
      }
    }
  }

  return scaled;
}

void ApplyLibRawOrientationToPreview(int libraw_flip, PreviewImage* preview) {
  if (preview == nullptr || preview->width == 0 || preview->height == 0 || preview->colors == 0) {
    return;
  }

  const int normalized_flip = NormalizeLibRawFlip(libraw_flip);
  if (normalized_flip == 0) {
    return;
  }

  const uint32_t source_width = preview->width;
  const uint32_t source_height = preview->height;
  const uint32_t target_width = OrientedImageWidth(source_width, source_height, normalized_flip);
  const uint32_t target_height = OrientedImageHeight(source_width, source_height, normalized_flip);
  std::vector<uint8_t> rotated(static_cast<size_t>(target_width) * target_height * preview->colors, 0);

  for (uint32_t row = 0; row < source_height; ++row) {
    for (uint32_t col = 0; col < source_width; ++col) {
      uint32_t target_col = col;
      uint32_t target_row = row;
      if ((normalized_flip & 1) != 0) {
        target_col = source_width - 1 - target_col;
      }
      if ((normalized_flip & 2) != 0) {
        target_row = source_height - 1 - target_row;
      }
      if ((normalized_flip & 4) != 0) {
        std::swap(target_col, target_row);
      }

      const size_t source_index = (static_cast<size_t>(row) * source_width + col) * preview->colors;
      const size_t target_index = (static_cast<size_t>(target_row) * target_width + target_col) * preview->colors;
      for (uint32_t channel = 0; channel < preview->colors; ++channel) {
        rotated[target_index + channel] = preview->pixels[source_index + channel];
      }
    }
  }

  preview->width = target_width;
  preview->height = target_height;
  preview->pixels = std::move(rotated);
}

int InverseLibRawFlip(int libraw_flip) {
  switch (NormalizeLibRawFlip(libraw_flip)) {
    case 5:
      return 6;
    case 6:
      return 5;
    default:
      return NormalizeLibRawFlip(libraw_flip);
  }
}

PreviewImage BuildDisplayPreviewFromRaster(const SourceLinearDngMetadata& metadata,
                                          const RasterImage& source,
                                          bool source_is_camera_space,
                                          uint32_t max_dimension,
                                          double extra_linear_gain = 1.0) {
  RasterImage preview_raster = BuildScaledRasterImage(source, max_dimension);
  if (source_is_camera_space) {
    ApplySourceDrivenLinearPreviewTransform(metadata, &preview_raster);
    ApplyLinearExposureCompensation(SourceDrivenBaselineExposure(metadata, source),
                                    &preview_raster);
  }
  ApplyLinearGain(extra_linear_gain, &preview_raster);
  ApplyLinearSrgbGamma(&preview_raster);
  PreviewImage preview = BuildPreviewImage(preview_raster, 0);
  ApplyLibRawOrientationToPreview(metadata.libraw_flip, &preview);
  return preview;
}

CropRect ClampCropRectToBounds(const CropRect& requested,
                               uint32_t width,
                               uint32_t height) {
  CropRect clamped = requested;
  clamped.x = std::min(clamped.x, width > 0 ? width - 1 : 0);
  clamped.y = std::min(clamped.y, height > 0 ? height - 1 : 0);
  clamped.width = std::max(1u, std::min(clamped.width, width - clamped.x));
  clamped.height = std::max(1u, std::min(clamped.height, height - clamped.y));
  return clamped;
}

CropRect ExpandCropRectWithBorder(const CropRect& crop,
                                  uint32_t width,
                                  uint32_t height,
                                  uint32_t border) {
  const CropRect clamped = ClampCropRectToBounds(crop, width, height);

  CropRect expanded;
  expanded.x = clamped.x > border ? clamped.x - border : 0;
  expanded.y = clamped.y > border ? clamped.y - border : 0;
  const uint32_t right = std::min(width, clamped.x + clamped.width + border);
  const uint32_t bottom = std::min(height, clamped.y + clamped.height + border);
  expanded.width = right - expanded.x;
  expanded.height = bottom - expanded.y;
  return expanded;
}

RasterImage CropRasterImageWithBorder(const RasterImage& source,
                                      const CropRect& crop,
                                      uint32_t border) {
  RasterImage result;
  result.width = crop.width + 2 * border;
  result.height = crop.height + 2 * border;
  result.colors = source.colors;
  result.bits = source.bits;
  result.pixels.resize(static_cast<size_t>(result.width) * result.height * result.colors, 0);

  for (uint32_t row = 0; row < result.height; ++row) {
    const int source_row = std::clamp(static_cast<int>(crop.y) + static_cast<int>(row) - static_cast<int>(border),
                                      0,
                                      static_cast<int>(source.height) - 1);
    for (uint32_t col = 0; col < result.width; ++col) {
      const int source_col = std::clamp(static_cast<int>(crop.x) + static_cast<int>(col) - static_cast<int>(border),
                                        0,
                                        static_cast<int>(source.width) - 1);
      const size_t src_index =
          (static_cast<size_t>(source_row) * source.width + static_cast<uint32_t>(source_col)) * source.colors;
      const size_t dst_index =
          (static_cast<size_t>(row) * result.width + col) * result.colors;
      for (uint32_t channel = 0; channel < result.colors; ++channel) {
        result.pixels[dst_index + channel] = source.pixels[src_index + channel];
      }
    }
  }

  return result;
}

RasterImage CropRasterImage(const RasterImage& source,
                           const CropRect& crop) {
  RasterImage result;
  result.width = crop.width;
  result.height = crop.height;
  result.colors = source.colors;
  result.bits = source.bits;
  result.pixels.resize(static_cast<size_t>(result.width) * result.height * result.colors, 0);

  for (uint32_t row = 0; row < result.height; ++row) {
    const size_t src_row = static_cast<size_t>(crop.y + row) * source.width;
    const size_t dst_row = static_cast<size_t>(row) * result.width;
    for (uint32_t col = 0; col < result.width; ++col) {
      const size_t src_index = (src_row + crop.x + col) * source.colors;
      const size_t dst_index = (dst_row + col) * result.colors;
      for (uint32_t channel = 0; channel < result.colors; ++channel) {
        result.pixels[dst_index + channel] = source.pixels[src_index + channel];
      }
    }
  }

  return result;
}

AutoPtr<dng_image> MakeUint16Image(dng_host& host, const RasterImage& image) {
  dng_rect bounds(image.height, image.width);
  AutoPtr<dng_image> dng_image_ptr(host.Make_dng_image(bounds,
                                                       image.colors,
                                                       ttShort));
  dng_pixel_buffer buffer(bounds,
                          0,
                          image.colors,
                          ttShort,
                          pcInterleaved,
                          const_cast<uint16_t*>(image.pixels.data()));
  dng_image_ptr->Put(buffer);
  return AutoPtr<dng_image>(dng_image_ptr.Release());
}

AutoPtr<dng_image> MakeUint8Image(dng_host& host, const PreviewImage& image) {
  dng_rect bounds(image.height, image.width);
  AutoPtr<dng_image> dng_image_ptr(host.Make_dng_image(bounds,
                                                       image.colors,
                                                       ttByte));
  dng_pixel_buffer buffer(bounds,
                          0,
                          image.colors,
                          ttByte,
                          pcInterleaved,
                          const_cast<uint8_t*>(image.pixels.data()));
  dng_image_ptr->Put(buffer);
  return AutoPtr<dng_image>(dng_image_ptr.Release());
}

void AttachLinearSrgbProfile(dng_negative& negative) {
  AutoPtr<dng_camera_profile> profile(new dng_camera_profile());
  profile->SetName("hiraco-linear-srgb");
  profile->SetCalibrationIlluminant1(lsD65);
  profile->SetColorMatrix1(dng_matrix_3by3(
      3.2404542, -1.5371385, -0.4985314,
      -0.9692660, 1.8760108, 0.0415560,
      0.0556434, -0.2040259, 1.0572252));
  profile->SetForwardMatrix1(dng_matrix_3by3(
      0.4124564, 0.3575761, 0.1804375,
      0.2126729, 0.7151522, 0.0721750,
      0.0193339, 0.1191920, 0.9503041));
  negative.AddProfile(profile);
  negative.SetAsShotProfileName("hiraco-linear-srgb");
}

void AttachOm3AdobeLikeProfile(dng_negative& negative) {
  AutoPtr<dng_camera_profile> profile(new dng_camera_profile());
  profile->SetName("Adobe Standard");
  profile->SetCalibrationIlluminant1(lsStandardLightA);
  profile->SetCalibrationIlluminant2(lsD65);
  profile->SetColorMatrix1(dng_matrix_3by3(
      1.0602, -0.5977, 0.0574,
      -0.3109, 1.1296, 0.2063,
      0.0002, 0.0521, 0.6609));
  profile->SetColorMatrix2(dng_matrix_3by3(
      0.9090, -0.3591, -0.0756,
      -0.3252, 1.1396, 0.2109,
      -0.0318, 0.1059, 0.5606));
  profile->SetForwardMatrix1(dng_matrix_3by3(
      0.4436, 0.4158, 0.1049,
      0.1597, 0.8100, 0.0304,
      0.0503, 0.0022, 0.7725));
  profile->SetForwardMatrix2(dng_matrix_3by3(
      0.4504, 0.3593, 0.1546,
      0.2285, 0.7248, 0.0467,
      0.1088, 0.0045, 0.7118));
  negative.AddProfile(profile);
  negative.SetAsShotProfileName("Adobe Standard");
}

void SetSourceCameraNeutral(const SourceLinearDngMetadata& metadata,
                            dng_negative& negative) {
  if (!metadata.has_as_shot_neutral) {
    return;
  }

  dng_vector camera_neutral(3);
  camera_neutral[0] = metadata.as_shot_neutral[0];
  camera_neutral[1] = metadata.as_shot_neutral[1];
  camera_neutral[2] = metadata.as_shot_neutral[2];
  negative.SetCameraNeutral(camera_neutral);
}

void PopulateLinearRawNegative(dng_host& host,
                               const std::string& source_path,
                               const RasterImage& raw_image,
                               const SourceLinearDngMetadata& metadata,
                               dng_negative& negative,
                               double preview_auto_bright_gain) {
  const std::string model_name = metadata.unique_camera_model.empty() ? "hiraco" : metadata.unique_camera_model;
  const std::string local_name = metadata.model.empty() ? "hiraco" : metadata.model;

  negative.SetModelName(model_name.c_str());
  negative.SetLocalName(local_name.c_str());
  negative.SetOriginalRawFileName(std::filesystem::path(source_path).filename().string().c_str());
  negative.SetColorChannels(raw_image.colors);
  negative.SetBaseOrientation(dng_orientation::TIFFtoDNG(LibRawFlipToTiffOrientation(metadata.libraw_flip)));
  if (metadata.has_default_crop) {
    negative.SetDefaultCropOrigin(metadata.default_crop_origin_h, metadata.default_crop_origin_v);
    negative.SetDefaultCropSize(metadata.default_crop_width, metadata.default_crop_height);
  } else {
    negative.SetDefaultCropOrigin(0, 0);
    negative.SetDefaultCropSize(raw_image.width, raw_image.height);
  }
  negative.SetDefaultScale(dng_urational(1, 1), dng_urational(1, 1));
  negative.SetRawDefaultCrop();
  negative.SetRawDefaultScale();
  negative.SetRawBestQualityScale();
  if (ShouldApplyOm3SourceDrivenLinearTransform(metadata, raw_image) && metadata.has_black_level) {
    negative.SetBlackLevel(metadata.black_level);
  } else {
    negative.SetBlackLevel(0.0);
  }
  negative.SetWhiteLevel((1u << raw_image.bits) - 1);
  negative.SetBaselineExposure(std::log2(std::max(preview_auto_bright_gain, 1e-6)));
  negative.SetLinearResponseLimit(1.0);

  if (dng_exif* exif = negative.GetExif()) {
    if (!metadata.make.empty()) {
      exif->fMake.Set(metadata.make.c_str());
    }
    if (!metadata.model.empty()) {
      exif->fModel.Set(metadata.model.c_str());
    }
    if (metadata.has_exif_iso) {
      exif->fISOSpeedRatings[0] = static_cast<uint32_t>(metadata.exif_iso);
    }
    if (metadata.has_exif_shutter_speed) {
      exif->SetExposureTime(metadata.exif_shutter_speed, true);
    }
    if (metadata.has_exif_aperture) {
      exif->SetFNumber(metadata.exif_aperture);
    }
  }

  negative.UpdateDateTimeToNow();

  dng_vector analog_balance(raw_image.colors);
  analog_balance.SetIdentity(raw_image.colors);
  negative.SetAnalogBalance(analog_balance);

  if (ShouldUseOm3AdobeMetadata(metadata, raw_image)) {
    AttachOm3AdobeLikeProfile(negative);
    SetSourceCameraNeutral(metadata, negative);
  } else {
    AttachLinearSrgbProfile(negative);
    dng_vector camera_neutral(3);
    camera_neutral.SetIdentity(3);
    negative.SetCameraNeutral(camera_neutral);
    negative.SetCameraWhiteXY(D50_xy_coord());
  }

  AutoPtr<dng_image> image = MakeUint16Image(host, raw_image);
  negative.SetStage3Image(image);
  negative.SynchronizeMetadata();
}

void AppendImagePreview(dng_host& host,
                        const PreviewImage& preview_image,
                        dng_preview_list* preview_list) {
  if (preview_image.width == 0 || preview_image.height == 0) {
    return;
  }

  AutoPtr<dng_preview> preview(new dng_image_preview());
  preview->fInfo.fApplicationName.Set("hiraco");
  preview->fInfo.fSettingsName.Set("Default");
  preview->fInfo.fColorSpace = previewColorSpace_sRGB;

  AutoPtr<dng_image> preview_dng_image = MakeUint8Image(host, preview_image);
  preview->SetImage(host, preview_dng_image);
  preview_list->Append(preview);
}

}  // namespace

ResolvedStageSettings ResolveStageSettingsForImage(const SourceLinearDngMetadata& metadata,
                                                   uint32_t width,
                                                   uint32_t height,
                                                   const StageOverrideSet& overrides) {
  return ResolveStageSettingsForImageImpl(metadata, width, height, overrides);
}

namespace {

class CallbackAbortSniffer final : public dng_abort_sniffer {
 public:
  CallbackAbortSniffer(ProgressCallback progress, CancelCheck cancel)
      : progress_(std::move(progress)), cancel_(std::move(cancel)) {}

  bool ThreadSafe() const override {
    return true;
  }

 protected:
  void Sniff() override {
    if (IsCancelled(cancel_)) {
      ThrowUserCanceled();
    }
  }

  void StartTask(const char* name, real64 /* fract */) override {
    current_task_ = (name != nullptr && *name != '\0') ? name : "Writing DNG";
    ReportProgress(progress_, "write", 0.0, current_task_);
  }

  void UpdateProgress(real64 fract) override {
    ReportProgress(progress_,
                   "write",
                   std::clamp(static_cast<double>(fract), 0.0, 1.0),
                   current_task_.empty() ? "Writing DNG" : current_task_);
  }

 private:
  ProgressCallback progress_;
  CancelCheck cancel_;
  std::string current_task_;
};

}  // namespace

bool BuildOriginalPreviewFromRaw(const std::string& source_path,
                                 const SourceLinearDngMetadata& metadata,
                                 const LibRawOverrideSet& libraw_overrides,
                                 std::shared_ptr<PreviewImage> preview,
                                 ProgressCallback progress,
                                 CancelCheck cancel,
                                 std::string* error_message) {
  const hiraco::ScopedTimingLog preview_timer("preview", "Build original preview");
  if (!preview) {
    if (error_message != nullptr) {
      *error_message = "missing preview output";
    }
    return false;
  }

  ReportProgress(progress, "preview", 0.05, "Rendering original preview");
  if (CheckCancelled(cancel, error_message)) {
    return false;
  }

  RasterImage rendered;
  const RenderSettings preview_settings = BuildPreviewRenderSettings(metadata, libraw_overrides);
  {
    const hiraco::ScopedTimingLog timer("preview", "Render LibRaw preview");
    if (!RenderLibRawImage(source_path, metadata, preview_settings, &rendered, error_message)) {
      return false;
    }
  }

  if (CheckCancelled(cancel, error_message)) {
    return false;
  }

  *preview = BuildPreviewImage(rendered, 0);
  ApplyLibRawOrientationToPreview(metadata.libraw_flip, preview.get());
  ReportProgress(progress, "preview", 1.0, "Original preview ready");
  return true;
}

bool EstimatePreviewAutoBrightGainFromRaw(const std::string& source_path,
                                          const SourceLinearDngMetadata& metadata,
                                          const LibRawOverrideSet& libraw_overrides,
                                          double* gain,
                                          std::string* error_message) {
  const hiraco::ScopedTimingLog gain_timer("preview", "Estimate preview auto-bright gain");
  if (gain == nullptr) {
    if (error_message != nullptr) {
      *error_message = "missing preview auto-bright gain output";
    }
    return false;
  }

  const RenderSettings preview_settings = BuildPreviewRenderSettings(metadata, libraw_overrides);
  if (preview_settings.no_auto_bright != 0) {
    *gain = 1.0;
    return true;
  }

  RasterImage linear_preview;
  const RenderSettings probe_settings = BuildLinearPreviewProbeSettings(metadata, libraw_overrides);
  {
    const hiraco::ScopedTimingLog timer("preview", "Render auto-bright probe");
    if (!RenderLibRawImage(source_path, metadata, probe_settings, &linear_preview, error_message)) {
      return false;
    }
  }

  if (linear_preview.width == 0 || linear_preview.height == 0 || linear_preview.colors < 3) {
    *gain = 1.0;
    return true;
  }

  std::vector<uint16_t> maxima(static_cast<size_t>(linear_preview.width) * linear_preview.height, 0);
  for (size_t index = 0; index < maxima.size(); ++index) {
    const size_t sample_index = index * linear_preview.colors;
    maxima[index] = std::max({linear_preview.pixels[sample_index + 0],
                              linear_preview.pixels[sample_index + 1],
                              linear_preview.pixels[sample_index + 2]});
  }

  const double clipped_fraction = 0.01;
  const double quantile = std::clamp(1.0 - clipped_fraction, 0.0, 1.0);
  const size_t quantile_index = std::min(maxima.size() - 1,
                                         static_cast<size_t>(quantile * static_cast<double>(maxima.size() - 1)));
  std::nth_element(maxima.begin(), maxima.begin() + quantile_index, maxima.end());
  const double quantile_value = std::max(static_cast<double>(maxima[quantile_index]), 1.0);

  *gain = std::clamp(65535.0 / quantile_value, 1.0, 64.0);
  return true;
}

bool BuildProcessingCacheFromRaw(const std::string& source_path,
                                 const SourceLinearDngMetadata& metadata,
                                 uint32_t source_width,
                                 uint32_t source_height,
                                 const CropRect& crop_rect,
                                 const LibRawOverrideSet& libraw_overrides,
                                 ProcessingCache* cache,
                                 ProgressCallback progress,
                                 CancelCheck cancel,
                                 std::string* error_message) {
  const hiraco::ScopedTimingLog cache_timer("cache", "Build processing cache");
  if (cache == nullptr) {
    if (error_message != nullptr) {
      *error_message = "missing processing cache";
    }
    return false;
  }

  cache->raw_image = RasterImage();
  cache->cfa_guide_image = RasterImage();
  cache->raw_image_is_camera_space = false;
  cache->preview_auto_bright_gain = 1.0;
  EstimatePreviewAutoBrightGainFromRaw(source_path, metadata, libraw_overrides, &cache->preview_auto_bright_gain, nullptr);
  cache->source_width = source_width;
  cache->source_height = source_height;
  cache->region_origin_x = 0;
  cache->region_origin_y = 0;
  cache->has_cached_crop = false;
  cache->cached_crop_rect = CropRect();

  ReportProgress(progress, "cache", 0.05, "Building crop preview cache");
  if (CheckCancelled(cancel, error_message)) {
    return false;
  }

  CropRect cache_region;
  if (source_width > 0 && source_height > 0) {
    cache->cached_crop_rect = ClampCropRectToBounds(crop_rect, source_width, source_height);
    cache_region = ExpandCropRectWithBorder(cache->cached_crop_rect,
                                            source_width,
                                            source_height,
                                            kCropPreviewProcessingBorder);
    cache->region_origin_x = cache_region.x;
    cache->region_origin_y = cache_region.y;
    cache->has_cached_crop = true;
  }

  if (IsOm3HighResMetadata(metadata)) {
    const CropRect* region = cache->has_cached_crop ? &cache_region : nullptr;
    const hiraco::ScopedTimingLog timer("cache", "Render cache region");
    if (!RenderOm3RawDomainImage(source_path, metadata, region, &cache->raw_image, error_message)) {
      return false;
    }
    cache->raw_image_is_camera_space = true;
  } else {
    const RenderSettings raw_settings = BuildRawRenderSettings(metadata, libraw_overrides);
    RasterImage rendered_image;
    {
      const hiraco::ScopedTimingLog timer("cache", "Render LibRaw cache source");
      if (!RenderLibRawImage(source_path, metadata, raw_settings, &rendered_image, error_message)) {
        return false;
      }
    }

    if (cache->has_cached_crop) {
      cache->raw_image = CropRasterImage(rendered_image, cache_region);
    } else {
      cache->raw_image = std::move(rendered_image);
    }
  }

  if (cache->source_width == 0 || cache->source_height == 0) {
    cache->source_width = cache->raw_image.width;
    cache->source_height = cache->raw_image.height;
  }

  if (IsCancelled(cancel)) {
    ReportProgress(progress, "cache", 1.0, "Crop preview cache ready");
    return true;
  }

  ReportProgress(progress, "cache", 1.0, "Crop preview cache ready");
  return true;
}

bool ApplyResolvedStageSettingsForTesting(const SourceLinearDngMetadata& metadata,
                                          const ResolvedStageSettings& settings,
                                          const RasterImage* cfa_guide_image,
                                          RasterImage* image,
                                          ProgressCallback progress,
                                          CancelCheck cancel,
                                          std::string* error_message) {
  return ApplyPredictedDetailGain(metadata,
                                  settings,
                                  cfa_guide_image,
                                  image,
                                  progress,
                                  cancel,
                                  error_message);
}

bool RenderConvertedCropPreview(const SourceLinearDngMetadata& metadata,
                                const ProcessingCache& cache,
                                const CropRect& crop_rect,
                                const StageOverrideSet& stage_overrides,
                                std::shared_ptr<PreviewImage> preview,
                                ProgressCallback progress,
                                CancelCheck cancel,
                                std::string* error_message) {
  const hiraco::ScopedTimingLog crop_timer("crop", "Render converted crop preview");
  if (!preview) {
    if (error_message != nullptr) {
      *error_message = "missing crop preview output";
    }
    return false;
  }

  if (cache.raw_image.width == 0 || cache.raw_image.height == 0) {
    if (error_message != nullptr) {
      *error_message = "processing cache is empty";
    }
    return false;
  }

  const uint32_t source_width = cache.source_width > 0 ? cache.source_width : cache.raw_image.width;
  const uint32_t source_height = cache.source_height > 0 ? cache.source_height : cache.raw_image.height;
  CropRect clamped_crop = ClampCropRectToBounds(crop_rect, source_width, source_height);

  RasterImage crop_image;
  RasterImage crop_guide;
  CropRect inner_crop;
  const RasterImage* guide_ptr = nullptr;

  if (cache.has_cached_crop) {
    const CropRect needed_region = ExpandCropRectWithBorder(clamped_crop,
                                                            source_width,
                                                            source_height,
                                                            kCropPreviewProcessingBorder);
    const uint64_t region_right = static_cast<uint64_t>(cache.region_origin_x) + cache.raw_image.width;
    const uint64_t region_bottom = static_cast<uint64_t>(cache.region_origin_y) + cache.raw_image.height;
    const uint64_t needed_right = static_cast<uint64_t>(needed_region.x) + needed_region.width;
    const uint64_t needed_bottom = static_cast<uint64_t>(needed_region.y) + needed_region.height;
    if (needed_region.x < cache.region_origin_x ||
        needed_region.y < cache.region_origin_y ||
        needed_right > region_right ||
        needed_bottom > region_bottom) {
      if (error_message != nullptr) {
        *error_message = "requested crop is outside cached preview region";
      }
      return false;
    }

    crop_image = cache.raw_image;
    if (cache.cfa_guide_image.width == cache.raw_image.width &&
        cache.cfa_guide_image.height == cache.raw_image.height &&
        cache.cfa_guide_image.colors == 4) {
      guide_ptr = &cache.cfa_guide_image;
    }

    inner_crop.x = clamped_crop.x - cache.region_origin_x;
    inner_crop.y = clamped_crop.y - cache.region_origin_y;
    inner_crop.width = clamped_crop.width;
    inner_crop.height = clamped_crop.height;
  } else {
    clamped_crop = ClampCropRectToBounds(crop_rect, cache.raw_image.width, cache.raw_image.height);
    crop_image = CropRasterImageWithBorder(cache.raw_image,
                                           clamped_crop,
                                           kCropPreviewProcessingBorder);
    if (cache.cfa_guide_image.width == cache.raw_image.width &&
        cache.cfa_guide_image.height == cache.raw_image.height &&
        cache.cfa_guide_image.colors == 4) {
      crop_guide = CropRasterImageWithBorder(cache.cfa_guide_image,
                                             clamped_crop,
                                             kCropPreviewProcessingBorder);
      guide_ptr = &crop_guide;
    }

    inner_crop.x = kCropPreviewProcessingBorder;
    inner_crop.y = kCropPreviewProcessingBorder;
    inner_crop.width = clamped_crop.width;
    inner_crop.height = clamped_crop.height;
  }

  const ResolvedStageSettings settings =
      ResolveStageSettingsForImage(metadata, source_width, source_height, stage_overrides);

  ReportProgress(progress, "crop", 0.05, "Processing converted crop");

  if (!cache.raw_image_is_camera_space) {
    if (!ApplyPredictedDetailGain(metadata,
                                  settings,
                                  guide_ptr,
                                  &crop_image,
                                  progress,
                                  cancel,
                                  error_message)) {
      return false;
    }
    ApplyLinearDngRasterTransform(metadata, &crop_image);
  } else {
    const double pedestal = metadata.has_black_level ? metadata.black_level : 0.0;
    if (pedestal > 0.0) {
      for (uint16_t& sample : crop_image.pixels) {
        const double value = static_cast<double>(sample) - pedestal;
        sample = static_cast<uint16_t>(std::clamp(value, 0.0, 65535.0));
      }
    }

    if (!ApplyPredictedDetailGain(metadata,
                                  settings,
                                  guide_ptr,
                                  &crop_image,
                                  progress,
                                  cancel,
                                  error_message)) {
      return false;
    }

    if (pedestal > 0.0) {
      for (uint16_t& sample : crop_image.pixels) {
        const double value = static_cast<double>(sample) + pedestal;
        sample = static_cast<uint16_t>(std::clamp(value, 0.0, 65535.0));
      }
    }
  }

  if (CheckCancelled(cancel, error_message)) {
    return false;
  }

  RasterImage final_crop = CropRasterImage(crop_image, inner_crop);

  *preview = BuildDisplayPreviewFromRaster(metadata,
                                          final_crop,
                                          cache.raw_image_is_camera_space,
                                          0,
                                          cache.preview_auto_bright_gain);
  ReportProgress(progress, "crop", 1.0, "Converted crop ready");
  return true;
}

DngWriterRuntimeSummary BuildDngWriterRuntimeSummary(const std::string& compression) {
  DngWriterRuntimeSummary summary;
  summary.enabled = true;

  try {
    dng_host host;
    summary.host_created = true;

    ConfigureHost(host, compression);

    AutoPtr<dng_negative> negative(host.Make_dng_negative());
    summary.negative_created = negative.Get() != nullptr;
    summary.status = summary.negative_created
        ? "Adobe runtime initialized host and dng_negative successfully"
        : "Adobe runtime created host but returned a null dng_negative";
  } catch (const dng_exception& exc) {
    summary.status = exc.what();
  } catch (const std::exception& exc) {
    summary.status = exc.what();
  }

  return summary;
}

DngWriteResult WriteLinearDngFromRaw(const std::string& source_path,
                                     const std::string& output_path,
                                     const std::string& compression,
                                     const SourceLinearDngMetadata& metadata,
                                     const StageOverrideSet& stage_overrides,
                                     const LibRawOverrideSet& libraw_overrides,
                                     std::shared_ptr<const PreviewImage> preview_override,
                                     double preview_auto_bright_gain,
                                     ProgressCallback progress,
                                     CancelCheck cancel) {
  const hiraco::ScopedTimingLog convert_timer("convert", "Write linear DNG from raw");
  DngWriteResult result;

  if (!IsSupportedWriteCompression(compression)) {
    result.message = UnsupportedCompressionMessage(compression);
    return result;
  }

  try {
    ReportProgress(progress, "convert", 0.05, "Building source payload");
    if (IsCancelled(cancel)) {
      result.message = "operation canceled";
      return result;
    }

    PreviewImage preview_image;
    if (preview_override && preview_override->width > 0 && preview_override->height > 0) {
      const hiraco::ScopedTimingLog timer("convert", "Scale cached embedded preview");
      preview_image = BuildPreviewImage(*preview_override, 1024);
      ApplyLibRawOrientationToPreview(InverseLibRawFlip(metadata.libraw_flip), &preview_image);
    } else {
      auto embedded_preview = std::make_shared<PreviewImage>();
      auto preview_progress = [&progress](const ProcessingProgress& update) {
        if (!progress) {
          return;
        }
        if (update.phase != "preview") {
          progress(update);
          return;
        }

        ProcessingProgress mapped = update;
        mapped.phase = "convert";
        mapped.fraction = 0.05 + 0.15 * std::clamp(update.fraction, 0.0, 1.0);
        mapped.message = update.message == "Original preview ready"
            ? "Embedded preview ready"
            : "Rendering embedded preview";
        progress(mapped);
      };
      {
        const hiraco::ScopedTimingLog timer("convert", "Render embedded preview");
        if (!BuildOriginalPreviewFromRaw(source_path,
                                         metadata,
                                         libraw_overrides,
                                         embedded_preview,
                                         preview_progress,
                                         cancel,
                                         &result.message)) {
          return result;
        }
      }
      preview_image = BuildPreviewImage(*embedded_preview, 1024);
      ApplyLibRawOrientationToPreview(InverseLibRawFlip(metadata.libraw_flip), &preview_image);
    }

    LinearDngPayload payload;
    std::string render_error;
    {
      const hiraco::ScopedTimingLog timer("convert", "Prepare source payload");
      if (!BuildLinearDngPayload(source_path,
                                 metadata,
                                 libraw_overrides,
                                 &payload,
                                 &render_error)) {
        result.message = render_error;
        return result;
      }
    }

    const ResolvedStageSettings resolved_settings =
        ResolveStageSettingsForImage(metadata, payload.raw_image.width, payload.raw_image.height, stage_overrides);

    if (!payload.raw_image_is_camera_space) {
      if (!ApplyPredictedDetailGain(metadata,
                                    resolved_settings,
                                    &payload.cfa_guide_image,
                                    &payload.raw_image,
                                    progress,
                                    cancel,
                                    &result.message)) {
        return result;
      }
      ApplyLinearDngRasterTransform(metadata, &payload.raw_image);
    } else {
      const double pedestal = metadata.has_black_level ? metadata.black_level : 0.0;
      const size_t pcount = static_cast<size_t>(payload.raw_image.width) * payload.raw_image.height;
      if (pedestal > 0.0) {
        for (size_t i = 0; i < pcount * payload.raw_image.colors; ++i) {
          double val = static_cast<double>(payload.raw_image.pixels[i]) - pedestal;
          payload.raw_image.pixels[i] = static_cast<uint16_t>(std::clamp(val, 0.0, 65535.0));
        }
      }
      
      if (!ApplyPredictedDetailGain(metadata,
                                    resolved_settings,
                                    &payload.cfa_guide_image,
                                    &payload.raw_image,
                                    progress,
                                    cancel,
                                    &result.message)) {
        return result;
      }
      
      if (pedestal > 0.0) {
        for (size_t i = 0; i < pcount * payload.raw_image.colors; ++i) {
          double val = static_cast<double>(payload.raw_image.pixels[i]) + pedestal;
          payload.raw_image.pixels[i] = static_cast<uint16_t>(std::clamp(val, 0.0, 65535.0));
        }
      }
    }

    CallbackAbortSniffer sniffer(progress, cancel);
    dng_host host(nullptr, &sniffer);
    ConfigureHost(host, compression);

    AutoPtr<dng_negative> negative(host.Make_dng_negative());
    PopulateLinearRawNegative(host, source_path, payload.raw_image, metadata, *negative.Get(), preview_auto_bright_gain);

    dng_preview_list preview_list;
    AppendImagePreview(host, preview_image, &preview_list);

    const std::filesystem::path output_fs_path(output_path);
    if (!output_fs_path.parent_path().empty()) {
      std::filesystem::create_directories(output_fs_path.parent_path());
    }

    dng_file_stream stream(output_path.c_str(), true);
    dng_image_writer writer;
    if (compression == "jpeg-xl") {
      negative->LosslessCompressJXL(host, writer, false);
    }

    ReportProgress(progress, "convert", 0.92, "Writing DNG");
    {
      const hiraco::ScopedTimingLog timer("convert", "Write DNG stream");
      writer.WriteDNG(host,
                      stream,
                      *negative.Get(),
                      &preview_list,
                      DngVersionForCompression(compression),
                      compression == "uncompressed");
      stream.Flush();
    }

    result.ok = true;
    result.message = "native linear DNG write succeeded";
    ReportProgress(progress, "convert", 1.0, "Conversion complete");
  } catch (const dng_exception& exc) {
    result.message = exc.what();
  } catch (const std::exception& exc) {
    result.message = exc.what();
  }

  return result;
}

DngWriteResult WriteSyntheticLinearDng(const std::string& output_path,
                                       const std::string& compression) {
  DngWriteResult result;

  if (!IsSupportedWriteCompression(compression)) {
    result.message = UnsupportedCompressionMessage(compression);
    return result;
  }

  try {
    RasterImage processed = MakeSyntheticRgbImage();
    PreviewImage preview_image = BuildPreviewImage(processed, 256);

    dng_host host;
    ConfigureHost(host, compression);

    AutoPtr<dng_negative> negative(host.Make_dng_negative());
    PopulateLinearRawNegative(host, "synthetic-gradient.raw", processed, SourceLinearDngMetadata(), *negative.Get(), 1.0);

    dng_preview_list preview_list;
    AppendImagePreview(host, preview_image, &preview_list);

    dng_file_stream stream(output_path.c_str(), true);
    dng_image_writer writer;
    if (compression == "jpeg-xl") {
      negative->LosslessCompressJXL(host, writer, false);
    }

    writer.WriteDNG(host,
                    stream,
                    *negative.Get(),
                    &preview_list,
                    DngVersionForCompression(compression),
                    compression == "uncompressed");
    stream.Flush();

    result.ok = true;
    result.message = "synthetic linear DNG write succeeded";
  } catch (const dng_exception& exc) {
    result.message = exc.what();
  } catch (const std::exception& exc) {
    result.message = exc.what();
  }

  return result;
}

#else

DngWriterRuntimeSummary BuildDngWriterRuntimeSummary(const std::string&) {
  DngWriterRuntimeSummary summary;
  summary.enabled = false;
  summary.status = "Adobe DNG SDK integration not enabled in this build";
  return summary;
}

ResolvedStageSettings ResolveStageSettingsForImage(const SourceLinearDngMetadata&,
                                                   uint32_t,
                                                   uint32_t,
                                                   const StageOverrideSet& overrides) {
  ResolvedStageSettings settings;
  if (overrides.stage1_psf_sigma.has_value()) settings.stage1_psf_sigma = *overrides.stage1_psf_sigma;
  if (overrides.stage1_nsr.has_value()) settings.stage1_nsr = *overrides.stage1_nsr;
  if (overrides.stage2_denoise.has_value()) settings.stage2_denoise = *overrides.stage2_denoise;
  if (overrides.stage2_gain1.has_value()) settings.stage2_gain1 = *overrides.stage2_gain1;
  if (overrides.stage2_gain2.has_value()) settings.stage2_gain2 = *overrides.stage2_gain2;
  if (overrides.stage2_gain3.has_value()) settings.stage2_gain3 = *overrides.stage2_gain3;
  if (overrides.stage3_radius.has_value()) settings.stage3_radius = *overrides.stage3_radius;
  if (overrides.stage3_gain.has_value()) settings.stage3_gain = *overrides.stage3_gain;
  FinalizeStage2Settings(&settings);
  return settings;
}

bool BuildOriginalPreviewFromRaw(const std::string&,
                                 const SourceLinearDngMetadata&,
                                 const LibRawOverrideSet&,
                                 std::shared_ptr<PreviewImage>,
                                 ProgressCallback,
                                 CancelCheck,
                                 std::string* error_message) {
  if (error_message != nullptr) {
    *error_message = "Adobe DNG SDK integration not enabled in this build";
  }
  return false;
}

bool EstimatePreviewAutoBrightGainFromRaw(const std::string&,
                                          const SourceLinearDngMetadata&,
                                          const LibRawOverrideSet&,
                                          double*,
                                          std::string* error_message) {
  if (error_message != nullptr) {
    *error_message = "Adobe DNG SDK integration not enabled in this build";
  }
  return false;
}

bool BuildProcessingCacheFromRaw(const std::string&,
                                 const SourceLinearDngMetadata&,
                                 uint32_t,
                                 uint32_t,
                                 const CropRect&,
                                 const LibRawOverrideSet&,
                                 ProcessingCache*,
                                 ProgressCallback,
                                 CancelCheck,
                                 std::string* error_message) {
  if (error_message != nullptr) {
    *error_message = "Adobe DNG SDK integration not enabled in this build";
  }
  return false;
}

bool RenderConvertedCropPreview(const SourceLinearDngMetadata&,
                                const ProcessingCache&,
                                const CropRect&,
                                const StageOverrideSet&,
                                std::shared_ptr<PreviewImage>,
                                ProgressCallback,
                                CancelCheck,
                                std::string* error_message) {
  if (error_message != nullptr) {
    *error_message = "Adobe DNG SDK integration not enabled in this build";
  }
  return false;
}

bool ApplyResolvedStageSettingsForTesting(const SourceLinearDngMetadata&,
                                          const ResolvedStageSettings&,
                                          const RasterImage*,
                                          RasterImage*,
                                          ProgressCallback,
                                          CancelCheck,
                                          std::string* error_message) {
  if (error_message != nullptr) {
    *error_message = "Adobe DNG SDK integration not enabled in this build";
  }
  return false;
}

DngWriteResult WriteLinearDngFromRaw(const std::string&,
                                     const std::string&,
                                     const std::string&,
                                     const SourceLinearDngMetadata&,
                                     const StageOverrideSet&,
                                     const LibRawOverrideSet&,
                                     std::shared_ptr<const PreviewImage>,
                                     double,
                                     ProgressCallback,
                                     CancelCheck) {
  DngWriteResult result;
  result.message = "Adobe DNG SDK integration not enabled in this build";
  return result;
}

DngWriteResult WriteSyntheticLinearDng(const std::string&, const std::string&) {
  DngWriteResult result;
  result.message = "Adobe DNG SDK integration not enabled in this build";
  return result;
}

#endif
