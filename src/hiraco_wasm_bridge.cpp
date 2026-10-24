// hiraco_wasm_bridge.cpp  —  The browser facing WebAssembly bridge.
//
// The browser has no file system that the conversion can rely on, so the
// bridge takes the RAW data as a buffer, writes it into the virtual file
// system the Emscripten runtime provides, runs the ordinary conversion, and
// hands the resulting DNG back as a buffer.  The conversion itself is the
// same code path the command line tool uses, so both produce the same file.

#include "hiraco_wasm_bridge.h"

#include "dng_sdk_bridge.h"
#include "hiraco_core.h"

#include <algorithm>
#include <atomic>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <map>
#include <memory>
#include <string>
#include <vector>

namespace {

// Where the bridge stages its input and output inside the virtual file
// system.  The names are fixed so that a browser page can pre-load a file
// under a known name, and so that repeated calls do not accumulate files.
constexpr const char* kInputDirectory = "/hiraco";
constexpr const char* kOutputPath = "/hiraco/output.dng";

std::string& LastError() {
  static std::string message;
  return message;
}

bool WriteWholeFile(const std::filesystem::path& path, const uint8_t* data, size_t size) {
  std::error_code error;
  std::filesystem::create_directories(path.parent_path(), error);
  std::ofstream stream(path, std::ios::binary | std::ios::trunc);
  if (!stream) {
    return false;
  }
  if (size > 0) {
    stream.write(reinterpret_cast<const char*>(data), static_cast<std::streamsize>(size));
  }
  return stream.good();
}

bool ReadWholeFile(const std::filesystem::path& path, std::vector<uint8_t>* out) {
  std::ifstream stream(path, std::ios::binary | std::ios::ate);
  if (!stream) {
    return false;
  }
  const std::streamsize size = stream.tellg();
  if (size < 0) {
    return false;
  }
  stream.seekg(0, std::ios::beg);
  out->resize(static_cast<size_t>(size));
  if (size > 0) {
    stream.read(reinterpret_cast<char*>(out->data()), size);
    if (!stream) {
      return false;
    }
  }
  return true;
}

// The name of the staged input file, derived from the caller's name so that
// LibRaw sees the right extension.
std::string StagedInputPath(const char* input_name) {
  std::string name = input_name != nullptr ? input_name : "input.raw";
  // Keep only the file name; a path from the caller must not escape the
  // staging directory.
  const size_t separator = name.find_last_of("/\\");
  if (separator != std::string::npos) {
    name = name.substr(separator + 1);
  }
  if (name.empty()) {
    name = "input.raw";
  }
  return std::string(kInputDirectory) + "/" + name;
}

// ---------------------------------------------------------------------------
// The browser facing session API
//
// A browser has no file system for the core to work with, so a session
// owns the staged files, the prepared source and the buffers the page asked
// for.  Keeping the prepared source alive between calls is what lets the
// page render a preview and then convert without decoding the RAW file
// twice.
// ---------------------------------------------------------------------------

constexpr size_t kStageValueCount = HIRACO_WASM_STAGE_VALUE_COUNT;

struct WasmSession {
  PreparedSource prepared;
  std::string output_path;
  std::shared_ptr<std::atomic<bool>> cancel_requested = std::make_shared<std::atomic<bool>>(false);
};

std::map<uint32_t, WasmSession>& Sessions() {
  static std::map<uint32_t, WasmSession> sessions;
  return sessions;
}

uint32_t& NextSessionId() {
  static uint32_t next_session_id = 1;
  return next_session_id;
}

WasmSession* FindSession(uint32_t session_id) {
  auto entry = Sessions().find(session_id);
  return entry == Sessions().end() ? nullptr : &entry->second;
}

// The progress of a running operation is reported on stdout, one line per
// update:
//
//   HIRACO_PROGRESS|<phase>|<fraction>|<message>
//
// The line is flushed right away, because the C library buffers stdout and
// the page has to see the progress while the operation is still running.
// The browser side picks the lines up from the module's print channel.
constexpr const char* kProgressPrefix = "HIRACO_PROGRESS|";

std::string SanitizeForProgressLine(const std::string& text) {
  std::string result;
  result.reserve(text.size());
  for (char character : text) {
    result.push_back(character == '|' || character == '\n' || character == '\r' ? ' ' : character);
  }
  return result;
}

void ReportProgress(const ProcessingProgress& progress) {
  std::printf("%s%s|%.6f|%s\n",
              kProgressPrefix,
              SanitizeForProgressLine(progress.phase).c_str(),
              progress.fraction,
              SanitizeForProgressLine(progress.message).c_str());
  std::fflush(stdout);
}

// Builds the stage overrides from the flat value list the browser sends.
// A NaN means "not overridden", which keeps the caller from having to know
// which settings differ from the base.
void BuildStageOverrideSet(const double* values, size_t count, StageOverrideSet* overrides) {
  if (overrides == nullptr) {
    return;
  }
  *overrides = StageOverrideSet();
  if (values == nullptr || count == 0) {
    return;
  }
  const size_t available = std::min(count, kStageValueCount);
  const auto is_set = [](double value) { return !std::isnan(value); };
  // Shorter value lists are allowed: every read is bounded by available, so
  // a count below kStageValueCount never goes past the end of values.
  if (available > 0 && is_set(values[0])) overrides->stage1_psf_sigma = static_cast<float>(values[0]);
  if (available > 1 && is_set(values[1])) overrides->stage1_nsr = static_cast<float>(values[1]);
  if (available > 2 && is_set(values[2])) overrides->stage2_denoise = static_cast<float>(values[2]);
  if (available > 3 && is_set(values[3])) overrides->stage2_gain1 = static_cast<float>(values[3]);
  if (available > 4 && is_set(values[4])) overrides->stage2_gain2 = static_cast<float>(values[4]);
  if (available > 5 && is_set(values[5])) overrides->stage2_gain3 = static_cast<float>(values[5]);
  if (available > 6 && is_set(values[6])) overrides->stage3_radius = static_cast<int>(std::lround(values[6]));
  if (available > 7 && is_set(values[7])) overrides->stage3_gain = static_cast<float>(values[7]);
}

void CopyResolvedStageSettings(const ResolvedStageSettings& settings, double* values) {
  if (values == nullptr) {
    return;
  }
  values[0] = settings.stage1_psf_sigma;
  values[1] = settings.stage1_nsr;
  values[2] = settings.stage2_denoise;
  values[3] = settings.stage2_gain1;
  values[4] = settings.stage2_gain2;
  values[5] = settings.stage2_gain3;
  values[6] = static_cast<double>(settings.stage3_radius);
  values[7] = settings.stage3_gain;
}

std::string JsonString(const std::string& text) {
  std::string result = "\"";
  for (unsigned char character : text) {
    switch (character) {
      case '"':
        result += "\\\"";
        break;
      case '\\':
        result += "\\\\";
        break;
      case '\n':
        result += "\\n";
        break;
      case '\r':
        result += "\\r";
        break;
      case '\t':
        result += "\\t";
        break;
      default:
        if (character < 0x20) {
          char buffer[8];
          std::snprintf(buffer, sizeof(buffer), "\\u%04x", character);
          result += buffer;
        } else {
          result.push_back(static_cast<char>(character));
        }
        break;
    }
  }
  result += "\"";
  return result;
}

std::string JsonNumber(double value) {
  if (!std::isfinite(value)) {
    return "null";
  }
  char buffer[64];
  std::snprintf(buffer, sizeof(buffer), "%.10g", value);
  return buffer;
}

std::string JsonBoolean(bool value) {
  return value ? "true" : "false";
}

std::string JsonNumberArray(const double* values, size_t count) {
  std::string result = "[";
  for (size_t index = 0; index < count; ++index) {
    if (index > 0) {
      result += ",";
    }
    result += JsonNumber(values[index]);
  }
  result += "]";
  return result;
}

// The metadata of the prepared source, as a JSON object the page can read
// without a parser of its own.
std::string BuildMetadataJson(const WasmSession& session) {
  const SourceLinearDngMetadata& metadata = session.prepared.metadata;
  std::string json = "{";
  json += "\"make\":" + JsonString(metadata.make);
  json += ",\"model\":" + JsonString(metadata.model);
  json += ",\"unique_camera_model\":" + JsonString(metadata.unique_camera_model);
  json += ",\"flip\":" + JsonNumber(metadata.libraw_flip);
  json += ",\"width\":" + JsonNumber(session.prepared.image_width);
  json += ",\"height\":" + JsonNumber(session.prepared.image_height);
  json += ",\"has_black_level\":" + JsonBoolean(metadata.has_black_level);
  json += ",\"black_level\":" + JsonNumber(metadata.black_level);
  json += ",\"has_white_level\":" + JsonBoolean(metadata.has_white_level);
  json += ",\"white_level\":" + JsonNumber(metadata.white_level);
  json += ",\"has_as_shot_neutral\":" + JsonBoolean(metadata.has_as_shot_neutral);
  json += ",\"as_shot_neutral\":" +
          JsonNumberArray(metadata.as_shot_neutral, 3);
  json += ",\"has_color_matrix1\":" + JsonBoolean(metadata.has_color_matrix1);
  json += ",\"color_matrix1\":" + JsonNumberArray(metadata.color_matrix1, 9);
  json += ",\"has_rgb_cam\":" + JsonBoolean(metadata.has_rgb_cam);
  json += ",\"rgb_cam\":" + JsonNumberArray(metadata.rgb_cam, 9);
  json += ",\"has_exif_iso\":" + JsonBoolean(metadata.has_exif_iso);
  json += ",\"exif_iso\":" + JsonNumber(metadata.exif_iso);
  json += ",\"has_exif_shutter_speed\":" + JsonBoolean(metadata.has_exif_shutter_speed);
  json += ",\"exif_shutter_speed\":" + JsonNumber(metadata.exif_shutter_speed);
  json += ",\"has_exif_aperture\":" + JsonBoolean(metadata.has_exif_aperture);
  json += ",\"exif_aperture\":" + JsonNumber(metadata.exif_aperture);
  json += ",\"has_working_geometry\":" + JsonBoolean(metadata.has_working_geometry);
  json += ",\"working_width\":" + JsonNumber(metadata.working_width);
  json += ",\"working_height\":" + JsonNumber(metadata.working_height);
  json += ",\"has_default_crop\":" + JsonBoolean(metadata.has_default_crop);
  json += ",\"default_crop\":[" + JsonNumber(metadata.default_crop_origin_h) + "," +
          JsonNumber(metadata.default_crop_origin_v) + "," + JsonNumber(metadata.default_crop_width) +
          "," + JsonNumber(metadata.default_crop_height) + "]";
  json += ",\"has_highlight_recovery\":" +
          JsonBoolean(session.prepared.HasHighlightRecoverySource() &&
                      session.prepared.enable_highlight_recovery);
  json += "}";
  return json;
}

// Scales a preview down so its longest edge is at most `max_dimension`.
// The sampling matches the native interface, which picks the nearest source
// pixel for every target pixel.
void ScalePreview(const PreviewImage& source, uint32_t max_dimension, PreviewImage* target) {
  if (max_dimension == 0 || source.width == 0 || source.height == 0 || source.colors == 0) {
    return;
  }
  const uint32_t longest_edge = std::max(source.width, source.height);
  if (longest_edge <= max_dimension) {
    *target = source;
    return;
  }

  const double scale = static_cast<double>(max_dimension) / longest_edge;
  target->width = std::max(1u, static_cast<uint32_t>(std::lround(source.width * scale)));
  target->height = std::max(1u, static_cast<uint32_t>(std::lround(source.height * scale)));
  target->colors = source.colors;
  target->bits = source.bits;
  target->pixels.resize(static_cast<size_t>(target->width) * target->height * source.colors);

  for (uint32_t row = 0; row < target->height; ++row) {
    const uint32_t source_row = std::min<uint32_t>(
        source.height - 1,
        static_cast<uint32_t>((static_cast<uint64_t>(row) * source.height) / target->height));
    for (uint32_t column = 0; column < target->width; ++column) {
      const uint32_t source_column = std::min<uint32_t>(
          source.width - 1,
          static_cast<uint32_t>((static_cast<uint64_t>(column) * source.width) / target->width));
      const size_t source_index =
          (static_cast<size_t>(source_row) * source.width + source_column) * source.colors;
      const size_t target_index =
          (static_cast<size_t>(row) * target->width + column) * source.colors;
      std::memcpy(target->pixels.data() + target_index,
                  source.pixels.data() + source_index,
                  source.colors);
    }
  }
}

// The page paints with the canvas, which wants RGBA.
void CopyPreviewToRgba(const PreviewImage& preview, std::vector<uint8_t>* out) {
  if (preview.width == 0 || preview.height == 0 || preview.colors < 3) {
    return;
  }
  const size_t pixel_count = static_cast<size_t>(preview.width) * preview.height;
  out->assign(pixel_count * 4, 255);
  for (size_t index = 0; index < pixel_count; ++index) {
    const size_t source = index * preview.colors;
    const size_t target = index * 4;
    (*out)[target + 0] = preview.pixels[source + 0];
    (*out)[target + 1] = preview.pixels[source + 1];
    (*out)[target + 2] = preview.pixels[source + 2];
    (*out)[target + 3] = 255;
  }
}

bool HandOutBuffer(const std::vector<uint8_t>& data, uint8_t** out_buffer, size_t* out_size) {
  uint8_t* buffer = static_cast<uint8_t*>(std::malloc(data.size()));
  if (buffer == nullptr) {
    return false;
  }
  std::memcpy(buffer, data.data(), data.size());
  *out_buffer = buffer;
  *out_size = data.size();
  return true;
}

}  // namespace

