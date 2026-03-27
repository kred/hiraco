#pragma once

#include <cstdint>
#include <string>
#include <vector>

struct VendorMakerNoteResult {
  bool ok = false;
  std::string error;

  // TIFF Make and Model tags (read directly from IFD0 for exactness).
  std::string tiff_make;
  std::string tiff_model;

  // StackedImage raw numeric tag value (e.g. "11 12" for hand-held high res)
  bool has_stacked_image = false;
  std::string stacked_image_label;

  // UnknownBlock binary payloads extracted from MakerNote sub-IFDs.
  std::vector<uint8_t> unknown_block_1;
  std::vector<uint8_t> unknown_block_3;

  // Working dimensions extracted from UnknownBlock1 offset 23 (uint32 word).
  bool has_working_geometry = false;
  uint32_t working_width = 0;
  uint32_t working_height = 0;
};

// Parse an Vendor ORF file to extract MakerNote data needed for the
// hiraco conversion pipeline.  Reads the TIFF IFD chain to locate the
// MakerNote tag and navigates Vendor-specific sub-IFDs.
VendorMakerNoteResult ReadVendorMakerNote(const std::string& path);
