#include "hiraco_wasm_bridge.h"

#include "dng_sdk_bridge.h"

#include <cstring>
#include <string>

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

}  // extern "C"
