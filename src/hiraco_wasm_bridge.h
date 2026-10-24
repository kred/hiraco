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

/// Releases a buffer handed out by hiraco_wasm_convert() or by one of the
/// session calls below.
void hiraco_wasm_free_buffer(uint8_t* buffer, size_t size);

/// The message describing the most recent failure, as a static string.
const char* hiraco_wasm_get_last_error(void);

/// The number of stage settings the bridge exchanges with its caller.
///
/// The order matches `StageOverrideSet` in src/hiraco_types.h:
///   0 stage1_psf_sigma   1 stage1_nsr
///   2 stage2_denoise     3 stage2_gain1   4 stage2_gain2   5 stage2_gain3
///   6 stage3_radius      7 stage3_gain
///
/// A value of NaN means "not overridden".  That lets a caller send the
/// whole set without having to know which entries differ from the base,
/// and it keeps the bridge free of a JSON parser.
#define HIRACO_WASM_STAGE_VALUE_COUNT 8

/// Prepares a RAW file for the preview and conversion calls below and
/// returns a session handle, or 0 if the file could not be prepared.
///
/// `raw_bytes` holds `raw_size` bytes of RAW data and `input_name` is the
/// name the data came from; only the file name is used, so a path from the
/// caller cannot escape the staging directory.  When `companion_bytes` is
/// given, it holds `companion_size` bytes of the optional ORI companion that
/// is used for highlight recovery, and `companion_name` is its file name.
///
/// The companion is optional: without it the session behaves exactly like
/// the command line tool running without highlight recovery.
uint32_t hiraco_wasm_open_session(const uint8_t* raw_bytes, size_t raw_size,
                                   const char* input_name,
                                   const uint8_t* companion_bytes, size_t companion_size,
                                   const char* companion_name);

/// Releases a session and everything it holds.  Unknown handles are ignored.
void hiraco_wasm_close_session(uint32_t session_id);

/// Writes the size of the prepared image into `out_width` and `out_height`.
/// Returns 0 on success and a non-zero value for an unknown session.
int hiraco_wasm_get_image_size(uint32_t session_id, uint32_t* out_width, uint32_t* out_height);

/// Writes the metadata of the prepared source as a JSON object into
/// `out_buffer`.  Returns the number of bytes written, excluding the
/// terminating null, or 0 if the buffer is unusable or the session is
/// unknown.
int hiraco_wasm_get_metadata_json(uint32_t session_id, char* out_buffer, size_t out_buffer_size);

/// Turns highlight recovery on or off for the session.  Returns 0 on
/// success.  The setting has no effect while the session holds no
/// companion file.
int hiraco_wasm_set_highlight_recovery(uint32_t session_id, int enabled);

/// Renders the original preview of the prepared source.
///
/// The result is an RGBA image whose longest edge is at most `max_dimension`
/// pixels; it is handed back as a buffer of `*out_size` bytes that the
/// caller releases with hiraco_wasm_free_buffer().  `out_width` and
/// `out_height` receive the size of the rendered image.
///
/// Returns 0 on success and a non-zero value on failure.
int hiraco_wasm_get_original_preview(uint32_t session_id, uint32_t max_dimension,
                                     uint8_t** out_pixels, size_t* out_size,
                                     uint32_t* out_width, uint32_t* out_height);

/// Renders the preview of the processed source, using the same pipeline the
/// conversion uses.
///
/// `stage_overrides` holds `stage_override_count` values in the order
/// described by HIRACO_WASM_STAGE_VALUE_COUNT; a NaN means "not overridden".
///
/// The result is an RGBA image whose longest edge is at most `max_dimension`
/// pixels, handed back as a buffer of `*out_size` bytes that the caller
/// releases with hiraco_wasm_free_buffer().  `out_mask` receives the
/// optional recovered-pixel overlay, one byte per pixel, where 1 marks a
/// pixel that highlight recovery changed; it is empty when no recovery ran.
///
/// Returns 0 on success and a non-zero value on failure.
int hiraco_wasm_get_converted_preview(uint32_t session_id, uint32_t max_dimension,
                                      const double* stage_overrides, size_t stage_override_count,
                                      uint8_t** out_pixels, size_t* out_size,
                                      uint32_t* out_width, uint32_t* out_height,
                                      uint8_t** out_mask, size_t* out_mask_size);

/// Writes the stage settings that the session would use for the given
/// overrides into `out_values`, in the order described by
/// HIRACO_WASM_STAGE_VALUE_COUNT.  `out_value_count` is the number of
/// values the caller provides.
///
/// Returns 0 on success and a non-zero value on failure.
int hiraco_wasm_get_resolved_stage_settings(uint32_t session_id,
                                            const double* stage_overrides, size_t stage_override_count,
                                            double* out_values, size_t out_value_count);

/// Converts the prepared source of the session into a Linear DNG.
///
/// `compression` is one of "uncompressed", "deflate" or "jpeg-xl" and
/// `stage_overrides` holds the stage overrides in the order described by
/// HIRACO_WASM_STAGE_VALUE_COUNT; a NaN means "not overridden".
///
/// On success 0 is returned, `*out_dng` points to a buffer of `*out_size`
/// bytes holding the complete DNG, and the caller must release it with
/// hiraco_wasm_free_buffer().
int hiraco_wasm_convert_session(uint32_t session_id, const char* compression,
                                const double* stage_overrides, size_t stage_override_count,
                                uint8_t** out_dng, size_t* out_size);

/// Asks the session to stop at the next cancellation point.
///
/// A browser cannot call into the module while a conversion is running, so
/// the web application aborts a running conversion by replacing the worker
/// that runs it.  This call is the cooperative half of that: it takes effect
/// at the cancellation points the conversion itself checks.
void hiraco_wasm_request_cancel(uint32_t session_id);

#ifdef __cplusplus
}
#endif
