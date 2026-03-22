#include "dng_jxl.h"

#include "dng_exceptions.h"
#include "dng_image.h"
#include "dng_pixel_buffer.h"

bool ParseJXL(dng_host&, dng_stream&, dng_info&, bool, bool) {
  ThrowNotYetImplemented("JPEG XL parsing is stubbed out in the non-JXL build path");
  return false;
}

dng_jxl_decoder::~dng_jxl_decoder() = default;

void dng_jxl_decoder::Decode(dng_host&, dng_stream&) {
  ThrowNotYetImplemented("JPEG XL decoding is stubbed out in the non-JXL build path");
}

void dng_jxl_decoder::ProcessExifBox(dng_host&, const std::vector<uint8>&) {
  ThrowNotYetImplemented("JPEG XL EXIF box processing is stubbed out in the non-JXL build path");
}

void dng_jxl_decoder::ProcessXMPBox(dng_host&, const std::vector<uint8>&) {
  ThrowNotYetImplemented("JPEG XL XMP box processing is stubbed out in the non-JXL build path");
}

void dng_jxl_decoder::ProcessBox(dng_host&, const dng_string&, const std::vector<uint8>&) {
  ThrowNotYetImplemented("JPEG XL box processing is stubbed out in the non-JXL build path");
}

void EncodeJXL_Tile(dng_host&, dng_stream&, const dng_pixel_buffer&, const dng_jxl_color_space_info&, const dng_jxl_encode_settings&) {
  ThrowNotYetImplemented("JPEG XL tile encoding is stubbed out in the non-JXL build path");
}

void EncodeJXL_Tile(dng_host&, dng_stream&, const dng_image&, const dng_jxl_color_space_info&, const dng_jxl_encode_settings&) {
  ThrowNotYetImplemented("JPEG XL tile encoding is stubbed out in the non-JXL build path");
}

void EncodeJXL_Container(dng_host&, dng_stream&, const dng_image&, const dng_jxl_encode_settings&, const dng_jxl_color_space_info&, const dng_metadata*, const bool, const bool, const bool, const dng_bmff_box_list*) {
  ThrowNotYetImplemented("JPEG XL container encoding is stubbed out in the non-JXL build path");
}

void EncodeJXL_Container(dng_host&, dng_stream&, const dng_pixel_buffer&, const dng_jxl_encode_settings&, const dng_jxl_color_space_info&, const dng_metadata*, const bool, const bool, const bool, const dng_bmff_box_list*) {
  ThrowNotYetImplemented("JPEG XL container encoding is stubbed out in the non-JXL build path");
}

real32 JXLQualityToDistance(uint32) {
  ThrowNotYetImplemented("JPEG XL quality mapping is stubbed out in the non-JXL build path");
  return 0.0f;
}

dng_jxl_encode_settings* JXLQualityToSettings(uint32) {
  ThrowNotYetImplemented("JPEG XL quality mapping is stubbed out in the non-JXL build path");
  return nullptr;
}

void PreviewColorSpaceToJXLEncoding(const PreviewColorSpaceEnum, const uint32, dng_jxl_color_space_info&) {
  ThrowNotYetImplemented("JPEG XL color encoding is stubbed out in the non-JXL build path");
}

bool SupportsJXL(const dng_image&) {
  return false;
}