extern "C" {

int hiraco_wasm_is_dng_sdk_enabled() {
  return GetDngSdkSupportSummary().enabled ? 1 : 0;
}

const char* hiraco_wasm_get_dng_status() {
  static const std::string status = GetDngSdkSupportSummary().status;
  return status.c_str();
}

int hiraco_wasm_get_writer_summary(char* out_buffer, size_t out_buffer_size,
                                  const char* compression) {
  if (out_buffer == nullptr || out_buffer_size == 0) {
    return 0;
  }

  const std::string requested = compression != nullptr ? compression : "deflate";
  const DngWriterConfigSummary summary = BuildDngWriterConfigSummary(requested);
  const std::string message = summary.enabled ? summary.status : "Adobe DNG SDK integration not enabled in this build";

  const size_t available = std::min<size_t>(message.size(), out_buffer_size - 1U);
  std::memcpy(out_buffer, message.c_str(), available);
  out_buffer[available] = '\0';
  return static_cast<int>(available);
}

int hiraco_wasm_convert(const uint8_t* raw_bytes, size_t raw_size,
                        const char* input_name, const char* compression,
                        uint8_t** out_dng, size_t* out_size) {
  LastError().clear();
  if (out_dng == nullptr || out_size == nullptr) {
    LastError() = "no output buffer was provided";
    return 1;
  }
  *out_dng = nullptr;
  *out_size = 0;
  if (raw_bytes == nullptr && raw_size > 0) {
    LastError() = "no input data was provided";
    return 1;
  }

  const std::string input_path = StagedInputPath(input_name);
  if (!WriteWholeFile(input_path, raw_bytes, raw_size)) {
    LastError() = "could not stage the input file in the virtual file system";
    return 1;
  }

  HiracoCompression mode = HiracoCompression::kDeflate;
  const std::string requested = compression != nullptr ? compression : "deflate";
  if (!ParseCompressionString(requested, &mode)) {
    LastError() = "unsupported compression mode: " + requested;
    return 1;
  }

  PreparedSource prepared;
  std::string error_message;
  if (!PrepareSource(input_path, &prepared, &error_message)) {
    LastError() = error_message.empty() ? "could not read the input file" : error_message;
    return 1;
  }

  // The highlight recovery companion cannot be discovered in a browser
  // because it is a separate file next to the source, so that stage stays
  // off here; the command line tool enables it when it finds the companion.

  const DngWriteResult result = ConvertToDng(prepared,
                                             std::filesystem::path(kOutputPath),
                                             mode,
                                             StageOverrideSet(),
                                             LibRawOverrideSet());
  if (!result.ok) {
    LastError() = result.message.empty() ? "the DNG could not be written" : result.message;
    return 1;
  }

  std::vector<uint8_t> dng;
  if (!ReadWholeFile(kOutputPath, &dng)) {
    LastError() = "the DNG could not be read back from the virtual file system";
    return 1;
  }
  if (dng.empty()) {
    LastError() = "the DNG writer produced an empty file";
    return 1;
  }

  // The buffer is handed to the caller and released with
  // hiraco_wasm_free_buffer(), so it is allocated the same way.
  uint8_t* buffer = static_cast<uint8_t*>(std::malloc(dng.size()));
  if (buffer == nullptr) {
    LastError() = "out of memory";
    return 1;
  }
  std::memcpy(buffer, dng.data(), dng.size());
  *out_dng = buffer;
  *out_size = dng.size();
  return 0;
}

void hiraco_wasm_free_buffer(uint8_t* buffer, size_t size) {
  (void)size;
  std::free(buffer);
}

const char* hiraco_wasm_get_last_error() {
  return LastError().c_str();
}

uint32_t hiraco_wasm_open_session(const uint8_t* raw_bytes, size_t raw_size,
                                   const char* input_name,
                                   const uint8_t* companion_bytes, size_t companion_size,
                                   const char* companion_name) {
  LastError().clear();
  if (raw_bytes == nullptr && raw_size > 0) {
    LastError() = "no input data was provided";
    return 0;
  }

  const std::string input_path = StagedInputPath(input_name);
  if (!WriteWholeFile(input_path, raw_bytes, raw_size)) {
    LastError() = "could not stage the input file in the virtual file system";
    return 0;
  }

  std::string companion_path;
  if (companion_bytes != nullptr && companion_size > 0) {
    const char* companion_file_name =
        companion_name != nullptr && companion_name[0] != '\0' ? companion_name : "companion.ORI";
    companion_path = StagedInputPath(companion_file_name);
    if (!WriteWholeFile(companion_path, companion_bytes, companion_size)) {
      LastError() = "could not stage the highlight recovery companion in the virtual file system";
      return 0;
    }
  }

  auto cancel_requested = std::make_shared<std::atomic<bool>>(false);
  const CancelCheck cancel = [cancel_requested]() { return cancel_requested->load(); };

  PreparedSource prepared;
  std::string error_message;
  if (!PrepareSource(input_path, &prepared, &error_message, ReportProgress, cancel)) {
    LastError() = error_message.empty() ? "could not read the input file" : error_message;
    return 0;
  }

  if (!companion_path.empty()) {
    // A browser cannot rely on the core finding a companion next to the
    // source, because the page picks the file explicitly and it may carry
    // any name.  The path is set here and the recovery is turned on with
    // it, the same way the native interface does it.
    prepared.highlight_recovery_source_path = companion_path;
    prepared.enable_highlight_recovery = true;
  }

  const uint32_t session_id = NextSessionId()++;
  WasmSession session;
  session.prepared = std::move(prepared);
  session.cancel_requested = cancel_requested;
  session.output_path =
      std::string(kInputDirectory) + "/output-" + std::to_string(session_id) + ".dng";
  Sessions().emplace(session_id, std::move(session));
  return session_id;
}

void hiraco_wasm_close_session(uint32_t session_id) {
  Sessions().erase(session_id);
}

int hiraco_wasm_get_image_size(uint32_t session_id, uint32_t* out_width, uint32_t* out_height) {
  LastError().clear();
  WasmSession* session = FindSession(session_id);
  if (session == nullptr) {
    LastError() = "unknown session";
    return 1;
  }
  if (out_width != nullptr) {
    *out_width = session->prepared.image_width;
  }
  if (out_height != nullptr) {
    *out_height = session->prepared.image_height;
  }
  return 0;
}

int hiraco_wasm_get_metadata_json(uint32_t session_id, char* out_buffer, size_t out_buffer_size) {
  LastError().clear();
  if (out_buffer == nullptr || out_buffer_size == 0) {
    return 0;
  }
  WasmSession* session = FindSession(session_id);
  if (session == nullptr) {
    LastError() = "unknown session";
    return 0;
  }

  const std::string json = BuildMetadataJson(*session);
  const size_t available = std::min<size_t>(json.size(), out_buffer_size - 1U);
  std::memcpy(out_buffer, json.c_str(), available);
  out_buffer[available] = '\0';
  return static_cast<int>(available);
}

int hiraco_wasm_set_highlight_recovery(uint32_t session_id, int enabled) {
  LastError().clear();
  WasmSession* session = FindSession(session_id);
  if (session == nullptr) {
    LastError() = "unknown session";
    return 1;
  }
  session->prepared.enable_highlight_recovery =
      enabled != 0 && session->prepared.HasHighlightRecoverySource();
  return 0;
}

int hiraco_wasm_get_original_preview(uint32_t session_id, uint32_t max_dimension,
                                     uint8_t** out_pixels, size_t* out_size,
                                     uint32_t* out_width, uint32_t* out_height) {
  LastError().clear();
  if (out_pixels == nullptr || out_size == nullptr) {
    LastError() = "no output buffer was provided";
    return 1;
  }
  *out_pixels = nullptr;
  *out_size = 0;

  WasmSession* session = FindSession(session_id);
  if (session == nullptr) {
    LastError() = "unknown session";
    return 1;
  }

  session->cancel_requested->store(false);
  std::shared_ptr<PreviewImage> preview = std::make_shared<PreviewImage>();
  std::string error_message;
  if (!RenderOriginalPreview(&session->prepared,
                             preview,
                             LibRawOverrideSet(),
                             ReportProgress,
                             [session]() { return session->cancel_requested->load(); },
                             &error_message)) {
    LastError() = error_message.empty() ? "the original preview could not be rendered" : error_message;
    return 1;
  }

  // A max_dimension of 0 means no limit: return the full-resolution preview.
  // Otherwise scale down so the longest edge is at most max_dimension.
  PreviewImage output;
  if (max_dimension != 0) {
    ScalePreview(*preview, max_dimension, &output);
  } else {
    output = *preview;
  }
  std::vector<uint8_t> rgba;
  CopyPreviewToRgba(output, &rgba);
  if (rgba.empty()) {
    LastError() = "the original preview is empty";
    return 1;
  }
  if (!HandOutBuffer(rgba, out_pixels, out_size)) {
    LastError() = "out of memory";
    return 1;
  }
  if (out_width != nullptr) {
    *out_width = output.width;
  }
  if (out_height != nullptr) {
    *out_height = output.height;
  }
  return 0;
}

int hiraco_wasm_get_converted_preview(uint32_t session_id, uint32_t max_dimension,
                                      const double* stage_overrides, size_t stage_override_count,
                                      uint8_t** out_pixels, size_t* out_size,
                                      uint32_t* out_width, uint32_t* out_height,
                                      uint8_t** out_mask, size_t* out_mask_size) {
  LastError().clear();
  if (out_pixels == nullptr || out_size == nullptr) {
    LastError() = "no output buffer was provided";
    return 1;
  }
  *out_pixels = nullptr;
  *out_size = 0;
  if (out_mask != nullptr) {
    *out_mask = nullptr;
  }
  if (out_mask_size != nullptr) {
    *out_mask_size = 0;
  }

  WasmSession* session = FindSession(session_id);
  if (session == nullptr) {
    LastError() = "unknown session";
    return 1;
  }

  StageOverrideSet overrides;
  BuildStageOverrideSet(stage_overrides, stage_override_count, &overrides);

  session->cancel_requested->store(false);
  std::shared_ptr<PreviewImage> preview = std::make_shared<PreviewImage>();
  std::string error_message;
  if (!RenderConvertedFullPreview(&session->prepared,
                                  overrides,
                                  max_dimension,
                                  preview,
                                  LibRawOverrideSet(),
                                  ReportProgress,
                                  [session]() { return session->cancel_requested->load(); },
                                  &error_message)) {
    LastError() = error_message.empty() ? "the converted preview could not be rendered" : error_message;
    return 1;
  }

  std::vector<uint8_t> rgba;
  CopyPreviewToRgba(*preview, &rgba);
  if (rgba.empty()) {
    LastError() = "the converted preview is empty";
    return 1;
  }
  if (!HandOutBuffer(rgba, out_pixels, out_size)) {
    LastError() = "out of memory";
    return 1;
  }
  if (out_width != nullptr) {
    *out_width = preview->width;
  }
  if (out_height != nullptr) {
    *out_height = preview->height;
  }

  if (out_mask != nullptr && out_mask_size != nullptr && !preview->recovered_mask.empty()) {
    if (!HandOutBuffer(preview->recovered_mask, out_mask, out_mask_size)) {
      LastError() = "out of memory";
      return 1;
    }
  }
  return 0;
}

int hiraco_wasm_get_resolved_stage_settings(uint32_t session_id,
                                            const double* stage_overrides, size_t stage_override_count,
                                            double* out_values, size_t out_value_count) {
  LastError().clear();
  if (out_values == nullptr || out_value_count < kStageValueCount) {
    LastError() = "no output buffer was provided";
    return 1;
  }

  WasmSession* session = FindSession(session_id);
  if (session == nullptr) {
    LastError() = "unknown session";
    return 1;
  }

  StageOverrideSet overrides;
  BuildStageOverrideSet(stage_overrides, stage_override_count, &overrides);
  const ResolvedStageSettings settings = GetResolvedStageSettings(session->prepared, overrides);
  CopyResolvedStageSettings(settings, out_values);
  return 0;
}

int hiraco_wasm_convert_session(uint32_t session_id, const char* compression,
                                const double* stage_overrides, size_t stage_override_count,
                                uint8_t** out_dng, size_t* out_size) {
  LastError().clear();
  if (out_dng == nullptr || out_size == nullptr) {
    LastError() = "no output buffer was provided";
    return 1;
  }
  *out_dng = nullptr;
  *out_size = 0;

  WasmSession* session = FindSession(session_id);
  if (session == nullptr) {
    LastError() = "unknown session";
    return 1;
  }

  HiracoCompression mode = HiracoCompression::kDeflate;
  const std::string requested = compression != nullptr ? compression : "deflate";
  if (!ParseCompressionString(requested, &mode)) {
    LastError() = "unsupported compression mode: " + requested;
    return 1;
  }

  StageOverrideSet overrides;
  BuildStageOverrideSet(stage_overrides, stage_override_count, &overrides);

  session->cancel_requested->store(false);
  const DngWriteResult result = ConvertToDng(session->prepared,
                                             std::filesystem::path(session->output_path),
                                             mode,
                                             overrides,
                                             LibRawOverrideSet(),
                                             ReportProgress,
                                             [session]() { return session->cancel_requested->load(); });
  if (!result.ok) {
    LastError() = result.message.empty() ? "the DNG could not be written" : result.message;
    return 1;
  }

  std::vector<uint8_t> dng;
  if (!ReadWholeFile(session->output_path, &dng)) {
    LastError() = "the DNG could not be read back from the virtual file system";
    return 1;
  }
  if (dng.empty()) {
    LastError() = "the DNG writer produced an empty file";
    return 1;
  }
  if (!HandOutBuffer(dng, out_dng, out_size)) {
    LastError() = "out of memory";
    return 1;
  }
  return 0;
}

void hiraco_wasm_request_cancel(uint32_t session_id) {
  WasmSession* session = FindSession(session_id);
  if (session == nullptr) {
    return;
  }
  session->cancel_requested->store(true);
}

}  // extern "C"
