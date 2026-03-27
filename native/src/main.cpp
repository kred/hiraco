#include "dng_sdk_bridge.h"
#include "dng_writer_bridge.h"

#include <filesystem>
#include <fstream>
#include <iostream>
#include <libraw/libraw.h>
#include <regex>
#include <set>
#include <sstream>
#include <string>
#include <vector>

namespace {

struct DecodeSummary {
  std::string camera_make;
  std::string camera_model;
  std::string normalized_make;
  std::string normalized_model;
  std::string color_description;
  std::string decoder_name;
  std::string libraw_version;
  std::vector<std::string> runtime_capabilities;
  std::vector<std::string> process_warning_names;
  unsigned raw_count = 0;
  unsigned raw_width = 0;
  unsigned raw_height = 0;
  unsigned image_width = 0;
  unsigned image_height = 0;
  unsigned top_margin = 0;
  unsigned left_margin = 0;
  int flip = 0;
  unsigned filters = 0;
  int colors = 0;
  unsigned process_warnings = 0;
  unsigned process_warnings_before_unpack = 0;
  unsigned process_warnings_after_unpack = 0;
  unsigned decoder_flags = 0;
  unsigned black_level = 0;
  unsigned white_level = 0;
  unsigned linear_maximum_0 = 0;
  unsigned linear_maximum_1 = 0;
  unsigned linear_maximum_2 = 0;
  unsigned linear_maximum_3 = 0;
  unsigned inset_crop_0_left = 0;
  unsigned inset_crop_0_top = 0;
  unsigned inset_crop_0_width = 0;
  unsigned inset_crop_0_height = 0;
  unsigned inset_crop_1_left = 0;
  unsigned inset_crop_1_top = 0;
  unsigned inset_crop_1_width = 0;
  unsigned inset_crop_1_height = 0;
  unsigned inset_crop_applied_index = 0;
  int as_shot_wb_applied = 0;
  int raw_use_rawspeed = 0;
  int raw_use_dngsdk = 0;
  unsigned raw_processing_options = 0;
};

struct AnalysisSummary {
  bool wrote_mosaic = false;
  bool wrote_cfa_index = false;
  bool wrote_crop_geometry = false;
  std::string mosaic_path;
  std::string cfa_index_path;
  std::string crop_geometry_path;
};

std::string BuildDecodeSummaryJson(const DecodeSummary& summary);
std::string BuildDngSdkSummaryJson(const DngSdkSupportSummary& summary);
std::string BuildDngWriterConfigJson(const DngWriterConfigSummary& summary);
std::string BuildDngWriterRuntimeJson(const DngWriterRuntimeSummary& summary);
std::string BuildJsonStringArray(const std::vector<std::string>& values);
std::string BuildAnalysisSummaryJson(const AnalysisSummary& summary);

std::vector<std::string> CollectLibRawRuntimeCapabilities() {
  std::vector<std::string> capabilities;
  const unsigned raw_capabilities = libraw_capabilities();
  if ((raw_capabilities & LIBRAW_CAPS_RAWSPEED) != 0u) {
    capabilities.emplace_back("rawspeed");
  }
  if ((raw_capabilities & LIBRAW_CAPS_DNGSDK) != 0u) {
    capabilities.emplace_back("dngsdk");
  }
  if ((raw_capabilities & LIBRAW_CAPS_GPRSDK) != 0u) {
    capabilities.emplace_back("gprsdk");
  }
  if ((raw_capabilities & LIBRAW_CAPS_UNICODEPATHS) != 0u) {
    capabilities.emplace_back("unicode-paths");
  }
  if ((raw_capabilities & LIBRAW_CAPS_X3FTOOLS) != 0u) {
    capabilities.emplace_back("x3ftools");
  }
  if ((raw_capabilities & LIBRAW_CAPS_RPI6BY9) != 0u) {
    capabilities.emplace_back("rpi6by9");
  }
  if ((raw_capabilities & LIBRAW_CAPS_ZLIB) != 0u) {
    capabilities.emplace_back("zlib");
  }
  if ((raw_capabilities & LIBRAW_CAPS_JPEG) != 0u) {
    capabilities.emplace_back("jpeg");
  }
  if ((raw_capabilities & LIBRAW_CAPS_RAWSPEED3) != 0u) {
    capabilities.emplace_back("rawspeed3");
  }
  if ((raw_capabilities & LIBRAW_CAPS_RAWSPEED_BITS) != 0u) {
    capabilities.emplace_back("rawspeed-bits");
  }
  return capabilities;
}

std::vector<std::string> DecodeProcessWarnings(unsigned process_warnings) {
  std::vector<std::string> warnings;
  struct WarningEntry {
    unsigned bit;
    const char* name;
  };
  static const WarningEntry kWarnings[] = {
      {LIBRAW_WARN_BAD_CAMERA_WB, "bad-camera-wb"},
      {LIBRAW_WARN_NO_METADATA, "no-metadata"},
      {LIBRAW_WARN_NO_JPEGLIB, "no-jpeglib"},
      {LIBRAW_WARN_NO_EMBEDDED_PROFILE, "no-embedded-profile"},
      {LIBRAW_WARN_NO_INPUT_PROFILE, "no-input-profile"},
      {LIBRAW_WARN_BAD_OUTPUT_PROFILE, "bad-output-profile"},
      {LIBRAW_WARN_NO_BADPIXELMAP, "no-badpixelmap"},
      {LIBRAW_WARN_BAD_DARKFRAME_FILE, "bad-darkframe-file"},
      {LIBRAW_WARN_BAD_DARKFRAME_DIM, "bad-darkframe-dim"},
      {LIBRAW_WARN_RAWSPEED_PROBLEM, "rawspeed-problem"},
      {LIBRAW_WARN_RAWSPEED_UNSUPPORTED, "rawspeed-unsupported"},
      {LIBRAW_WARN_RAWSPEED_PROCESSED, "rawspeed-processed"},
      {LIBRAW_WARN_FALLBACK_TO_AHD, "fallback-to-ahd"},
      {LIBRAW_WARN_PARSEFUJI_PROCESSED, "parsefuji-processed"},
      {LIBRAW_WARN_DNGSDK_PROCESSED, "dngsdk-processed"},
      {LIBRAW_WARN_DNG_IMAGES_REORDERED, "dng-images-reordered"},
      {LIBRAW_WARN_DNG_STAGE2_APPLIED, "dng-stage2-applied"},
      {LIBRAW_WARN_DNG_STAGE3_APPLIED, "dng-stage3-applied"},
      {LIBRAW_WARN_RAWSPEED3_PROBLEM, "rawspeed3-problem"},
      {LIBRAW_WARN_RAWSPEED3_UNSUPPORTED, "rawspeed3-unsupported"},
      {LIBRAW_WARN_RAWSPEED3_PROCESSED, "rawspeed3-processed"},
      {LIBRAW_WARN_RAWSPEED3_NOTLISTED, "rawspeed3-notlisted"},
      {LIBRAW_WARN_VENDOR_CROP_SUGGESTED, "vendor-crop-suggested"},
      {LIBRAW_WARN_DNG_NOT_PROCESSED, "dng-not-processed"},
      {LIBRAW_WARN_DNG_NOT_PARSED, "dng-not-parsed"},
  };

  for (const WarningEntry& entry : kWarnings) {
    if ((process_warnings & entry.bit) != 0u) {
      warnings.emplace_back(entry.name);
    }
  }
  return warnings;
}

std::string JsonEscape(const std::string& value) {
  std::string escaped;
  escaped.reserve(value.size());
  for (char ch : value) {
    switch (ch) {
      case '\\':
        escaped += "\\\\";
        break;
      case '"':
        escaped += "\\\"";
        break;
      case '\n':
        escaped += "\\n";
        break;
      default:
        escaped += ch;
        break;
    }
  }
  return escaped;
}

std::string JsonEscape(unsigned value) {
  return std::to_string(value);
}

std::string JsonEscape(int value) {
  return std::to_string(value);
}

std::string BuildJsonStringArray(const std::vector<std::string>& values) {
  std::ostringstream output;
  output << "[";
  for (size_t index = 0; index < values.size(); ++index) {
    if (index != 0) {
      output << ", ";
    }
    output << '"' << JsonEscape(values[index]) << '"';
  }
  output << "]";
  return output.str();
}

std::string FindJsonString(const std::string& json, const std::string& key) {
  const std::regex pattern("\"" + key + "\"\\s*:\\s*\"([^\"]*)\"");
  std::smatch match;
  if (!std::regex_search(json, match, pattern)) {
    return {};
  }
  return match[1].str();
}

bool FindJsonBool(const std::string& json, const std::string& key, bool default_value) {
  const std::regex pattern("\"" + key + "\"\\s*:\\s*(true|false)");
  std::smatch match;
  if (!std::regex_search(json, match, pattern)) {
    return default_value;
  }
  return match[1].str() == "true";
}

bool FindJsonUnsigned(const std::string& json, const std::string& key, unsigned* value) {
  const std::regex pattern("\"" + key + "\"\\s*:\\s*([0-9]+)");
  std::smatch match;
  if (!std::regex_search(json, match, pattern)) {
    return false;
  }
  *value = static_cast<unsigned>(std::stoul(match[1].str()));
  return true;
}

bool FindJsonDouble(const std::string& json, const std::string& key, double* value) {
  const std::regex pattern("\"" + key + "\"\\s*:\\s*(-?[0-9]+(?:\\.[0-9]+)?)");
  std::smatch match;
  if (!std::regex_search(json, match, pattern)) {
    return false;
  }
  *value = std::stod(match[1].str());
  return true;
}

SourceLinearDngMetadata ParseSourceLinearDngMetadata(const std::string& request_json) {
  SourceLinearDngMetadata metadata;
  metadata.make = FindJsonString(request_json, "make");
  metadata.model = FindJsonString(request_json, "model");
  metadata.unique_camera_model = FindJsonString(request_json, "unique_camera_model");

  metadata.has_black_level = FindJsonDouble(request_json, "linear_dng_black_level", &metadata.black_level);
  metadata.has_white_level = FindJsonDouble(request_json, "linear_dng_white_level", &metadata.white_level);

  metadata.has_as_shot_neutral =
      FindJsonDouble(request_json, "linear_dng_as_shot_neutral_0", &metadata.as_shot_neutral[0]) &&
      FindJsonDouble(request_json, "linear_dng_as_shot_neutral_1", &metadata.as_shot_neutral[1]) &&
      FindJsonDouble(request_json, "linear_dng_as_shot_neutral_2", &metadata.as_shot_neutral[2]);

  metadata.has_color_matrix1 =
      FindJsonDouble(request_json, "linear_dng_color_matrix_00", &metadata.color_matrix1[0]) &&
      FindJsonDouble(request_json, "linear_dng_color_matrix_01", &metadata.color_matrix1[1]) &&
      FindJsonDouble(request_json, "linear_dng_color_matrix_02", &metadata.color_matrix1[2]) &&
      FindJsonDouble(request_json, "linear_dng_color_matrix_10", &metadata.color_matrix1[3]) &&
      FindJsonDouble(request_json, "linear_dng_color_matrix_11", &metadata.color_matrix1[4]) &&
      FindJsonDouble(request_json, "linear_dng_color_matrix_12", &metadata.color_matrix1[5]) &&
      FindJsonDouble(request_json, "linear_dng_color_matrix_20", &metadata.color_matrix1[6]) &&
      FindJsonDouble(request_json, "linear_dng_color_matrix_21", &metadata.color_matrix1[7]) &&
      FindJsonDouble(request_json, "linear_dng_color_matrix_22", &metadata.color_matrix1[8]);

  metadata.has_predicted_detail_gain =
      FindJsonDouble(request_json, "predicted_detail_gain", &metadata.predicted_detail_gain);

  metadata.stack_stability_path = FindJsonString(request_json, "om3_stack_stability_path");
  unsigned stack_stability_width = 0;
  unsigned stack_stability_height = 0;
  metadata.has_stack_stability_map =
      !metadata.stack_stability_path.empty() &&
      FindJsonUnsigned(request_json, "om3_stack_stability_width", &stack_stability_width) &&
      FindJsonUnsigned(request_json, "om3_stack_stability_height", &stack_stability_height);
  metadata.stack_stability_width = stack_stability_width;
  metadata.stack_stability_height = stack_stability_height;

  metadata.stack_mean_path = FindJsonString(request_json, "om3_stack_mean_path");
  unsigned stack_mean_width = 0;
  unsigned stack_mean_height = 0;
  metadata.has_stack_mean_map =
      !metadata.stack_mean_path.empty() &&
      FindJsonUnsigned(request_json, "om3_stack_mean_width", &stack_mean_width) &&
      FindJsonUnsigned(request_json, "om3_stack_mean_height", &stack_mean_height);
  metadata.stack_mean_width = stack_mean_width;
  metadata.stack_mean_height = stack_mean_height;

  metadata.stack_guide_path = FindJsonString(request_json, "om3_stack_guide_path");
  unsigned stack_guide_width = 0;
  unsigned stack_guide_height = 0;
  metadata.has_stack_guide_map =
      !metadata.stack_guide_path.empty() &&
      FindJsonUnsigned(request_json, "om3_stack_guide_width", &stack_guide_width) &&
      FindJsonUnsigned(request_json, "om3_stack_guide_height", &stack_guide_height);
  metadata.stack_guide_width = stack_guide_width;
  metadata.stack_guide_height = stack_guide_height;

  metadata.stack_tensor_x_path = FindJsonString(request_json, "om3_stack_tensor_x_path");
  unsigned stack_tensor_x_width = 0;
  unsigned stack_tensor_x_height = 0;
  metadata.has_stack_tensor_x_map =
      !metadata.stack_tensor_x_path.empty() &&
      FindJsonUnsigned(request_json, "om3_stack_tensor_x_width", &stack_tensor_x_width) &&
      FindJsonUnsigned(request_json, "om3_stack_tensor_x_height", &stack_tensor_x_height);
  metadata.stack_tensor_x_width = stack_tensor_x_width;
  metadata.stack_tensor_x_height = stack_tensor_x_height;

  metadata.stack_tensor_y_path = FindJsonString(request_json, "om3_stack_tensor_y_path");
  unsigned stack_tensor_y_width = 0;
  unsigned stack_tensor_y_height = 0;
  metadata.has_stack_tensor_y_map =
      !metadata.stack_tensor_y_path.empty() &&
      FindJsonUnsigned(request_json, "om3_stack_tensor_y_width", &stack_tensor_y_width) &&
      FindJsonUnsigned(request_json, "om3_stack_tensor_y_height", &stack_tensor_y_height);
  metadata.stack_tensor_y_width = stack_tensor_y_width;
  metadata.stack_tensor_y_height = stack_tensor_y_height;

  metadata.stack_tensor_coherence_path = FindJsonString(request_json, "om3_stack_tensor_coherence_path");
  unsigned stack_tensor_coherence_width = 0;
  unsigned stack_tensor_coherence_height = 0;
  metadata.has_stack_tensor_coherence_map =
      !metadata.stack_tensor_coherence_path.empty() &&
      FindJsonUnsigned(request_json, "om3_stack_tensor_coherence_width", &stack_tensor_coherence_width) &&
      FindJsonUnsigned(request_json, "om3_stack_tensor_coherence_height", &stack_tensor_coherence_height);
  metadata.stack_tensor_coherence_width = stack_tensor_coherence_width;
  metadata.stack_tensor_coherence_height = stack_tensor_coherence_height;

  metadata.stack_alias_path = FindJsonString(request_json, "om3_stack_alias_path");
  unsigned stack_alias_width = 0;
  unsigned stack_alias_height = 0;
  metadata.has_stack_alias_map =
      !metadata.stack_alias_path.empty() &&
      FindJsonUnsigned(request_json, "om3_stack_alias_width", &stack_alias_width) &&
      FindJsonUnsigned(request_json, "om3_stack_alias_height", &stack_alias_height);
  metadata.stack_alias_width = stack_alias_width;
  metadata.stack_alias_height = stack_alias_height;

  unsigned crop_left = 0;
  unsigned crop_top = 0;
  unsigned crop_width = 0;
  unsigned crop_height = 0;
  metadata.has_default_crop =
      FindJsonUnsigned(request_json, "linear_dng_crop_left_margin", &crop_left) &&
      FindJsonUnsigned(request_json, "linear_dng_crop_top_margin", &crop_top) &&
      FindJsonUnsigned(request_json, "linear_dng_crop_width", &crop_width) &&
      FindJsonUnsigned(request_json, "linear_dng_crop_height", &crop_height);
  metadata.default_crop_origin_h = crop_left;
  metadata.default_crop_origin_v = crop_top;
  metadata.default_crop_width = crop_width;
  metadata.default_crop_height = crop_height;

  unsigned working_width = 0;
  unsigned working_height = 0;
  metadata.has_working_geometry =
      FindJsonUnsigned(request_json, "working_width", &working_width) &&
      FindJsonUnsigned(request_json, "working_height", &working_height);
  metadata.working_width = working_width;
  metadata.working_height = working_height;

  return metadata;
}

int PrintUsage() {
  std::cerr << "usage: hiraco-native convert --request-json <json>\n";
  std::cerr << "       hiraco-native analyze --request-json <json>\n";
  std::cerr << "       hiraco-native probe --source <path>\n";
  std::cerr << "       hiraco-native selftest-write --output <path> [--compression uncompressed|deflate|jpeg-xl]\n";
  return 2;
}

bool EnsureParentDirectoryExists(const std::string& output_path, std::string* error_message) {
  const std::filesystem::path path(output_path);
  const std::filesystem::path parent = path.parent_path();
  if (parent.empty()) {
    return true;
  }

  std::error_code error;
  if (std::filesystem::exists(parent, error)) {
    return true;
  }

  if (std::filesystem::create_directories(parent, error)) {
    return true;
  }

  if (std::filesystem::exists(parent, error)) {
    return true;
  }

  *error_message = std::string("failed to create parent directory for output: ") + output_path;
  return false;
}

bool WriteBinaryFile(const std::string& output_path,
                    const void* data,
                    size_t size_bytes,
                    std::string* error_message) {
  if (!EnsureParentDirectoryExists(output_path, error_message)) {
    return false;
  }

  std::ofstream stream(output_path, std::ios::binary | std::ios::trunc);
  if (!stream) {
    *error_message = std::string("failed to open output file for writing: ") + output_path;
    return false;
  }

  stream.write(reinterpret_cast<const char*>(data), static_cast<std::streamsize>(size_bytes));
  if (!stream.good()) {
    *error_message = std::string("failed to write output file: ") + output_path;
    return false;
  }

  return true;
}

bool WriteTextFile(const std::string& output_path,
                   const std::string& contents,
                   std::string* error_message) {
  if (!EnsureParentDirectoryExists(output_path, error_message)) {
    return false;
  }

  std::ofstream stream(output_path, std::ios::binary | std::ios::trunc);
  if (!stream) {
    *error_message = std::string("failed to open output file for writing: ") + output_path;
    return false;
  }

  stream.write(contents.data(), static_cast<std::streamsize>(contents.size()));
  if (!stream.good()) {
    *error_message = std::string("failed to write output file: ") + output_path;
    return false;
  }

  return true;
}

void PrintProbeJson(bool ok,
                    const std::string& message,
                    const std::string& source_path,
                    bool source_exists,
                    const DecodeSummary* decode_summary = nullptr) {
  const auto dng_sdk_summary = GetDngSdkSupportSummary();
  std::cout
      << "{\n"
      << "  \"diagnostics\": {\n"
      << "    \"decode_summary\": "
      << (decode_summary ? BuildDecodeSummaryJson(*decode_summary) : "null") << ",\n"
      << "    \"dng_sdk\": " << BuildDngSdkSummaryJson(dng_sdk_summary) << ",\n"
      << "    \"source_exists\": " << (source_exists ? "true" : "false") << ",\n"
      << "    \"source_path\": \"" << JsonEscape(source_path) << "\",\n"
      << "    \"stage\": \"native-probe\"\n"
      << "  },\n"
      << "  \"message\": \"" << JsonEscape(message) << "\",\n"
      << "  \"ok\": " << (ok ? "true" : "false") << "\n"
      << "}\n";
}

std::string BuildDecodeSummaryJson(const DecodeSummary& summary) {
  std::ostringstream output;
  output
      << "{\n"
      << "      \"as_shot_wb_applied\": " << summary.as_shot_wb_applied << ",\n"
      << "      \"camera_make\": \"" << JsonEscape(summary.camera_make) << "\",\n"
      << "      \"camera_model\": \"" << JsonEscape(summary.camera_model) << "\",\n"
      << "      \"black_level\": " << summary.black_level << ",\n"
      << "      \"color_description\": \"" << JsonEscape(summary.color_description) << "\",\n"
      << "      \"colors\": " << summary.colors << ",\n"
      << "      \"decoder_flags\": " << summary.decoder_flags << ",\n"
      << "      \"decoder_name\": \"" << JsonEscape(summary.decoder_name) << "\",\n"
      << "      \"filters\": " << summary.filters << ",\n"
      << "      \"flip\": " << summary.flip << ",\n"
      << "      \"image_height\": " << summary.image_height << ",\n"
      << "      \"image_width\": " << summary.image_width << ",\n"
      << "      \"inset_crop_0_height\": " << summary.inset_crop_0_height << ",\n"
      << "      \"inset_crop_0_left\": " << summary.inset_crop_0_left << ",\n"
      << "      \"inset_crop_0_top\": " << summary.inset_crop_0_top << ",\n"
      << "      \"inset_crop_0_width\": " << summary.inset_crop_0_width << ",\n"
      << "      \"inset_crop_1_height\": " << summary.inset_crop_1_height << ",\n"
      << "      \"inset_crop_1_left\": " << summary.inset_crop_1_left << ",\n"
      << "      \"inset_crop_1_top\": " << summary.inset_crop_1_top << ",\n"
      << "      \"inset_crop_1_width\": " << summary.inset_crop_1_width << ",\n"
      << "      \"inset_crop_applied_index\": " << summary.inset_crop_applied_index << ",\n"
      << "      \"left_margin\": " << summary.left_margin << ",\n"
      << "      \"linear_maximum_0\": " << summary.linear_maximum_0 << ",\n"
      << "      \"linear_maximum_1\": " << summary.linear_maximum_1 << ",\n"
      << "      \"linear_maximum_2\": " << summary.linear_maximum_2 << ",\n"
      << "      \"linear_maximum_3\": " << summary.linear_maximum_3 << ",\n"
      << "      \"libraw_version\": \"" << JsonEscape(summary.libraw_version) << "\",\n"
      << "      \"normalized_make\": \"" << JsonEscape(summary.normalized_make) << "\",\n"
      << "      \"normalized_model\": \"" << JsonEscape(summary.normalized_model) << "\",\n"
      << "      \"process_warning_names\": " << BuildJsonStringArray(summary.process_warning_names) << ",\n"
      << "      \"process_warnings\": " << summary.process_warnings << ",\n"
      << "      \"process_warnings_after_unpack\": " << summary.process_warnings_after_unpack << ",\n"
      << "      \"process_warnings_before_unpack\": " << summary.process_warnings_before_unpack << ",\n"
      << "      \"raw_count\": " << summary.raw_count << ",\n"
      << "      \"raw_height\": " << summary.raw_height << ",\n"
      << "      \"raw_processing_options\": " << summary.raw_processing_options << ",\n"
      << "      \"raw_use_dngsdk\": " << summary.raw_use_dngsdk << ",\n"
      << "      \"raw_use_rawspeed\": " << summary.raw_use_rawspeed << ",\n"
      << "      \"raw_width\": " << summary.raw_width << ",\n"
      << "      \"runtime_capabilities\": " << BuildJsonStringArray(summary.runtime_capabilities) << ",\n"
      << "      \"top_margin\": " << summary.top_margin << ",\n"
      << "      \"white_level\": " << summary.white_level << "\n"
      << "    }";
  return output.str();
}

std::string BuildDngSdkSummaryJson(const DngSdkSupportSummary& summary) {
  std::ostringstream output;
  output
      << "{\n"
      << "      \"backward_version\": \"" << JsonEscape(summary.backward_version) << "\",\n"
      << "      \"enabled\": " << (summary.enabled ? "true" : "false") << ",\n"
      << "      \"jpeg_xl_available\": " << (summary.jpeg_xl_available ? "true" : "false") << ",\n"
      << "      \"sdk_root\": \"" << JsonEscape(summary.sdk_root) << "\",\n"
      << "      \"status\": \"" << JsonEscape(summary.status) << "\"\n"
      << "    }";
  return output.str();
}

std::string BuildDngWriterConfigJson(const DngWriterConfigSummary& summary) {
  std::ostringstream output;
  output
      << "{\n"
      << "      \"can_configure_writer\": " << (summary.can_configure_writer ? "true" : "false") << ",\n"
      << "      \"compression_code\": " << summary.compression_code << ",\n"
      << "      \"compression_name\": \"" << JsonEscape(summary.compression_name) << "\",\n"
  << "      \"dng_version\": \"" << JsonEscape(summary.dng_version) << "\",\n"
      << "      \"enabled\": " << (summary.enabled ? "true" : "false") << ",\n"
  << "      \"experimental\": " << (summary.experimental ? "true" : "false") << ",\n"
      << "      \"lossless_jpeg_xl\": " << (summary.lossless_jpeg_xl ? "true" : "false") << ",\n"
      << "      \"lossy_mosaic_jpeg_xl\": " << (summary.lossy_mosaic_jpeg_xl ? "true" : "false") << ",\n"
      << "      \"save_linear_dng\": " << (summary.save_linear_dng ? "true" : "false") << ",\n"
      << "      \"status\": \"" << JsonEscape(summary.status) << "\"\n"
      << "    }";
  return output.str();
}

std::string BuildDngWriterRuntimeJson(const DngWriterRuntimeSummary& summary) {
  std::ostringstream output;
  output
      << "{\n"
      << "      \"enabled\": " << (summary.enabled ? "true" : "false") << ",\n"
      << "      \"host_created\": " << (summary.host_created ? "true" : "false") << ",\n"
      << "      \"negative_created\": " << (summary.negative_created ? "true" : "false") << ",\n"
      << "      \"status\": \"" << JsonEscape(summary.status) << "\"\n"
      << "    }";
  return output.str();
}

std::string BuildAnalysisSummaryJson(const AnalysisSummary& summary) {
  std::ostringstream output;
  output
      << "{\n"
      << "      \"cfa_index_path\": "
      << (summary.cfa_index_path.empty() ? "null" : "\"" + JsonEscape(summary.cfa_index_path) + "\"") << ",\n"
      << "      \"crop_geometry_path\": "
      << (summary.crop_geometry_path.empty() ? "null" : "\"" + JsonEscape(summary.crop_geometry_path) + "\"") << ",\n"
      << "      \"mosaic_path\": "
      << (summary.mosaic_path.empty() ? "null" : "\"" + JsonEscape(summary.mosaic_path) + "\"") << ",\n"
      << "      \"wrote_cfa_index\": " << (summary.wrote_cfa_index ? "true" : "false") << ",\n"
      << "      \"wrote_crop_geometry\": " << (summary.wrote_crop_geometry ? "true" : "false") << ",\n"
      << "      \"wrote_mosaic\": " << (summary.wrote_mosaic ? "true" : "false") << "\n"
      << "    }";
  return output.str();
}

bool DecodeWithLibRaw(const std::string& source_path,
                      DecodeSummary* summary,
                      std::string* error_message,
                      bool* has_summary) {
  LibRaw processor;
  *has_summary = false;

  summary->libraw_version = LibRaw::version();
  summary->runtime_capabilities = CollectLibRawRuntimeCapabilities();

  int result = processor.open_file(source_path.c_str());
  if (result != LIBRAW_SUCCESS) {
    *error_message = std::string("LibRaw open_file failed: ") + libraw_strerror(result);
    return false;
  }

  summary->camera_make = processor.imgdata.idata.make;
  summary->camera_model = processor.imgdata.idata.model;
  summary->normalized_make = processor.imgdata.idata.normalized_make;
  summary->normalized_model = processor.imgdata.idata.normalized_model;
  summary->raw_count = processor.imgdata.idata.raw_count;
  summary->raw_width = processor.imgdata.sizes.raw_width;
  summary->raw_height = processor.imgdata.sizes.raw_height;
  summary->image_width = processor.imgdata.sizes.width;
  summary->image_height = processor.imgdata.sizes.height;
  summary->top_margin = processor.imgdata.sizes.top_margin;
  summary->left_margin = processor.imgdata.sizes.left_margin;
  summary->flip = processor.imgdata.sizes.flip;
  summary->filters = processor.imgdata.idata.filters;
  summary->colors = processor.imgdata.idata.colors;
  summary->process_warnings = processor.imgdata.process_warnings;
  summary->process_warnings_before_unpack = processor.imgdata.process_warnings;
  summary->color_description = processor.imgdata.idata.cdesc;
  summary->black_level = processor.imgdata.color.black;
  summary->white_level = processor.imgdata.color.maximum;
  summary->linear_maximum_0 = processor.imgdata.color.linear_max[0];
  summary->linear_maximum_1 = processor.imgdata.color.linear_max[1];
  summary->linear_maximum_2 = processor.imgdata.color.linear_max[2];
  summary->linear_maximum_3 = processor.imgdata.color.linear_max[3];
  summary->inset_crop_0_left = processor.imgdata.sizes.raw_inset_crops[0].cleft;
  summary->inset_crop_0_top = processor.imgdata.sizes.raw_inset_crops[0].ctop;
  summary->inset_crop_0_width = processor.imgdata.sizes.raw_inset_crops[0].cwidth;
  summary->inset_crop_0_height = processor.imgdata.sizes.raw_inset_crops[0].cheight;
  summary->inset_crop_1_left = processor.imgdata.sizes.raw_inset_crops[1].cleft;
  summary->inset_crop_1_top = processor.imgdata.sizes.raw_inset_crops[1].ctop;
  summary->inset_crop_1_width = processor.imgdata.sizes.raw_inset_crops[1].cwidth;
  summary->inset_crop_1_height = processor.imgdata.sizes.raw_inset_crops[1].cheight;
  summary->as_shot_wb_applied = processor.imgdata.color.as_shot_wb_applied;
  summary->raw_use_rawspeed = processor.imgdata.rawparams.use_rawspeed;
  summary->raw_use_dngsdk = processor.imgdata.idata.dng_version != 0
                                  ? processor.imgdata.rawparams.use_dngsdk
                                  : 0;
  summary->raw_processing_options = processor.imgdata.rawparams.options;

  libraw_decoder_info_t decoder_info;
  if (processor.get_decoder_info(&decoder_info) == LIBRAW_SUCCESS) {
    summary->decoder_flags = decoder_info.decoder_flags;
    if (decoder_info.decoder_name != nullptr) {
      summary->decoder_name = decoder_info.decoder_name;
    }
  }

  const char* open_decoder_name = libraw_unpack_function_name(&processor.imgdata);
  if (open_decoder_name != nullptr) {
    summary->decoder_name = open_decoder_name;
  }
  *has_summary = true;

  result = processor.unpack();
  if (result != LIBRAW_SUCCESS) {
    *error_message = std::string("LibRaw unpack failed: ") + libraw_strerror(result);
    processor.recycle();
    return false;
  }

  summary->process_warnings_after_unpack = processor.imgdata.process_warnings;
  summary->process_warnings = processor.imgdata.process_warnings;
  summary->process_warning_names = DecodeProcessWarnings(processor.imgdata.process_warnings);
  summary->black_level = processor.imgdata.color.black;
  summary->white_level = processor.imgdata.color.maximum;
  summary->linear_maximum_0 = processor.imgdata.color.linear_max[0];
  summary->linear_maximum_1 = processor.imgdata.color.linear_max[1];
  summary->linear_maximum_2 = processor.imgdata.color.linear_max[2];
  summary->linear_maximum_3 = processor.imgdata.color.linear_max[3];

  LibRaw inset_crop_probe;
  if (inset_crop_probe.open_file(source_path.c_str()) == LIBRAW_SUCCESS &&
      inset_crop_probe.unpack() == LIBRAW_SUCCESS) {
    summary->inset_crop_applied_index = static_cast<unsigned>(
        inset_crop_probe.adjust_to_raw_inset_crop(0x3, 0.0f));
  }
  inset_crop_probe.recycle();

  const char* decoder_name = libraw_unpack_function_name(&processor.imgdata);
  if (decoder_name != nullptr) {
    summary->decoder_name = decoder_name;
  }

  processor.recycle();
  return true;
}

std::string BuildCropGeometryJson(const std::string& source_path,
                                  const DecodeSummary& summary) {
  std::ostringstream output;
  output
      << "{\n"
      << "  \"source_path\": \"" << JsonEscape(source_path) << "\",\n"
  << "  \"camera_make\": \"" << JsonEscape(summary.camera_make) << "\",\n"
  << "  \"camera_model\": \"" << JsonEscape(summary.camera_model) << "\",\n"
  << "  \"normalized_make\": \"" << JsonEscape(summary.normalized_make) << "\",\n"
  << "  \"normalized_model\": \"" << JsonEscape(summary.normalized_model) << "\",\n"
      << "  \"raw_width\": " << summary.raw_width << ",\n"
      << "  \"raw_height\": " << summary.raw_height << ",\n"
      << "  \"image_width\": " << summary.image_width << ",\n"
      << "  \"image_height\": " << summary.image_height << ",\n"
      << "  \"left_margin\": " << summary.left_margin << ",\n"
      << "  \"top_margin\": " << summary.top_margin << ",\n"
      << "  \"active_area\": ["
      << summary.top_margin << ", "
      << summary.left_margin << ", "
      << (summary.top_margin + summary.image_height) << ", "
      << (summary.left_margin + summary.image_width) << "],\n"
      << "  \"inset_crops\": [\n"
      << "    {\"index\": 0, \"left\": " << summary.inset_crop_0_left
      << ", \"top\": " << summary.inset_crop_0_top
      << ", \"width\": " << summary.inset_crop_0_width
      << ", \"height\": " << summary.inset_crop_0_height << "},\n"
      << "    {\"index\": 1, \"left\": " << summary.inset_crop_1_left
      << ", \"top\": " << summary.inset_crop_1_top
      << ", \"width\": " << summary.inset_crop_1_width
      << ", \"height\": " << summary.inset_crop_1_height << "}\n"
      << "  ],\n"
      << "  \"selected_inset_crop_index\": " << summary.inset_crop_applied_index << ",\n"
      << "  \"black_level\": " << summary.black_level << ",\n"
      << "  \"white_level\": " << summary.white_level << ",\n"
      << "  \"linear_maximum\": ["
      << summary.linear_maximum_0 << ", "
      << summary.linear_maximum_1 << ", "
      << summary.linear_maximum_2 << ", "
      << summary.linear_maximum_3 << "],\n"
      << "  \"filters\": " << summary.filters << ",\n"
      << "  \"colors\": " << summary.colors << ",\n"
      << "  \"color_description\": \"" << JsonEscape(summary.color_description) << "\",\n"
      << "  \"mosaic_dump_format\": {\"dtype\": \"uint16_le\", \"layout\": \"raw_width_by_raw_height\"},\n"
      << "  \"cfa_index_dump_format\": {\"dtype\": \"uint8\", \"layout\": \"raw_width_by_raw_height\", \"channels\": \"0=R,1=G,2=B,3=G2_or_alt\"}\n"
      << "}\n";
  return output.str();
}

bool RunAnalysisWithLibRaw(const std::string& source_path,
                           const std::string& dump_mosaic_path,
                           const std::string& dump_cfa_index_path,
                           const std::string& dump_crop_geometry_path,
                           const DecodeSummary& decode_summary,
                           AnalysisSummary* analysis_summary,
                           std::string* error_message) {
  if (!dump_crop_geometry_path.empty()) {
    const std::string crop_geometry_json = BuildCropGeometryJson(source_path, decode_summary);
    if (!WriteTextFile(dump_crop_geometry_path, crop_geometry_json, error_message)) {
      return false;
    }
    analysis_summary->wrote_crop_geometry = true;
    analysis_summary->crop_geometry_path = dump_crop_geometry_path;
  }

  if (dump_mosaic_path.empty() && dump_cfa_index_path.empty()) {
    return true;
  }

  LibRaw processor;
  int result = processor.open_file(source_path.c_str());
  if (result != LIBRAW_SUCCESS) {
    *error_message = std::string("LibRaw open_file failed: ") + libraw_strerror(result);
    return false;
  }

  result = processor.unpack();
  if (result != LIBRAW_SUCCESS) {
    *error_message = std::string("LibRaw unpack failed: ") + libraw_strerror(result);
    processor.recycle();
    return false;
  }

  const ushort* raw_image = processor.imgdata.rawdata.raw_image;
  if (raw_image == nullptr) {
    *error_message = "LibRaw unpack did not expose a Bayer raw_image buffer";
    processor.recycle();
    return false;
  }

  const size_t pixel_count = static_cast<size_t>(processor.imgdata.sizes.raw_width) *
                             static_cast<size_t>(processor.imgdata.sizes.raw_height);

  if (!dump_mosaic_path.empty()) {
    if (!WriteBinaryFile(dump_mosaic_path,
                         raw_image,
                         pixel_count * sizeof(uint16_t),
                         error_message)) {
      processor.recycle();
      return false;
    }
    analysis_summary->wrote_mosaic = true;
    analysis_summary->mosaic_path = dump_mosaic_path;
  }

  if (!dump_cfa_index_path.empty()) {
    std::vector<uint8_t> cfa_index(pixel_count);
    for (unsigned row = 0; row < processor.imgdata.sizes.raw_height; ++row) {
      for (unsigned col = 0; col < processor.imgdata.sizes.raw_width; ++col) {
        const size_t index = static_cast<size_t>(row) * processor.imgdata.sizes.raw_width + col;
        cfa_index[index] = static_cast<uint8_t>(processor.COLOR(row, col));
      }
    }

    if (!WriteBinaryFile(dump_cfa_index_path,
                         cfa_index.data(),
                         cfa_index.size() * sizeof(uint8_t),
                         error_message)) {
      processor.recycle();
      return false;
    }
    analysis_summary->wrote_cfa_index = true;
    analysis_summary->cfa_index_path = dump_cfa_index_path;
  }

  processor.recycle();
  return true;
}

void PrintAnalyzeJson(bool ok,
                      const std::string& message,
                      const std::string& source_path,
                      bool source_exists,
                      const DecodeSummary* decode_summary,
                      const AnalysisSummary* analysis_summary) {
  std::cout
      << "{\n"
      << "  \"diagnostics\": {\n"
      << "    \"analysis_outputs\": "
      << (analysis_summary ? BuildAnalysisSummaryJson(*analysis_summary) : "null") << ",\n"
      << "    \"decode_summary\": "
      << (decode_summary ? BuildDecodeSummaryJson(*decode_summary) : "null") << ",\n"
      << "    \"source_exists\": " << (source_exists ? "true" : "false") << ",\n"
      << "    \"source_path\": \"" << JsonEscape(source_path) << "\",\n"
      << "    \"stage\": \"native-analyze\"\n"
      << "  },\n"
      << "  \"message\": \"" << JsonEscape(message) << "\",\n"
      << "  \"ok\": " << (ok ? "true" : "false") << "\n"
      << "}\n";
}

void PrintJsonFailure(const std::string& message,
                      const std::string& source_path = {},
                      const std::string& output_path = {},
                      const std::string& compression = {},
                      bool overwrite = false,
                      bool source_exists = false,
                      bool output_exists = false,
                      const DecodeSummary* decode_summary = nullptr) {
  const auto dng_sdk_summary = GetDngSdkSupportSummary();
  const auto writer_config_summary = BuildDngWriterConfigSummary(compression);
  const auto writer_runtime_summary = BuildDngWriterRuntimeSummary(compression);
  std::cout
      << "{\n"
      << "  \"diagnostics\": {\n"
      << "    \"compression\": \"" << JsonEscape(compression) << "\",\n"
      << "    \"decode_summary\": "
      << (decode_summary ? BuildDecodeSummaryJson(*decode_summary) : "null") << ",\n"
      << "    \"dng_sdk\": " << BuildDngSdkSummaryJson(dng_sdk_summary) << ",\n"
      << "    \"writer_config\": " << BuildDngWriterConfigJson(writer_config_summary) << ",\n"
      << "    \"writer_runtime\": " << BuildDngWriterRuntimeJson(writer_runtime_summary) << ",\n"
      << "    \"output_exists\": " << (output_exists ? "true" : "false") << ",\n"
      << "    \"output_path\": \"" << JsonEscape(output_path) << "\",\n"
      << "    \"overwrite\": " << (overwrite ? "true" : "false") << ",\n"
      << "    \"source_exists\": " << (source_exists ? "true" : "false") << ",\n"
      << "    \"source_path\": \"" << JsonEscape(source_path) << "\",\n"
      << "    \"stage\": \"native-helper-stub\"\n"
      << "  },\n"
      << "  \"message\": \"" << JsonEscape(message) << "\",\n"
      << "  \"ok\": false\n"
      << "}\n";
}

