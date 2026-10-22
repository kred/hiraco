// halide_pipeline_check.cpp  —  Exercises the two Halide AOT pipelines and
// writes their results to files.
//
// The WebAssembly build compiles the Halide pipelines differently from the
// native build: Halide's own `wasmrt` backend only supports wasm32, so the
// WebAssembly build asks the generators for C source and compiles that source
// with the Emscripten toolchain, while the native build links the object file
// Halide itself produced.  The two therefore use different code generators and
// different vector widths, so their results differ in the last floating point
// bits.
//
// This program produces the numbers for both builds so the difference can be
// measured instead of assumed.
//
// Usage:
//   hiraco-halide-check <wavelet-out.bin> <guided-out.bin>
#include "hiraco_atrous_wavelet.h"
#include "hiraco_guided_filter.h"

#include <HalideBuffer.h>

#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <vector>

namespace {

constexpr int kWidth = 97;
constexpr int kHeight = 61;

void FillInput(std::vector<double>* values) {
  values->resize(static_cast<size_t>(kWidth) * kHeight);
  for (int y = 0; y < kHeight; ++y) {
    for (int x = 0; x < kWidth; ++x) {
      (*values)[static_cast<size_t>(y) * kWidth + x] =
          std::sin(0.1 * x) * std::cos(0.07 * y) + 0.001 * (x * y);
    }
  }
}

bool WriteFile(const char* path, const std::vector<double>& values) {
  FILE* handle = fopen(path, "wb");
  if (handle == nullptr) {
    fprintf(stderr, "cannot open %s for writing\n", path);
    return false;
  }
  const size_t written = fwrite(values.data(), sizeof(double), values.size(), handle);
  fclose(handle);
  return written == values.size();
}

}  // namespace

int main(int argc, char** argv) {
  if (argc < 3) {
    fprintf(stderr, "usage: %s <wavelet-out.bin> <guided-out.bin>\n", argv[0]);
    return 2;
  }

  std::vector<double> input;
  FillInput(&input);

  // --- atrous wavelet ---
  std::vector<double> wavelet(kWidth * kHeight, 0.0);
  {
    Halide::Runtime::Buffer<double> in_buf(input.data(), kWidth, kHeight);
    Halide::Runtime::Buffer<double> out_buf(wavelet.data(), kWidth, kHeight);
    const int status = hiraco_atrous_wavelet(in_buf, 1, out_buf);
    if (status != 0) {
      fprintf(stderr, "hiraco_atrous_wavelet failed with %d\n", status);
      return 1;
    }
  }
  if (!WriteFile(argv[1], wavelet)) {
    return 1;
  }

  // --- guided filter ---
  std::vector<double> confidence(kWidth * kHeight, 0.0);
  for (size_t i = 0; i < confidence.size(); ++i) {
    confidence[i] = 0.25 + 0.5 * std::fabs(std::sin(0.03 * static_cast<double>(i)));
  }
  std::vector<double> guided(kWidth * kHeight, 0.0);
  {
    Halide::Runtime::Buffer<double> luma_buf(input.data(), kWidth, kHeight);
    Halide::Runtime::Buffer<double> conf_buf(confidence.data(), kWidth, kHeight);
    Halide::Runtime::Buffer<double> out_buf(guided.data(), kWidth, kHeight);
    const int status = hiraco_guided_filter(luma_buf, conf_buf, 4, 1e-6, 1.25, out_buf);
    if (status != 0) {
      fprintf(stderr, "hiraco_guided_filter failed with %d\n", status);
      return 1;
    }
  }
  if (!WriteFile(argv[2], guided)) {
    return 1;
  }

  printf("ok: wavelet[0]=%.17g guided[0]=%.17g\n", wavelet[0], guided[0]);
  return 0;
}
