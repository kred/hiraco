#include "hiraco_core.h"

#include <cstdlib>
#include <filesystem>
#include <iostream>
#include <set>
#include <string>

namespace {

int PrintUsage() {
  std::cerr << "usage: hiraco-cli convert <source> <output> [--compression uncompressed|deflate|jpeg-xl] [--debug]\n";
  std::cerr << "default compression: deflate\n";
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

  for (int index = 4; index < argc; ++index) {
    const std::string arg = argv[index];
    if (arg == "--compression" && index + 1 < argc) {
      compression_text = argv[++index];
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

  const StageOverrideSet stage_overrides = ReadStageOverridesFromEnvironment();
  const LibRawOverrideSet libraw_overrides = ReadLibRawOverridesFromEnvironment();
  const DngWriteResult result = ConvertToDng(prepared,
                                             std::filesystem::path(output_path),
                                             compression,
                                             stage_overrides,
                                             libraw_overrides);
  if (!result.ok) {
    std::cerr << "Error: " << result.message << "\n";
    return 1;
  }

  std::cout << "Success: " << output_path << "\n";
  return 0;
}
