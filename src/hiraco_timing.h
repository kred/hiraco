#pragma once

#include <chrono>
#include <cstdlib>
#include <iomanip>
#include <iostream>
#include <string>

namespace hiraco {

inline bool TimingLoggingEnabled() {
  static const bool enabled = []() {
    const char* raw = std::getenv("HIRACO_TIMING");
    if (raw == nullptr || *raw == '\0') {
      return false;
    }

    std::string value(raw);
    for (char& ch : value) {
      if (ch >= 'A' && ch <= 'Z') {
        ch = static_cast<char>(ch - 'A' + 'a');
      }
    }

    return value != "0" &&
           value != "false" &&
           value != "off" &&
           value != "no";
  }();

  return enabled;
}

inline void LogTiming(const std::string& pipeline,
                      const std::string& step,
                      std::chrono::steady_clock::duration elapsed,
                      const std::string& detail = {}) {
  if (!TimingLoggingEnabled()) {
    return;
  }

  const double elapsed_ms = std::chrono::duration<double, std::milli>(elapsed).count();
  const std::ios::fmtflags flags = std::cerr.flags();
  const std::streamsize precision = std::cerr.precision();

  std::cerr << std::fixed << std::setprecision(1)
            << "[hiraco][timing][" << pipeline << "] "
            << step << ": " << elapsed_ms << " ms";
  if (!detail.empty()) {
    std::cerr << " (" << detail << ")";
  }
  std::cerr << "\n";

  std::cerr.flags(flags);
  std::cerr.precision(precision);
}

class ScopedTimingLog {
 public:
  ScopedTimingLog(std::string pipeline,
                  std::string step,
                  std::string detail = {})
      : pipeline_(std::move(pipeline)),
        step_(std::move(step)),
        detail_(std::move(detail)),
        start_(std::chrono::steady_clock::now()),
        enabled_(TimingLoggingEnabled()) {}

  ~ScopedTimingLog() {
    if (!enabled_) {
      return;
    }
    LogTiming(pipeline_, step_, std::chrono::steady_clock::now() - start_, detail_);
  }

 private:
  std::string pipeline_;
  std::string step_;
  std::string detail_;
  std::chrono::steady_clock::time_point start_;
  bool enabled_ = false;
};

}  // namespace hiraco