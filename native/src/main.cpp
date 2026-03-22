#include "dng_sdk_bridge.h"
#include "dng_writer_bridge.h"

#include <filesystem>
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
  unsigned black_level = 0;
  unsigned white_level = 0;
  unsigned linear_maximum_0 = 0;
  unsigned linear_maximum_1 = 0;
  unsigned linear_maximum_2 = 0;
  unsigned linear_maximum_3 = 0;
};

std::string BuildDecodeSummaryJson(const DecodeSummary& summary);
std::string BuildDngSdkSummaryJson(const DngSdkSupportSummary& summary);
std::string BuildDngWriterConfigJson(const DngWriterConfigSummary& summary);
std::string BuildDngWriterRuntimeJson(const DngWriterRuntimeSummary& summary);

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

int PrintUsage() {
  std::cerr << "usage: hiraco-native convert --request-json <json>\n";
  std::cerr << "       hiraco-native probe --source <path>\n";
  std::cerr << "       hiraco-native selftest-write --output <path> [--compression uncompressed|deflate|jpeg-xl]\n";
  return 2;
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
      << "      \"camera_make\": \"" << JsonEscape(summary.camera_make) << "\",\n"
      << "      \"camera_model\": \"" << JsonEscape(summary.camera_model) << "\",\n"
      << "      \"black_level\": " << summary.black_level << ",\n"
      << "      \"color_description\": \"" << JsonEscape(summary.color_description) << "\",\n"
      << "      \"colors\": " << summary.colors << ",\n"
      << "      \"decoder_name\": \"" << JsonEscape(summary.decoder_name) << "\",\n"
      << "      \"filters\": " << summary.filters << ",\n"
      << "      \"flip\": " << summary.flip << ",\n"
      << "      \"image_height\": " << summary.image_height << ",\n"
      << "      \"image_width\": " << summary.image_width << ",\n"
      << "      \"left_margin\": " << summary.left_margin << ",\n"
      << "      \"linear_maximum_0\": " << summary.linear_maximum_0 << ",\n"
      << "      \"linear_maximum_1\": " << summary.linear_maximum_1 << ",\n"
      << "      \"linear_maximum_2\": " << summary.linear_maximum_2 << ",\n"
      << "      \"linear_maximum_3\": " << summary.linear_maximum_3 << ",\n"
      << "      \"normalized_make\": \"" << JsonEscape(summary.normalized_make) << "\",\n"
      << "      \"normalized_model\": \"" << JsonEscape(summary.normalized_model) << "\",\n"
      << "      \"process_warnings\": " << summary.process_warnings << ",\n"
      << "      \"raw_count\": " << summary.raw_count << ",\n"
      << "      \"raw_height\": " << summary.raw_height << ",\n"
      << "      \"raw_width\": " << summary.raw_width << ",\n"
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
      << "      \"enabled\": " << (summary.enabled ? "true" : "false") << ",\n"
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

bool DecodeWithLibRaw(const std::string& source_path,
                      DecodeSummary* summary,
                      std::string* error_message,
                      bool* has_summary) {
  LibRaw processor;
  *has_summary = false;

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
  summary->color_description = processor.imgdata.idata.cdesc;
  summary->black_level = processor.imgdata.color.black;
  summary->white_level = processor.imgdata.color.maximum;
  summary->linear_maximum_0 = processor.imgdata.color.linear_max[0];
  summary->linear_maximum_1 = processor.imgdata.color.linear_max[1];
  summary->linear_maximum_2 = processor.imgdata.color.linear_max[2];
  summary->linear_maximum_3 = processor.imgdata.color.linear_max[3];
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

  const char* decoder_name = libraw_unpack_function_name(&processor.imgdata);
  if (decoder_name != nullptr) {
    summary->decoder_name = decoder_name;
  }

  processor.recycle();
  return true;
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

  const auto write_result = WriteLinearDngFromRaw(source_path, output_path, compression);
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