#include "dng_sdk_bridge.h"
#include "dng_writer_bridge.h"
#include "vendor_makernote.h"
#include "stack_guidance.h"

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

int PrintUsage() {
  std::cerr << "usage: hiraco convert <source> <output> [--compression uncompressed|deflate|jpeg-xl] [--debug]\n";
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

SourceLinearDngMetadata BuildMetadataFromLibRaw(const std::string& source_path,
                                                const DecodeSummary& decode_summary) {
  SourceLinearDngMetadata metadata;
  metadata.make = decode_summary.camera_make;
  metadata.model = decode_summary.camera_model;
  // TODO: extract default matrix or as_shot_neutral directly from LibRaw instead of Python Exiftool 
  // For now, we seed the bridge with the basic decode limits.
  metadata.black_level = decode_summary.black_level;
  metadata.has_black_level = true;
  metadata.white_level = decode_summary.white_level;
  metadata.has_white_level = true;

  // Read maker notes to get working geometry 
  auto mn = ReadVendorMakerNote(source_path);
  if (mn.ok && mn.has_working_geometry) {
      metadata.has_working_geometry = true;
      metadata.working_width = mn.working_width;
      metadata.working_height = mn.working_height;
  }
  return metadata;
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

    bool PopulateStackGuidance(const std::string& source_path,
                           const std::string& tmp_dir,
                           SourceLinearDngMetadata* metadata,
                           std::string* error_message) {
  auto mn = ReadVendorMakerNote(source_path);
  if (!mn.ok) {
    // Not an error — just no stack guidance available.
    return true;
  }

  if (!mn.has_stacked_image ||
      mn.stacked_image_label != "Hand-held high resolution (11 12)") {
    return true;
  }

  if (mn.unknown_block_3.empty()) {
    return true;
  }

  auto guidance = ComputeStackGuidance(mn.unknown_block_3);
  if (!guidance.ok) {
    *error_message = "stack guidance computation failed: " + guidance.error;
    return false;
  }

  // Write guidance maps to temp files.
  const std::filesystem::path dir(tmp_dir);
  std::error_code ec;
  std::filesystem::create_directories(dir, ec);
  if (ec) {
    *error_message = "failed to create temp directory: " + dir.string();
    return false;
  }

  const std::filesystem::path source(source_path);
  const std::string stem = source.stem().string();

  auto write_map = [&](const std::vector<float>& data,
                        const std::string& suffix,
                        std::string* path_out) -> bool {
    const std::string filename = stem + suffix;
    const std::filesystem::path map_path = dir / filename;
    std::ofstream out(map_path, std::ios::binary | std::ios::trunc);
    if (!out) {
      *error_message = "failed to write map: " + map_path.string();
      return false;
    }
    out.write(reinterpret_cast<const char*>(data.data()),
              static_cast<std::streamsize>(data.size() * sizeof(float)));
    if (!out.good()) {
      *error_message = "failed to write map: " + map_path.string();
      return false;
    }
    *path_out = map_path.string();
    return true;
  };

  const uint32_t w = StackGuidanceMaps::kTileWidth;
  const uint32_t h = StackGuidanceMaps::kTileHeight;

  std::string path;

  if (!write_map(guidance.stability, "_stack_stability.f32", &path)) return false;
  metadata->has_stack_stability_map = true;
  metadata->stack_stability_path = path;
  metadata->stack_stability_width = w;
  metadata->stack_stability_height = h;

  if (!write_map(guidance.mean, "_stack_mean.f32", &path)) return false;
  metadata->has_stack_mean_map = true;
  metadata->stack_mean_path = path;
  metadata->stack_mean_width = w;
  metadata->stack_mean_height = h;

  if (!write_map(guidance.guide, "_stack_guide.f32", &path)) return false;
  metadata->has_stack_guide_map = true;
  metadata->stack_guide_path = path;
  metadata->stack_guide_width = w;
  metadata->stack_guide_height = h;

  if (!write_map(guidance.tensor_x, "_stack_tensor_x.f32", &path)) return false;
  metadata->has_stack_tensor_x_map = true;
  metadata->stack_tensor_x_path = path;
  metadata->stack_tensor_x_width = w;
  metadata->stack_tensor_x_height = h;

  if (!write_map(guidance.tensor_y, "_stack_tensor_y.f32", &path)) return false;
  metadata->has_stack_tensor_y_map = true;
  metadata->stack_tensor_y_path = path;
  metadata->stack_tensor_y_width = w;
  metadata->stack_tensor_y_height = h;

  if (!write_map(guidance.tensor_coherence, "_stack_tensor_coherence.f32", &path)) return false;
  metadata->has_stack_tensor_coherence_map = true;
  metadata->stack_tensor_coherence_path = path;
  metadata->stack_tensor_coherence_width = w;
  metadata->stack_tensor_coherence_height = h;

  if (!write_map(guidance.alias, "_stack_alias.f32", &path)) return false;
  metadata->has_stack_alias_map = true;
  metadata->stack_alias_path = path;
  metadata->stack_alias_width = w;
  metadata->stack_alias_height = h;

  return true;
}

int RunDirectConvert(const std::string& source_path,
                     const std::string& output_path,
                     const std::string& compression,
                     bool debug) {
  const bool source_exists = std::filesystem::exists(source_path);
  if (!source_exists) {
    std::cerr << "Error: source file does not exist: " << source_path << "\n";
    return 1;
  }

  DecodeSummary decode_summary;
  std::string decode_error;
  bool has_decode_summary = false;
  if (!DecodeWithLibRaw(source_path, &decode_summary, &decode_error, &has_decode_summary)) {
    std::cerr << "Error: " << decode_error << "\n";
    return 1;
  }

  auto metadata = BuildMetadataFromLibRaw(source_path, decode_summary);

  // Compute stack guidance maps if applicable.
  const std::filesystem::path output_dir = std::filesystem::path(output_path).parent_path();
  const std::string tmp_dir = (output_dir.empty() ? std::filesystem::path(".") : output_dir).string()
                              + "/.hiraco_tmp";
  std::string guidance_error;
  if (!PopulateStackGuidance(source_path, tmp_dir, &metadata, &guidance_error)) {
    std::cerr << "Error: " << guidance_error << "\n";
    return 1;
  }

  const auto write_result = WriteLinearDngFromRaw(source_path, output_path, compression, metadata);

  // Clean up temp files.
  {
    std::error_code ec;
    std::filesystem::remove_all(tmp_dir, ec);
  }

  if (write_result.ok) {
    std::cout << "Success: " << output_path << "\n";
    return 0;
  }

  std::cerr << "Error: " << write_result.message << "\n";
  return 1;
}

}  // namespace

int main(int argc, char** argv) {
  if (argc < 4) {
    return PrintUsage();
  }

  const std::string command = argv[1];
  if (command != "convert") {
    return PrintUsage();
  }

  const std::string source_path = argv[2];
  const std::string output_path = argv[3];
  std::string compression = "uncompressed";
  bool debug = false;

  for (int i = 4; i < argc; ++i) {
    const std::string arg = argv[i];
    if (arg == "--compression" && i + 1 < argc) {
      compression = argv[++i];
    } else if (arg == "--debug") {
      debug = true;
    } else {
      return PrintUsage();
    }
  }

  const std::set<std::string> valid_compressions = {
      "uncompressed",
      "deflate",
      "jpeg-xl",
  };
  if (valid_compressions.find(compression) == valid_compressions.end()) {
    std::cerr << "Error: invalid compression mode: " << compression << "\n";
    return 2;
  }

  return RunDirectConvert(source_path, output_path, compression, debug);
}
