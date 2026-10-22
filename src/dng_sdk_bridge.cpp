// dng_sdk_bridge.cpp  —  Reports what the Adobe DNG SDK integration of this
// build can actually do.
//
// The Adobe DNG SDK is a mandatory dependency of the project, so these
// summaries are never about "is the SDK compiled in".  They are about
// whether the DNG runtime really comes up and can be configured for the
// requested compression mode, which is what the caller ultimately cares
// about.  A build whose runtime cannot start says so instead of claiming a
// capability it does not have.

#include "dng_sdk_bridge.h"

#include "dng_writer_bridge.h"

#include <sstream>

#include "dng_tag_values.h"

#ifndef HIRACO_DNG_SDK_ROOT
#define HIRACO_DNG_SDK_ROOT "unknown"
#endif

namespace {

// The Adobe DNG SDK derives the saved DNGVersion tag from the features actually used
// and never writes below the DNG 1.4 baseline, so these are the versions the
// writer really produces for each mode.
uint32_t DngVersionForCompression(const std::string& compression) {
  if (compression == "jpeg-xl") {
    return dngVersion_1_7_0_0;
  }
  return dngVersion_1_4_0_0;
}

std::string FormatDngVersion(uint32_t version) {
  std::ostringstream stream;
  stream << ((version >> 24) & 0xFF) << "."
         << ((version >> 16) & 0xFF) << "."
         << ((version >> 8) & 0xFF) << "."
         << (version & 0xFF);
  return stream.str();
}

// Probing the runtime starts a DNG host and asks it for a negative, which is
// not free, so each answer is remembered.
bool ProbeWriter(const std::string& compression, std::string* reason) {
  static std::string cached_compression;
  static bool cached_result = false;
  static std::string cached_reason;

  if (cached_compression != compression) {
    cached_result = CanWriteLinearDng(compression, &cached_reason);
    cached_compression = compression;
  }
  if (reason != nullptr) {
    *reason = cached_reason;
  }
  return cached_result;
}

}  // namespace

DngSdkSupportSummary GetDngSdkSupportSummary() {
  DngSdkSupportSummary summary;
  summary.sdk_root = HIRACO_DNG_SDK_ROOT;
  summary.backward_version = FormatDngVersion(dngVersion_SaveDefault);

  std::string reason;
  if (!ProbeWriter("deflate", &reason)) {
    summary.enabled = false;
    summary.status = "The Adobe DNG SDK is present but its runtime is not usable in this build: " + reason;
    return summary;
  }

  summary.enabled = true;
  std::string jpeg_xl_reason;
  summary.jpeg_xl_available = ProbeWriter("jpeg-xl", &jpeg_xl_reason);
  summary.status = summary.jpeg_xl_available
      ? "Adobe DNG SDK runtime enabled for linear DNG writing, including Deflate and JPEG XL compression paths"
      : "Adobe DNG SDK runtime enabled for linear DNG writing, but JPEG XL compression is not available: " +
            jpeg_xl_reason;
  return summary;
}

DngWriterConfigSummary BuildDngWriterConfigSummary(const std::string& compression) {
  DngWriterConfigSummary summary;

  // The mode can only be offered when the DNG runtime really comes up for
  // it, so the reported capability is measured rather than assumed.
  std::string reason;
  if (!ProbeWriter(compression, &reason)) {
    summary.enabled = false;
    summary.can_configure_writer = false;
    summary.compression_name = compression;
    summary.status = reason.empty()
        ? "The Adobe DNG SDK runtime is not usable in this build"
        : reason;
    return summary;
  }

  summary.enabled = true;
  summary.can_configure_writer = true;

  if (compression == "uncompressed") {
    summary.compression_code = ccUncompressed;
    summary.compression_name = "uncompressed";
    summary.dng_version = FormatDngVersion(DngVersionForCompression(compression));
    summary.status = "Writer can request uncompressed linear DNG output as DNG 1.4.0.0";
    return summary;
  }

  if (compression == "deflate") {
    summary.compression_code = ccDeflate;
    summary.compression_name = "deflate";
    summary.dng_version = FormatDngVersion(DngVersionForCompression(compression));
    summary.status = "Writer can request Deflate-compressed linear DNG output as DNG 1.4.0.0";
    return summary;
  }

  if (compression == "jpeg-xl") {
    summary.compression_code = ccJXL;
    summary.compression_name = "jpeg-xl";
    summary.dng_version = FormatDngVersion(DngVersionForCompression(compression));
    summary.experimental = true;
    summary.lossless_jpeg_xl = true;
    summary.status = "Writer can request lossless JPEG XL linear DNG output as experimental DNG 1.7.0.0";
    return summary;
  }

  summary.can_configure_writer = false;
  summary.status = "Unsupported compression requested for the current DNG writer";
  return summary;
}
