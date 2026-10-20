#pragma once

#ifdef __cplusplus
extern "C" {
#endif

#include <stddef.h>

int hiraco_wasm_is_dng_sdk_enabled(void);
const char* hiraco_wasm_get_dng_status(void);
int hiraco_wasm_get_writer_summary(char* out_buffer, size_t out_buffer_size,
                                  const char* compression);

#ifdef __cplusplus
}
#endif
