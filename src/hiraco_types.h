#pragma once

#include <cstdint>
#include <functional>
#include <optional>
#include <string>
#include <vector>

struct DngWriteResult {
  bool ok = false;
  std::string message;
};

struct SourceLinearDngMetadata {
  std::string make;
  std::string model;
  std::string unique_camera_model;
  int libraw_flip = 0;
  bool has_black_level = false;
  double black_level = 0.0;
  bool has_white_level = false;
  double white_level = 0.0;
  bool has_as_shot_neutral = false;
  double as_shot_neutral[3] = {1.0, 1.0, 1.0};
  bool has_color_matrix1 = false;
  double color_matrix1[9] = {0.0, 0.0, 0.0, 0.0, 0.0, 0.0, 0.0, 0.0, 0.0};
  bool has_rgb_cam = false;
  double rgb_cam[9] = {0.0, 0.0, 0.0, 0.0, 0.0, 0.0, 0.0, 0.0, 0.0};
  bool has_predicted_detail_gain = false;
  double predicted_detail_gain = 1.0;
  bool has_stack_stability_map = false;
  std::string stack_stability_path;
  uint32_t stack_stability_width = 0;
  uint32_t stack_stability_height = 0;
  std::vector<float> stack_stability_data;
  bool has_stack_mean_map = false;
  std::string stack_mean_path;
  uint32_t stack_mean_width = 0;
  uint32_t stack_mean_height = 0;
  std::vector<float> stack_mean_data;
  bool has_stack_guide_map = false;
  std::string stack_guide_path;
  uint32_t stack_guide_width = 0;
  uint32_t stack_guide_height = 0;
  std::vector<float> stack_guide_data;
  bool has_stack_tensor_x_map = false;
  std::string stack_tensor_x_path;
  uint32_t stack_tensor_x_width = 0;
  uint32_t stack_tensor_x_height = 0;
  std::vector<float> stack_tensor_x_data;
  bool has_stack_tensor_y_map = false;
  std::string stack_tensor_y_path;
  uint32_t stack_tensor_y_width = 0;
  uint32_t stack_tensor_y_height = 0;
  std::vector<float> stack_tensor_y_data;
  bool has_stack_tensor_coherence_map = false;
  std::string stack_tensor_coherence_path;
  uint32_t stack_tensor_coherence_width = 0;
  uint32_t stack_tensor_coherence_height = 0;
  std::vector<float> stack_tensor_coherence_data;
  bool has_stack_alias_map = false;
  std::string stack_alias_path;
  uint32_t stack_alias_width = 0;
  uint32_t stack_alias_height = 0;
  std::vector<float> stack_alias_data;
  bool has_default_crop = false;
  uint32_t default_crop_origin_h = 0;
  uint32_t default_crop_origin_v = 0;
  uint32_t default_crop_width = 0;
  uint32_t default_crop_height = 0;
  bool has_working_geometry = false;
  uint32_t working_width = 0;
  uint32_t working_height = 0;
};

struct StageOverrideSet {
  std::optional<float> stage1_psf_sigma;
  std::optional<float> stage1_nsr;
  std::optional<float> stage2_denoise;
  std::optional<float> stage2_gain0;
  std::optional<float> stage2_gain1;
  std::optional<float> stage2_gain2;
  std::optional<float> stage2_gain3;
  std::optional<int> stage3_radius;
  std::optional<float> stage3_gain;

  bool HasAnyOverrides() const {
    return stage1_psf_sigma.has_value() ||
           stage1_nsr.has_value() ||
           stage2_denoise.has_value() ||
           stage2_gain0.has_value() ||
           stage2_gain1.has_value() ||
           stage2_gain2.has_value() ||
           stage2_gain3.has_value() ||
           stage3_radius.has_value() ||
           stage3_gain.has_value();
  }
};

struct LibRawOverrideSet {
  std::optional<int> user_qual;
  std::optional<int> four_color_rgb;
  std::optional<int> green_matching;
  std::optional<int> med_passes;
  std::optional<int> no_auto_scale;
  std::optional<int> no_auto_bright;
  std::optional<float> adjust_maximum_thr;

  bool HasAnyOverrides() const {
    return user_qual.has_value() ||
           four_color_rgb.has_value() ||
           green_matching.has_value() ||
           med_passes.has_value() ||
           no_auto_scale.has_value() ||
           no_auto_bright.has_value() ||
           adjust_maximum_thr.has_value();
  }
};

struct ResolvedStageSettings {
  float stage1_psf_sigma = 2.0f;
  float stage1_nsr = 0.09f;
  float stage2_denoise = 1.0f;
  float stage2_gain0 = 2.0f;
  float stage2_gain1 = 1.4f;
  float stage2_gain2 = 1.3f;
  float stage2_gain3 = 1.1f;
  int stage3_radius = 2;
  float stage3_gain = 2.0f;
};

struct RasterImage {
  uint32_t width = 0;
  uint32_t height = 0;
  uint32_t colors = 0;
  uint32_t bits = 0;
  std::vector<uint16_t> pixels;
};

struct PreviewImage {
  uint32_t width = 0;
  uint32_t height = 0;
  uint32_t colors = 0;
  uint32_t bits = 8;
  std::vector<uint8_t> pixels;
};

struct CropRect {
  uint32_t x = 0;
  uint32_t y = 0;
  uint32_t width = 0;
  uint32_t height = 0;
};

inline int NormalizeLibRawFlip(int flip) {
  const int normalized = flip % 8;
  return normalized < 0 ? normalized + 8 : normalized;
}

inline bool LibRawFlipSwapsAxes(int flip) {
  return (NormalizeLibRawFlip(flip) & 4) != 0;
}

inline uint32_t OrientedImageWidth(uint32_t width, uint32_t height, int flip) {
  return LibRawFlipSwapsAxes(flip) ? height : width;
}

inline uint32_t OrientedImageHeight(uint32_t width, uint32_t height, int flip) {
  return LibRawFlipSwapsAxes(flip) ? width : height;
}

inline uint32_t LibRawFlipToTiffOrientation(int flip) {
  switch (NormalizeLibRawFlip(flip)) {
    case 0:
      return 1;
    case 1:
      return 2;
    case 2:
      return 4;
    case 3:
      return 3;
    case 4:
      return 5;
    case 5:
      return 8;
    case 6:
      return 6;
    case 7:
      return 7;
    default:
      return 1;
  }
}

inline constexpr uint32_t kCropPreviewProcessingBorder = 96;

struct ProcessingProgress {
  std::string phase;
  double fraction = 0.0;
  std::string message;
};

using ProgressCallback = std::function<void(const ProcessingProgress&)>;
using CancelCheck = std::function<bool()>;

struct ProcessingCache {
  RasterImage raw_image;
  RasterImage cfa_guide_image;
  bool raw_image_is_camera_space = false;
  double preview_auto_bright_gain = 1.0;
  uint32_t source_width = 0;
  uint32_t source_height = 0;
  uint32_t region_origin_x = 0;
  uint32_t region_origin_y = 0;
  bool has_cached_crop = false;
  CropRect cached_crop_rect;
};
