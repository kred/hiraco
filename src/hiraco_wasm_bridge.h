// hiraco_wasm_bridge.h  —  The browser facing WebAssembly bridge.
#pragma once

#ifdef __cplusplus
extern "C" {
#endif

#include <stddef.h>
#include <stdint.h>

/// Reports whether the Adobe DNG SDK is compiled into this build.
int hiraco_wasm_is_dng_sdk_enabled(void);

/// A static string describing the DNG writer support of this build.
const char* hiraco_wasm_get_dng_status(void);

/// Writes a one line summary of the writer configuration for `compression`
/// into `out_buffer`.  Returns the number of bytes written, excluding the
/// terminating null, or 0 if the buffer is unusable.
int hiraco_wasm_get_writer_summary(char* out_buffer, size_t out_buffer_size,
                                  const char* compression);

/// Converts a RAW file to a Linear DNG entirely in memory.
///
/// `raw_bytes` holds `raw_size` bytes of RAW data and `input_name` is the
/// file name the data came from; it is used for the DNG's original file name
/// tag and to derive the file extension LibRaw needs.  `compression` is one of
/// "uncompressed", "deflate" or "jpeg-xl".
///
/// On success 0 is returned, `*out_dng` points to a buffer of `*out_size`
/// bytes holding the complete DNG, and the caller must release it with
/// hiraco_wasm_free_buffer().  On failure a non-zero value is returned and
/// hiraco_wasm_get_last_error() describes what went wrong.
int hiraco_wasm_convert(const uint8_t* raw_bytes, size_t raw_size,
                        const char* input_name, const char* compression,
                        uint8_t** out_dng, size_t* out_size);

/// Releases a buffer handed out by hiraco_wasm_convert().
void hiraco_wasm_free_buffer(uint8_t* buffer, size_t size);

/// The message describing the most recent failure, as a static string.
const char* hiraco_wasm_get_last_error(void);

#ifdef __cplusplus
}
#endif
