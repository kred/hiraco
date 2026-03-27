#pragma once

#include <cstdint>
#include <string>

struct DngWriterRuntimeSummary {
  bool enabled = false;
  bool host_created = false;
  bool negative_created = false;
  std::string status;
};

DngWriterRuntimeSummary BuildDngWriterRuntimeSummary(const std::string& compression);

struct DngWriteResult {
  bool ok = false;
  std::string message;
};

struct SourceLinearDngMetadata {
  std::string make;
  std::string model;
  std::string unique_camera_model;
  bool has_black_level = false;
  double black_level = 0.0;
  bool has_white_level = false;
  double white_level = 0.0;
  bool has_as_shot_neutral = false;
  double as_shot_neutral[3] = {1.0, 1.0, 1.0};
  bool has_color_matrix1 = false;
  double color_matrix1[9] = {0.0, 0.0, 0.0, 0.0, 0.0, 0.0, 0.0, 0.0, 0.0};
  bool has_predicted_detail_gain = false;
  double predicted_detail_gain = 1.0;
  bool has_stack_stability_map = false;
  std::string stack_stability_path;
  uint32_t stack_stability_width = 0;
  uint32_t stack_stability_height = 0;
  bool has_stack_mean_map = false;
  std::string stack_mean_path;
  uint32_t stack_mean_width = 0;
  uint32_t stack_mean_height = 0;
  bool has_stack_guide_map = false;
  std::string stack_guide_path;
  uint32_t stack_guide_width = 0;
  uint32_t stack_guide_height = 0;
  bool has_stack_tensor_x_map = false;
  std::string stack_tensor_x_path;
  uint32_t stack_tensor_x_width = 0;
  uint32_t stack_tensor_x_height = 0;
  bool has_stack_tensor_y_map = false;
  std::string stack_tensor_y_path;
  uint32_t stack_tensor_y_width = 0;
  uint32_t stack_tensor_y_height = 0;
  bool has_stack_tensor_coherence_map = false;
  std::string stack_tensor_coherence_path;
  uint32_t stack_tensor_coherence_width = 0;
  uint32_t stack_tensor_coherence_height = 0;
  bool has_stack_alias_map = false;
  std::string stack_alias_path;
  uint32_t stack_alias_width = 0;
  uint32_t stack_alias_height = 0;
  bool has_default_crop = false;
  uint32_t default_crop_origin_h = 0;
  uint32_t default_crop_origin_v = 0;
  uint32_t default_crop_width = 0;
  uint32_t default_crop_height = 0;
  bool has_working_geometry = false;
  uint32_t working_width = 0;
  uint32_t working_height = 0;
};

DngWriteResult WriteLinearDngFromRaw(const std::string& source_path,
                                     const std::string& output_path,
                                     const std::string& compression,
                                     const SourceLinearDngMetadata& metadata);

DngWriteResult WriteSyntheticLinearDng(const std::string& output_path,
                                       const std::string& compression);
