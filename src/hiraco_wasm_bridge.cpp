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
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <fstream>
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

}  // extern "C"
