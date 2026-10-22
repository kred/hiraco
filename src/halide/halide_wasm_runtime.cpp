// Halide runtime for the WebAssembly build.
//
// Halide 21 cannot emit WebAssembly for a 64-bit target: its `wasmrt`
// backend only supports wasm32.  The pipeline code it generates is
// nevertheless ordinary, target-neutral C++, so the WebAssembly build asks
// the Halide generators for C source and compiles that source with the
// Emscripten toolchain instead of linking a Halide-produced object file.
//
// That leaves the Halide runtime.  The generated source references a small,
// fixed set of runtime entry points; this file implements exactly those for
// the wasm64 target.  The semantics match the official runtime:
//
//   * halide_malloc / halide_free implement the alignment and padding
//     contract documented in HalideRuntime.h.
//   * halide_do_par_for runs the requested range of tasks.  Every task
//     computes an independent output, so splitting the range across threads
//     does not change the result.
//   * the halide_error_* family reports a diagnostic and aborts, exactly as
//     the official runtime does when no user error handler is installed.
//
// The official runtime is not used because it cannot be generated for a
// 64-bit WebAssembly target, and because the WebAssembly build needs the
// 64-bit address space: a 10386 x 7792 sensor-shift capture peaks at about
// 8 GB of resident memory in the native build.

#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <algorithm>
#include <atomic>
#include <thread>
#include <vector>

namespace {

// The alignment the official runtime uses on 64-bit x86, kept here because
// the WebAssembly build also wants room for 128-bit vector accesses.
constexpr size_t kHalideAlignment = 64;

// Bytes kept in front of the returned block so that reading 8 bytes before
// the start of a buffer is safe, as HalideRuntime.h requires.
constexpr size_t kHalidePrefix = 16;

int HalideWorkerCount() {
  const unsigned hardware = std::thread::hardware_concurrency();
  if (hardware <= 1) {
    return 1;
  }
  return static_cast<int>(std::min<unsigned>(hardware, 32u));
}

[[noreturn]] void HalideAbort(const char *message) {
  fprintf(stderr, "Halide error: %s\n", message != nullptr ? message : "(no message)");
  fflush(stderr);
  abort();
}

}  // namespace