    void PrintJsonSuccess(const std::string& message,
              const std::string& source_path,
              const std::string& output_path,
              const std::string& compression,
              bool overwrite,
              bool source_exists,
              bool output_exists,
              const DecodeSummary* decode_summary = nullptr) {
      const auto dng_sdk_summary = GetDngSdkSupportSummary();
      const auto writer_config_summary = BuildDngWriterConfigSummary(compression);
      const auto writer_runtime_summary = BuildDngWriterRuntimeSummary(compression);
      std::cout
      << "{\n"
      << "  \"diagnostics\": {\n"
      << "    \"compression\": \"" << JsonEscape(compression) << "\",\n"
      << "    \"decode_summary\": "
      << (decode_summary ? BuildDecodeSummaryJson(*decode_summary) : "null") << ",\n"
      << "    \"dng_sdk\": " << BuildDngSdkSummaryJson(dng_sdk_summary) << ",\n"
      << "    \"writer_config\": " << BuildDngWriterConfigJson(writer_config_summary) << ",\n"
      << "    \"writer_runtime\": " << BuildDngWriterRuntimeJson(writer_runtime_summary) << ",\n"
      << "    \"output_exists\": " << (output_exists ? "true" : "false") << ",\n"
      << "    \"output_path\": \"" << JsonEscape(output_path) << "\",\n"
      << "    \"overwrite\": " << (overwrite ? "true" : "false") << ",\n"
      << "    \"source_exists\": " << (source_exists ? "true" : "false") << ",\n"
      << "    \"source_path\": \"" << JsonEscape(source_path) << "\",\n"
      << "    \"stage\": \"native-write\"\n"
      << "  },\n"
      << "  \"message\": \"" << JsonEscape(message) << "\",\n"
      << "  \"ok\": true,\n"
      << "  \"output_path\": \"" << JsonEscape(output_path) << "\"\n"
      << "}\n";
    }

}  // namespace

