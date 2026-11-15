#pragma once

// The WebAssembly pthread pool has to cover every runtime that can create
// threads at the same time: OpenMP, FFTW, Halide's std::thread workers and the
// DNG compressor.  Each of them uses HiracoComputeThreadCount(), and the
// Emscripten link keeps enough pool workers for three capped runtimes plus
// headroom.  Without the cap a high-core machine can ask for more workers than
// the pool holds; Emscripten then proxies creation to the module main thread,
// which is blocked inside the wasm call, and the preview stalls.
#include <algorithm>
#include <thread>

#if defined(__EMSCRIPTEN__) && defined(_OPENMP)
#include <omp.h>
#endif

namespace hiraco {

inline constexpr unsigned kWasmMaxComputeThreads = 12;
// The conversion's CFA-guide extraction runs on a std::async thread, so the
// process can have two OpenMP teams alive at once.  Keeping each team small
// leaves room in the fixed Emscripten pthread pool for FFTW and Halide.
inline constexpr unsigned kWasmOpenMpThreads = 6;

inline int ComputeThreadCount() {
  const unsigned hardware = std::thread::hardware_concurrency();
#if defined(__EMSCRIPTEN__)
  // OpenMP, FFTW and Halide each own a thread set, and every one of those
  // threads comes from the same Emscripten pthread pool.  Capping each set
  // keeps the required pool size bounded on high-core machines.
  return static_cast<int>(std::max(1u, std::min(hardware, kWasmMaxComputeThreads)));
#else
  return static_cast<int>(std::max(1u, hardware));
#endif
}

#if defined(__EMSCRIPTEN__)
#if defined(_OPENMP)
inline void InitializeComputeThreads() {
  static const bool initialized = [] {
    omp_set_num_threads(
        static_cast<int>(std::min<unsigned>(static_cast<unsigned>(ComputeThreadCount()),
                                            kWasmOpenMpThreads)));
    return true;
  }();
  (void)initialized;
}
#else
inline void InitializeComputeThreads() {}
#endif
#else
inline void InitializeComputeThreads() {}
#endif

}  // namespace hiraco
