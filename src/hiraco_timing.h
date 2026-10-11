#pragma once

#include <chrono>
#include <cstdlib>
#include <iomanip>
#include <iostream>
#include <mutex>
#include <sstream>
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

// One mutex guarding all --debug diagnostic output on std::cerr. Timing logs and
// the CLI progress callback are both reachable from SDK worker threads, so they
// must share a single lock to serialise whole lines. Owning it here keeps it a
// single instance across every translation unit (inline function-local static).
inline std::mutex& DiagnosticsLogMutex() {
  static std::mutex diagnostics_log_mutex;
  return diagnostics_log_mutex;
}

inline void LogTiming(const std::string& pipeline,
                      const std::string& step,
                      std::chrono::steady_clock::duration elapsed,
                      const std::string& detail = {}) {
  if (!TimingLoggingEnabled()) {
    return;
  }

  const double elapsed_ms = std::chrono::duration<double, std::milli>(elapsed).count();

  // Build the line in a private stream and emit it under the shared diagnostics
  // mutex, so timing output emitted from SDK worker threads cannot interleave
  // with the progress logger (or with another LogTiming call) mid-line. This
  // also avoids mutating std::cerr's format state, which the old code saved and
  // restored around every call and could clobber concurrently.
  std::ostringstream line;
  line << std::fixed << std::setprecision(1)
       << "[hiraco][timing][" << pipeline << "] "
       << step << ": " << elapsed_ms << " ms";
  if (!detail.empty()) {
    line << " (" << detail << ")";
  }
  line << "\n";

  const std::lock_guard<std::mutex> lock(DiagnosticsLogMutex());
  std::cerr << line.str();
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