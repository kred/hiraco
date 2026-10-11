#include "hiraco_core.h"

#include <cstdlib>
#include <filesystem>
#include <iomanip>
#include <iostream>
#include <mutex>
#include <set>
#include <sstream>
#include <string>

#include "hiraco_timing.h"

namespace {

int PrintUsage() {
  std::cerr << "usage: hiraco-cli convert <source> <output> [--compression uncompressed|deflate|jpeg-xl] [--hl-rec] [--debug]\n";
  std::cerr << "default compression: deflate\n";
  std::cerr << "--hl-rec enables ORI-assisted highlight recovery when a companion is detected\n";
  return 2;
}

}  // namespace

void EnableTimingLogsForDebug() {
#if defined(_WIN32)
  _putenv_s("HIRACO_TIMING", "1");
#else
  setenv("HIRACO_TIMING", "1", 1);
#endif
}

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
  std::string compression_text = "deflate";
  bool debug = false;
  bool hl_rec = false;

  for (int index = 4; index < argc; ++index) {
    const std::string arg = argv[index];
    if (arg == "--compression" && index + 1 < argc) {
      compression_text = argv[++index];
    } else if (arg == "--hl-rec") {
      hl_rec = true;
    } else if (arg == "--debug") {
      debug = true;
    } else {
      return PrintUsage();
    }
  }

  if (debug) {
    EnableTimingLogsForDebug();
  }

  HiracoCompression compression = HiracoCompression::kDeflate;
  if (!ParseCompressionString(compression_text, &compression)) {
    std::cerr << "Error: invalid compression mode: " << compression_text << "\n";
    return 2;
  }

  PreparedSource prepared;
  std::string error_message;
  if (!PrepareSource(source_path, &prepared, &error_message)) {
    std::cerr << "Error: " << error_message << "\n";
    return 1;
  }

  // ShouldUseHighlightRecovery() also requires a detected companion, so this is
  // a no-op for sources without one.
  prepared.enable_highlight_recovery = hl_rec;

  const StageOverrideSet stage_overrides = ReadStageOverridesFromEnvironment();
  const LibRawOverrideSet libraw_overrides = ReadLibRawOverridesFromEnvironment();
  // --debug surfaces the progress callback, which is the only way to observe that
  // the long JPEG-XL compression phase advances instead of appearing hung.
  // The callback is invoked from the DNG SDK's worker threads via Sniff(), so
  // format each line completely and emit it under the shared diagnostics mutex;
  // otherwise concurrent calls could interleave their << chains, or interleave
  // with the timing logs, and garble the log.
  const auto log_progress = [enabled = debug](const ProcessingProgress& update) {
    if (!enabled) {
      return;
    }
    std::ostringstream line;
    line << std::fixed << std::setprecision(3)
         << "[hiraco][progress][" << update.phase << "] "
         << update.fraction << " " << update.message << "\n";

    const std::lock_guard<std::mutex> lock(hiraco::DiagnosticsLogMutex());
    std::cerr << line.str();
  };

  const DngWriteResult result = ConvertToDng(prepared,
                                             std::filesystem::path(output_path),
                                             compression,
                                             stage_overrides,
                                             libraw_overrides,
                                             log_progress);
  if (!result.ok) {
    std::cerr << "Error: " << result.message << "\n";
    return 1;
  }

  std::cout << "Success: " << output_path << "\n";
  return 0;
}