int main(int argc, char** argv) {
  if (argc < 2) {
    return PrintUsage();
  }

  const std::string command = argv[1];
  if (command == "probe") {
    if (argc != 4 || std::string(argv[2]) != "--source") {
      return PrintUsage();
    }

    const std::string source_path = argv[3];
    const bool source_exists = std::filesystem::exists(source_path);
    if (!source_exists) {
      PrintProbeJson(false, "source file does not exist", source_path, source_exists);
      return 1;
    }

    DecodeSummary decode_summary;
    std::string decode_error;
    bool has_decode_summary = false;
    if (!DecodeWithLibRaw(source_path, &decode_summary, &decode_error, &has_decode_summary)) {
      PrintProbeJson(false,
                     decode_error,
                     source_path,
                     source_exists,
                     has_decode_summary ? &decode_summary : nullptr);
      return 1;
    }

    PrintProbeJson(true, "native probe succeeded", source_path, source_exists, &decode_summary);
    return 0;
  }

  if (command == "selftest-write") {
    if (argc != 4 && argc != 6) {
      return PrintUsage();
    }

    if (std::string(argv[2]) != "--output") {
      return PrintUsage();
    }

    const std::string output_path = argv[3];
    std::string compression = "deflate";

    if (argc == 6) {
      if (std::string(argv[4]) != "--compression") {
        return PrintUsage();
      }
      compression = argv[5];
    }

    const auto write_result = WriteSyntheticLinearDng(output_path, compression);
    if (write_result.ok) {
      PrintJsonSuccess(write_result.message,
                       "synthetic-gradient.raw",
                       output_path,
                       compression,
                       true,
                       true,
                       std::filesystem::exists(output_path),
                       nullptr);
      return 0;
    }

    PrintJsonFailure(write_result.message,
                     "synthetic-gradient.raw",
                     output_path,
                     compression,
                     true,
                     true,
                     std::filesystem::exists(output_path),
                     nullptr);
    return 1;
  }

  if (command == "analyze") {
    if (argc != 4 || std::string(argv[2]) != "--request-json") {
      return PrintUsage();
    }

    const std::string request_json = argv[3];
    if (request_json.empty()) {
      PrintAnalyzeJson(false, "request payload is empty", {}, false, nullptr, nullptr);
      return 1;
    }

    const std::string source_path = FindJsonString(request_json, "source_path");
    const std::string dump_mosaic_path = FindJsonString(request_json, "dump_mosaic_path");
    const std::string dump_cfa_index_path = FindJsonString(request_json, "dump_cfa_index_path");
    const std::string dump_crop_geometry_path = FindJsonString(request_json, "dump_crop_geometry_path");

    if (source_path.empty()) {
      PrintAnalyzeJson(false, "source_path is missing from request", {}, false, nullptr, nullptr);
      return 1;
    }

    const bool source_exists = std::filesystem::exists(source_path);
    if (!source_exists) {
      PrintAnalyzeJson(false, "source file does not exist", source_path, source_exists, nullptr, nullptr);
      return 1;
    }

    DecodeSummary decode_summary;
    std::string decode_error;
    bool has_decode_summary = false;
    if (!DecodeWithLibRaw(source_path, &decode_summary, &decode_error, &has_decode_summary)) {
      PrintAnalyzeJson(false,
                       decode_error,
                       source_path,
                       source_exists,
                       has_decode_summary ? &decode_summary : nullptr,
                       nullptr);
      return 1;
    }

    AnalysisSummary analysis_summary;
    std::string analysis_error;
    if (!RunAnalysisWithLibRaw(source_path,
                               dump_mosaic_path,
                               dump_cfa_index_path,
                               dump_crop_geometry_path,
                               decode_summary,
                               &analysis_summary,
                               &analysis_error)) {
      PrintAnalyzeJson(false,
                       analysis_error,
                       source_path,
                       source_exists,
                       &decode_summary,
                       &analysis_summary);
      return 1;
    }

    PrintAnalyzeJson(true,
                     "native analysis succeeded",
                     source_path,
                     source_exists,
                     &decode_summary,
                     &analysis_summary);
    return 0;
  }

  if (command != "convert") {
    return PrintUsage();
  }

  if (argc != 4 || std::string(argv[2]) != "--request-json") {
    return PrintUsage();
  }

  const std::string request_json = argv[3];
  if (request_json.empty()) {
    PrintJsonFailure("request payload is empty");
    return 1;
  }

  const std::string source_path = FindJsonString(request_json, "source_path");
  const std::string output_path = FindJsonString(request_json, "output_path");
  const std::string compression = FindJsonString(request_json, "compression");
  const bool overwrite = FindJsonBool(request_json, "overwrite", false);
  const auto source_metadata = ParseSourceLinearDngMetadata(request_json);

  if (source_path.empty()) {
    PrintJsonFailure("source_path is missing from request");
    return 1;
  }
  if (output_path.empty()) {
    PrintJsonFailure("output_path is missing from request", source_path);
    return 1;
  }

  const std::set<std::string> valid_compressions = {
      "uncompressed",
      "deflate",
      "jpeg-xl",
  };
  if (valid_compressions.find(compression) == valid_compressions.end()) {
    PrintJsonFailure("compression mode is invalid",
                     source_path,
                     output_path,
                     compression,
                     overwrite);
    return 1;
  }

  const bool source_exists = std::filesystem::exists(source_path);
  const bool output_exists = std::filesystem::exists(output_path);
  if (!source_exists) {
    PrintJsonFailure("source file does not exist",
                     source_path,
                     output_path,
                     compression,
                     overwrite,
                     source_exists,
                     output_exists);
    return 1;
  }

  DecodeSummary decode_summary;
  std::string decode_error;
  bool has_decode_summary = false;
  if (!DecodeWithLibRaw(source_path, &decode_summary, &decode_error, &has_decode_summary)) {
    PrintJsonFailure(decode_error,
                     source_path,
                     output_path,
                     compression,
                     overwrite,
                     source_exists,
                     output_exists,
                     has_decode_summary ? &decode_summary : nullptr);
    return 1;
  }

  const auto write_result = WriteLinearDngFromRaw(source_path, output_path, compression, source_metadata);
  if (write_result.ok) {
    PrintJsonSuccess(write_result.message,
                     source_path,
                     output_path,
                     compression,
                     overwrite,
                     source_exists,
                     std::filesystem::exists(output_path),
                     &decode_summary);
    return 0;
  }

  PrintJsonFailure(write_result.message,
                   source_path,
                   output_path,
                   compression,
                   overwrite,
                   source_exists,
                   std::filesystem::exists(output_path),
                   &decode_summary);
  return 1;
}