extern "C" {

void *halide_malloc(void *user_context, size_t x) {
  (void)user_context;
  // Halide requires the returned size to be a multiple of the alignment.
  const size_t size = (x + kHalideAlignment - 1) & ~(kHalideAlignment - 1);
  // Room for the alignment slack, the prefix that holds the base pointer,
  // and a readable tail.
  const size_t total = size + kHalideAlignment + kHalidePrefix + 16;
  void *base = malloc(total);
  if (base == nullptr) {
    return nullptr;
  }
  const uintptr_t raw = reinterpret_cast<uintptr_t>(base) + kHalidePrefix;
  const uintptr_t aligned = (raw + kHalideAlignment - 1) & ~static_cast<uintptr_t>(kHalideAlignment - 1);
  void *result = reinterpret_cast<void *>(aligned);
  // Keep the base pointer just in front of the block so halide_free can
  // recover it.  Sixteen bytes back is still inside the allocation.
  memcpy(static_cast<char *>(result) - kHalidePrefix, &base, sizeof(base));
  return result;
}

void halide_free(void *user_context, void *ptr) {
  (void)user_context;
  if (ptr == nullptr) {
    return;
  }
  void *base = nullptr;
  memcpy(&base, static_cast<char *>(ptr) - kHalidePrefix, sizeof(base));
  free(base);
}

int halide_do_par_for(void *user_context,
                      int (*task)(void *, int, uint8_t *),
                      int min,
                      int size,
                      uint8_t *closure) {
  if (task == nullptr || size <= 0) {
    return 0;
  }

  const int workers = std::min(size, HalideWorkerCount());
  if (workers <= 1) {
    for (int index = min; index < min + size; ++index) {
      if (task(user_context, index, closure) != 0) {
        return 1;
      }
    }
    return 0;
  }

  // Split the range into contiguous chunks.  Halide's generated code gives
  // every task a disjoint slice of the output, so the split is invisible in
  // the result.
  std::atomic<int> failed(0);
  std::vector<std::thread> threads;
  threads.reserve(static_cast<size_t>(workers));
  const int chunk = (size + workers - 1) / workers;
  for (int worker = 0; worker < workers; ++worker) {
    const int low = min + worker * chunk;
    const int high = std::min(low + chunk, min + size);
    if (low >= high) {
      break;
    }
    threads.emplace_back([user_context, task, low, high, closure, &failed]() {
      for (int index = low; index < high; ++index) {
        if (task(user_context, index, closure) != 0) {
          failed.store(1);
          return;
        }
      }
    });
  }
  for (std::thread &thread : threads) {
    thread.join();
  }
  return failed.load() != 0 ? 1 : 0;
}

void halide_error(void *user_context, const char *message) {
  (void)user_context;
  HalideAbort(message);
}

int halide_error_out_of_memory(void *user_context) {
  (void)user_context;
  HalideAbort("Out of memory");
}

int halide_error_bad_type(void *user_context,
                          const char *func_name,
                          uint32_t type_given,
                          uint32_t correct_type) {
  (void)user_context;
  fprintf(stderr, "Halide error: %s: type mismatch, got 0x%x, expected 0x%x\n",
          func_name != nullptr ? func_name : "(unknown)",
          static_cast<unsigned>(type_given),
          static_cast<unsigned>(correct_type));
  fflush(stderr);
  abort();
  return -1;
}

int halide_error_bad_dimensions(void *user_context,
                                const char *func_name,
                                int32_t dimensions_given,
                                int32_t correct_dimensions) {
  (void)user_context;
  fprintf(stderr, "Halide error: %s: %d dimensions given, %d expected\n",
          func_name != nullptr ? func_name : "(unknown)",
          static_cast<int>(dimensions_given),
          static_cast<int>(correct_dimensions));
  fflush(stderr);
  abort();
  return -1;
}

int halide_error_access_out_of_bounds(void *user_context,
                                      const char *func_name,
                                      int dimension,
                                      int min_touched,
                                      int max_touched,
                                      int min_valid,
                                      int max_valid) {
  (void)user_context;
  fprintf(stderr,
          "Halide error: %s: out of bounds in dimension %d, touched [%d, %d], valid [%d, %d]\n",
          func_name != nullptr ? func_name : "(unknown)",
          dimension, min_touched, max_touched, min_valid, max_valid);
  fflush(stderr);
  abort();
  return -1;
}

int halide_error_buffer_allocation_too_large(void *user_context,
                                             const char *buffer_name,
                                             uint64_t allocation_size,
                                             uint64_t max_size) {
  (void)user_context;
  fprintf(stderr, "Halide error: buffer %s needs %llu bytes, maximum is %llu\n",
          buffer_name != nullptr ? buffer_name : "(unnamed)",
          static_cast<unsigned long long>(allocation_size),
          static_cast<unsigned long long>(max_size));
  fflush(stderr);
  abort();
  return -1;
}

int halide_error_buffer_extents_negative(void *user_context,
                                         const char *buffer_name,
                                         int dimension,
                                         int extent) {
  (void)user_context;
  fprintf(stderr, "Halide error: buffer %s has negative extent %d in dimension %d\n",
          buffer_name != nullptr ? buffer_name : "(unnamed)",
          extent, dimension);
  fflush(stderr);
  abort();
  return -1;
}

int halide_error_buffer_extents_too_large(void *user_context,
                                           const char *buffer_name,
                                           int64_t actual_size,
                                           int64_t max_size) {
  (void)user_context;
  fprintf(stderr, "Halide error: buffer %s is %lld bytes, maximum is %lld\n",
          buffer_name != nullptr ? buffer_name : "(unnamed)",
          static_cast<long long>(actual_size),
          static_cast<long long>(max_size));
  fflush(stderr);
  abort();
  return -1;
}

int halide_error_constraint_violated(void *user_context,
                                     const char *var,
                                     int val,
                                     const char *constrained_var,
                                     int constrained_val) {
  (void)user_context;
  fprintf(stderr, "Halide error: constraint violated: %s is %d, but %s is %d\n",
          var != nullptr ? var : "(unknown)", val,
          constrained_var != nullptr ? constrained_var : "(unknown)", constrained_val);
  fflush(stderr);
  abort();
  return -1;
}

int halide_error_buffer_argument_is_null(void *user_context, const char *buffer_name) {
  (void)user_context;
  fprintf(stderr, "Halide error: buffer %s is null\n",
          buffer_name != nullptr ? buffer_name : "(unnamed)");
  fflush(stderr);
  abort();
  return -1;
}

int halide_error_host_is_null(void *user_context, const char *func_name) {
  (void)user_context;
  fprintf(stderr, "Halide error: %s: host pointer is null\n",
          func_name != nullptr ? func_name : "(unknown)");
  fflush(stderr);
  abort();
  return -1;
}

int halide_error_device_dirty_with_no_device_support(void *user_context, const char *buffer_name) {
  (void)user_context;
  fprintf(stderr, "Halide error: buffer %s is device dirty but there is no device support\n",
          buffer_name != nullptr ? buffer_name : "(unnamed)");
  fflush(stderr);
  abort();
  return -1;
}

}  // extern "C"
