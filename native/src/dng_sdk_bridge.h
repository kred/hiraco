#pragma once

#include <string>

struct DngSdkSupportSummary {
  bool enabled = false;
  std::string status;
  std::string sdk_root;
  std::string backward_version;
  bool jpeg_xl_available = false;
};

struct DngWriterConfigSummary {
  bool enabled = false;
  bool can_configure_writer = false;
  bool save_linear_dng = true;
  bool lossless_jpeg_xl = false;
  bool lossy_mosaic_jpeg_xl = false;
  unsigned compression_code = 0;
  std::string compression_name;
  std::string dng_version;
  bool experimental = false;
  std::string status;
};

DngSdkSupportSummary GetDngSdkSupportSummary();
DngWriterConfigSummary BuildDngWriterConfigSummary(const std::string& compression);
