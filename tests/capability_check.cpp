// capability_check.cpp  —  Asserts that the DNG capability summaries describe
// what the running build can actually do.
//
// The summaries are what the browser bridge and the GUI show, so a build that
// claims a capability it does not have is worse than one that reports
// nothing.  This check starts the DNG runtime for real and compares what the
// summaries say with what the runtime does.
//
// It is built for both the native and the WebAssembly toolchains, because the
// two report the capability separately.
//
// Usage:
//   hiraco-capability-check
#include "dng_sdk_bridge.h"
#include "dng_writer_bridge.h"

#include <cstdio>
#include <iostream>
#include <string>

namespace {

int failures = 0;

void Expect(bool condition, const std::string& message) {
  if (condition) {
    return;
  }
  std::cerr << "capability check failed: " << message << "\n";
  ++failures;
}

}  // namespace

int main() {
  // --- The DNG SDK summary must describe a runtime that works. ---
  const DngSdkSupportSummary support = GetDngSdkSupportSummary();
  Expect(!support.status.empty(),
         "the DNG SDK summary should carry a status message");
  Expect(support.enabled,
         "the DNG SDK runtime should be usable in this build: " + support.status);

  // --- The summary's JPEG XL claim must match the runtime. ---
  std::string jpeg_xl_reason;
  const bool jpeg_xl_usable = CanWriteLinearDng("jpeg-xl", &jpeg_xl_reason);
  Expect(support.jpeg_xl_available == jpeg_xl_usable,
         std::string("the summary reports JPEG XL as ") +
             (support.jpeg_xl_available ? "available" : "unavailable") +
             " but the runtime says " + (jpeg_xl_usable ? "usable" : "unusable") +
             ": " + jpeg_xl_reason);

  // --- Every supported mode must be reported as configurable, and the
  // --- report must agree with the runtime probe for that mode.
  for (const char* mode : {"uncompressed", "deflate", "jpeg-xl"}) {
    const std::string name(mode);
    const DngWriterConfigSummary summary = BuildDngWriterConfigSummary(name);

    std::string reason;
    const bool usable = CanWriteLinearDng(name, &reason);
    Expect(usable, "the writer should be usable for " + name + ": " + reason);
    Expect(summary.enabled == usable,
           std::string("the summary for ") + name + " reports enabled=" +
               (summary.enabled ? "true" : "false") + " but the runtime says " +
               (usable ? "usable" : "unusable"));
    Expect(summary.can_configure_writer == usable,
           std::string("the summary for ") + name + " reports can_configure_writer=" +
               (summary.can_configure_writer ? "true" : "false") + " but the runtime says " +
               (usable ? "usable" : "unusable"));
    Expect(summary.compression_name == name,
           "the summary for " + name + " should report its own name, got " +
               summary.compression_name);
    Expect(!summary.status.empty(),
           "the summary for " + name + " should carry a status message");
  }

  // --- An unsupported mode must not be presented as configurable. ---
  const DngWriterConfigSummary unsupported = BuildDngWriterConfigSummary("nope");
  Expect(!unsupported.can_configure_writer,
         "an unsupported compression mode should not be configurable");
  Expect(!unsupported.enabled,
         "an unsupported compression mode should not report the writer as enabled");

  if (failures != 0) {
    std::cerr << failures << " capability check(s) failed\n";
    return 1;
  }
  std::cout << "hiraco capability check passed\n";
  return 0;
}
