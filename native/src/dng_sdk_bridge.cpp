#include "dng_sdk_bridge.h"

#include <sstream>

#if defined(HIRACO_ENABLE_DNG_SDK) && HIRACO_ENABLE_DNG_SDK

#include "dng_tag_values.h"

#ifndef HIRACO_DNG_SDK_ROOT
#define HIRACO_DNG_SDK_ROOT "unknown"
#endif

namespace {

std::string FormatDngVersion(uint32_t version) {
  std::ostringstream stream;
  stream << ((version >> 24) & 0xFF) << "."
         << ((version >> 16) & 0xFF) << "."
         << ((version >> 8) & 0xFF) << "."
         << (version & 0xFF);
  return stream.str();
}

uint32_t DngVersionForCompression(const std::string& compression) {
  if (compression == "jpeg-xl") {
    return dngVersion_SaveDefault;
  }
  return dngVersion_1_6_0_0;
}

}  // namespace

DngSdkSupportSummary GetDngSdkSupportSummary() {
  DngSdkSupportSummary summary;
  summary.enabled = true;
  summary.status = "Adobe DNG SDK runtime enabled for linear DNG writing, including Deflate and JPEG XL compression paths";
  summary.sdk_root = HIRACO_DNG_SDK_ROOT;
  summary.backward_version = FormatDngVersion(dngVersion_SaveDefault);
  summary.jpeg_xl_available = ccJXL == 52546;
  return summary;
}

DngWriterConfigSummary BuildDngWriterConfigSummary(const std::string& compression) {
  DngWriterConfigSummary summary;
  summary.enabled = true;
  summary.can_configure_writer = true;

  if (compression == "uncompressed") {
    summary.compression_code = ccUncompressed;
    summary.compression_name = "uncompressed";
    summary.dng_version = FormatDngVersion(DngVersionForCompression(compression));
    summary.status = "Writer can request uncompressed linear DNG output as DNG 1.6.0.0";
    return summary;
  }

  if (compression == "deflate") {
    summary.compression_code = ccDeflate;
    summary.compression_name = "deflate";
    summary.dng_version = FormatDngVersion(DngVersionForCompression(compression));
    summary.status = "Writer can request Deflate-compressed linear DNG output as DNG 1.6.0.0";
    return summary;
  }

  if (compression == "jpeg-xl") {
    summary.compression_code = ccJXL;
    summary.compression_name = "jpeg-xl";
    summary.dng_version = FormatDngVersion(DngVersionForCompression(compression));
    summary.experimental = true;
    summary.lossless_jpeg_xl = true;
    summary.status = "Writer can request lossless JPEG XL linear DNG output as experimental DNG 1.7.1.0";
    return summary;
  }

  summary.can_configure_writer = false;
  summary.status = "Unsupported compression requested for the current DNG writer";
  return summary;
}

#else

DngSdkSupportSummary GetDngSdkSupportSummary() {
  DngSdkSupportSummary summary;
  summary.enabled = false;
  summary.status = "Adobe DNG SDK integration not enabled in this build";
  summary.sdk_root = "disabled";
  summary.backward_version = "unavailable";
  summary.jpeg_xl_available = false;
  return summary;
}

DngWriterConfigSummary BuildDngWriterConfigSummary(const std::string& compression) {
  DngWriterConfigSummary summary;
  summary.enabled = false;
  summary.compression_name = compression;
  summary.status = "Adobe DNG SDK integration not enabled in this build";
  return summary;
}

#endif