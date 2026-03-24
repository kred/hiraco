#include "dng_writer_bridge.h"

#if defined(HIRACO_ENABLE_DNG_SDK) && HIRACO_ENABLE_DNG_SDK

#include "dng_auto_ptr.h"
#include "dng_camera_profile.h"
#include "dng_exceptions.h"
#include "dng_file_stream.h"
#include "dng_host.h"
#include "dng_image.h"
#include "dng_image_writer.h"
#include "dng_matrix.h"
#include "dng_negative.h"
#include "dng_pixel_buffer.h"
#include "dng_preview.h"
#include "dng_tag_values.h"
#include "dng_xy_coord.h"

#include <algorithm>
#include <cstdlib>
#include <filesystem>
#include <libraw/libraw.h>
#include <memory>
#include <string>
#include <vector>

namespace {

struct RasterImage {
  uint32_t width = 0;
  uint32_t height = 0;
  uint32_t colors = 0;
  uint32_t bits = 0;
  std::vector<uint16_t> pixels;
};

struct PreviewImage {
  uint32_t width = 0;
  uint32_t height = 0;
  uint32_t colors = 0;
  uint32_t bits = 8;
  std::vector<uint8_t> pixels;
};

struct LinearDngPayload {
  RasterImage raw_image;
  RasterImage rendered_preview_source;
};

struct RenderSettings {
  int output_color = 1;
  int use_camera_wb = 1;
  int use_camera_matrix = 1;
  int no_auto_scale = 0;
  int no_auto_bright = 1;
  int user_flip = 0;
  int user_qual = -1;
  int four_color_rgb = 0;
  int green_matching = 0;
  int med_passes = 0;
  float adjust_maximum_thr = 0.75f;
  float gamma_power = 1.0f;
  float gamma_slope = 1.0f;
};

bool IsOm3HighResMetadata(const SourceLinearDngMetadata& metadata) {
  return metadata.model == "OM-3" &&
         metadata.default_crop_origin_h == 6 &&
         metadata.default_crop_origin_v == 6 &&
         metadata.default_crop_width == 8160 &&
         metadata.default_crop_height == 6120;
}

bool ReadEnvInt(const char* name, int* value) {
  const char* raw = std::getenv(name);
  if (raw == nullptr || *raw == '\0') {
    return false;
  }

  char* end = nullptr;
  const long parsed = std::strtol(raw, &end, 10);
  if (end == raw || (end != nullptr && *end != '\0')) {
    return false;
  }

  *value = static_cast<int>(parsed);
  return true;
}

bool ReadEnvFloat(const char* name, float* value) {
  const char* raw = std::getenv(name);
  if (raw == nullptr || *raw == '\0') {
    return false;
  }

  char* end = nullptr;
  const float parsed = std::strtof(raw, &end);
  if (end == raw || (end != nullptr && *end != '\0')) {
    return false;
  }

  *value = parsed;
  return true;
}

void ApplyLibRawEnvironmentOverrides(RenderSettings* settings) {
  ReadEnvInt("HIRACO_LIBRAW_USER_QUAL", &settings->user_qual);
  ReadEnvInt("HIRACO_LIBRAW_FOUR_COLOR_RGB", &settings->four_color_rgb);
  ReadEnvInt("HIRACO_LIBRAW_GREEN_MATCHING", &settings->green_matching);
  ReadEnvInt("HIRACO_LIBRAW_MED_PASSES", &settings->med_passes);
  ReadEnvInt("HIRACO_LIBRAW_NO_AUTO_SCALE", &settings->no_auto_scale);
  ReadEnvInt("HIRACO_LIBRAW_NO_AUTO_BRIGHT", &settings->no_auto_bright);
  ReadEnvFloat("HIRACO_LIBRAW_ADJUST_MAXIMUM_THR", &settings->adjust_maximum_thr);
}

RenderSettings BuildRawRenderSettings(const SourceLinearDngMetadata& metadata) {
  RenderSettings settings;
  settings.output_color = 1;
  settings.use_camera_wb = 1;
  settings.use_camera_matrix = 1;
  settings.no_auto_scale = 0;
  settings.no_auto_bright = 1;
  settings.user_flip = 0;
  settings.user_qual = -1;
  settings.four_color_rgb = 0;
  settings.green_matching = 0;
  settings.med_passes = 0;
  settings.adjust_maximum_thr = 0.75f;
  settings.gamma_power = 1.0f;
  settings.gamma_slope = 1.0f;

  if (IsOm3HighResMetadata(metadata)) {
    settings.user_qual = 11;
    settings.four_color_rgb = 1;
    settings.green_matching = 1;
  }

  ApplyLibRawEnvironmentOverrides(&settings);
  return settings;
}

RenderSettings BuildPreviewRenderSettings(const SourceLinearDngMetadata& metadata) {
  RenderSettings settings;
  settings.output_color = 1;
  settings.use_camera_wb = 1;
  settings.use_camera_matrix = 1;
  settings.no_auto_scale = 0;
  settings.no_auto_bright = 0;
  settings.user_flip = 0;
  settings.user_qual = -1;
  settings.four_color_rgb = 0;
  settings.green_matching = 0;
  settings.med_passes = 0;
  settings.adjust_maximum_thr = 0.75f;
  settings.gamma_power = 0.45f;
  settings.gamma_slope = 4.5f;

  if (IsOm3HighResMetadata(metadata)) {
    settings.user_qual = 11;
    settings.four_color_rgb = 1;
    settings.green_matching = 1;
  }

  ApplyLibRawEnvironmentOverrides(&settings);
  return settings;
}

bool ShouldApplyOm3SourceDrivenLinearTransform(const SourceLinearDngMetadata& metadata,
                                               const RasterImage& image) {
  if (metadata.model != "OM-3" || !metadata.has_black_level || !metadata.has_as_shot_neutral) {
    return false;
  }

  const bool is_high_res = metadata.default_crop_origin_h == 6 &&
                           metadata.default_crop_origin_v == 6 &&
                           metadata.default_crop_width == 8160 &&
                           metadata.default_crop_height == 6120 &&
                           image.width == 8172 &&
                           image.height == 6132;
  const bool is_standard_20mp = metadata.default_crop_origin_h == 12 &&
                                metadata.default_crop_origin_v == 12 &&
                                metadata.default_crop_width == 5184 &&
                                metadata.default_crop_height == 3888 &&
                                image.width == 5220 &&
                                image.height == 3912;
  return is_high_res || is_standard_20mp;
}

bool IsOm3HighResRaster(const SourceLinearDngMetadata& metadata,
                       const RasterImage& image) {
  return metadata.model == "OM-3" &&
         metadata.default_crop_origin_h == 6 &&
         metadata.default_crop_origin_v == 6 &&
         metadata.default_crop_width == 8160 &&
         metadata.default_crop_height == 6120 &&
         image.width == 8172 &&
         image.height == 6132 &&
         image.colors == 3;
}

bool ShouldApplyPredictedDetailGain(const SourceLinearDngMetadata& metadata,
                                    const RasterImage& image) {
  return metadata.has_predicted_detail_gain &&
         metadata.predicted_detail_gain > 1.0001 &&
         IsOm3HighResRaster(metadata, image);
}

bool ShouldUseOm3AdobeMetadata(const SourceLinearDngMetadata& metadata,
                               const RasterImage& image) {
  if (metadata.model != "OM-3" || !metadata.has_black_level || !metadata.has_as_shot_neutral) {
    return false;
  }

  const bool is_high_res = metadata.default_crop_origin_h == 6 &&
                           metadata.default_crop_origin_v == 6 &&
                           metadata.default_crop_width == 8160 &&
                           metadata.default_crop_height == 6120 &&
                           image.width == 8172 &&
                           image.height == 6132;
  const bool is_standard_20mp = metadata.default_crop_origin_h == 12 &&
                                metadata.default_crop_origin_v == 12 &&
                                metadata.default_crop_width == 5184 &&
                                metadata.default_crop_height == 3888 &&
                                image.width == 5220 &&
                                image.height == 3912;
  return is_high_res || is_standard_20mp;
}

void ApplyLinearDngRasterTransform(const SourceLinearDngMetadata& metadata,
                                   RasterImage* image) {
  if (ShouldApplyOm3SourceDrivenLinearTransform(metadata, *image)) {
    const double pedestal = metadata.black_level;
    const double gain = (65535.0 - pedestal) / 65535.0;
    const size_t pixel_count = static_cast<size_t>(image->width) * static_cast<size_t>(image->height);
    for (size_t index = 0; index < pixel_count; ++index) {
      for (size_t channel = 0; channel < 3; ++channel) {
        const size_t sample_index = index * image->colors + channel;
        const double scaled = pedestal + gain * metadata.as_shot_neutral[channel] * image->pixels[sample_index];
        image->pixels[sample_index] = static_cast<uint16_t>(std::clamp(scaled, 0.0, 65535.0));
      }
    }
  }
}

void ApplyPredictedDetailGain(const SourceLinearDngMetadata& metadata,
                              RasterImage* image) {
  if (!ShouldApplyPredictedDetailGain(metadata, *image)) {
    return;
  }

  // 7x7 separable Gaussian kernel approximating sigma=2.0 for effective
  // high-pass detail extraction.  The 1D kernel is
  // [1, 6, 15, 20, 15, 6, 1] / 64 — a two-fold iterated Pascal row that
  // yields sigma ≈ sqrt(3/2) ≈ 1.73, close enough to 2.0 for our purposes.
  // The 2D normaliser is 64*64 = 4096.

  const double gain = std::clamp(metadata.predicted_detail_gain, 1.0, 5.0);
  const uint32_t width = image->width;
  const uint32_t height = image->height;
  const uint32_t colors = image->colors;
  const size_t row_stride = static_cast<size_t>(width) * colors;
  
  if (colors != 3) {
    return;
  }

  constexpr int kRadius = 3;
  constexpr int kKernelSize = 2 * kRadius + 1;
  const double k1D[kKernelSize] = {1.0, 6.0, 15.0, 20.0, 15.0, 6.0, 1.0};
  constexpr double kNorm = 4096.0;  // 64 * 64

  std::vector<double> y_rows[kKernelSize];
  for (int i = 0; i < kKernelSize; ++i) {
    y_rows[i].assign(width, 0.0);
  }

  auto load_y_row = [&](uint32_t row_idx, std::vector<double>* y_buffer) {
    const uint32_t clamped_row = std::min(row_idx, height - 1);
    const size_t offset = static_cast<size_t>(clamped_row) * row_stride;
    for (uint32_t col = 0; col < width; ++col) {
      const size_t px_idx = offset + col * colors;
      (*y_buffer)[col] = 0.299 * static_cast<double>(image->pixels[px_idx]) +
                         0.587 * static_cast<double>(image->pixels[px_idx + 1]) +
                         0.114 * static_cast<double>(image->pixels[px_idx + 2]);
    }
  };

  // Pre-fill the row ring buffer.  For the very first row (row 0) the center
  // sits at index kRadius.  Rows before the image are clamped to row 0.
  for (int i = 0; i < kKernelSize; ++i) {
    const int source_row = i - kRadius;  // may be negative
    const uint32_t clamped = source_row < 0 ? 0u
                             : (static_cast<uint32_t>(source_row) >= height
                                    ? height - 1
                                    : static_cast<uint32_t>(source_row));
    load_y_row(clamped, &y_rows[i]);
  }

  for (uint32_t row = 0; row < height; ++row) {
    const size_t output_row_offset = static_cast<size_t>(row) * row_stride;

    for (uint32_t col = 0; col < width; ++col) {
      // Compute column indices with clamped boundary handling.
      uint32_t cols[kKernelSize];
      for (int k = 0; k < kKernelSize; ++k) {
        const int src_col = static_cast<int>(col) + k - kRadius;
        cols[k] = src_col < 0 ? 0u
                  : (static_cast<uint32_t>(src_col) >= width
                         ? width - 1
                         : static_cast<uint32_t>(src_col));
      }

      double blurred_y = 0.0;
      for (int r = 0; r < kKernelSize; ++r) {
        const auto& Y = y_rows[r];
        double row_sum = 0.0;
        for (int c = 0; c < kKernelSize; ++c) {
          row_sum += k1D[c] * Y[cols[c]];
        }
        blurred_y += k1D[r] * row_sum;
      }
      blurred_y /= kNorm;

      const double center_y = y_rows[kRadius][col];
      const double luma_detail = center_y - blurred_y;
      const double luma_offset = (gain - 1.0) * luma_detail;

      for (uint32_t channel = 0; channel < colors; ++channel) {
        const size_t px_idx = output_row_offset + static_cast<size_t>(col) * colors + channel;
        const double source_value = static_cast<double>(image->pixels[px_idx]);
        const double enhanced = source_value + luma_offset;
        image->pixels[px_idx] = static_cast<uint16_t>(std::clamp(enhanced, 0.0, 65535.0));
      }
    }

    if (row + 1 >= height) {
      continue;
    }

    // Shift the ring buffer up by one row.
    for (int i = 0; i < kKernelSize - 1; ++i) {
      y_rows[i].swap(y_rows[i + 1]);
    }
    const uint32_t next_row = row + 1 + kRadius;
    load_y_row(next_row < height ? next_row : height - 1, &y_rows[kKernelSize - 1]);
  }
}


uint32_t DngVersionForCompression(const std::string& compression) {
  if (compression == "jpeg-xl") {
    return dngVersion_SaveDefault;
  }
  return dngVersion_1_6_0_0;
}

void ConfigureHost(dng_host& host, const std::string& compression) {
  host.SetSaveDNGVersion(DngVersionForCompression(compression));
  host.SetSaveLinearDNG(true);
  host.SetLosslessJXL(compression == "jpeg-xl");
  host.SetLossyMosaicJXL(false);
}

bool IsSupportedWriteCompression(const std::string& compression) {
  return compression == "uncompressed" || compression == "deflate" || compression == "jpeg-xl";
}

std::string UnsupportedCompressionMessage(const std::string& compression) {
  return "Unsupported compression requested for the current DNG writer path";
}

RasterImage MakeSyntheticRgbImage() {
  RasterImage image;
  image.width = 48;
  image.height = 32;
  image.colors = 3;
  image.bits = 16;
  image.pixels.resize(static_cast<size_t>(image.width) * image.height * image.colors);

  for (uint32_t row = 0; row < image.height; ++row) {
    for (uint32_t col = 0; col < image.width; ++col) {
      const size_t index = (static_cast<size_t>(row) * image.width + col) * image.colors;
      image.pixels[index + 0] = static_cast<uint16_t>((col * 65535u) / (image.width - 1));
      image.pixels[index + 1] = static_cast<uint16_t>((row * 65535u) / (image.height - 1));
      image.pixels[index + 2] = static_cast<uint16_t>(((row + col) * 65535u) / (image.width + image.height - 2));
    }
  }

  return image;
}

bool RenderLibRawImage(const std::string& source_path,
                       const RenderSettings& settings,
                       RasterImage* output,
                       std::string* error_message) {
  LibRaw processor;

  int result = processor.open_file(source_path.c_str());
  if (result != LIBRAW_SUCCESS) {
    *error_message = std::string("LibRaw open_file failed: ") + libraw_strerror(result);
    return false;
  }

  result = processor.unpack();
  if (result != LIBRAW_SUCCESS) {
    *error_message = std::string("LibRaw unpack failed: ") + libraw_strerror(result);
    processor.recycle();
    return false;
  }

  libraw_set_output_bps(&processor.imgdata, 16);
  libraw_set_output_color(&processor.imgdata, settings.output_color);
  libraw_set_no_auto_bright(&processor.imgdata, settings.no_auto_bright);
  libraw_set_gamma(&processor.imgdata, 0, settings.gamma_power);
  libraw_set_gamma(&processor.imgdata, 1, settings.gamma_slope);
  processor.imgdata.params.use_camera_wb = settings.use_camera_wb;
  processor.imgdata.params.use_camera_matrix = settings.use_camera_matrix;
  processor.imgdata.params.no_auto_scale = settings.no_auto_scale;
  processor.imgdata.params.user_flip = settings.user_flip;
  if (settings.user_qual >= 0) {
    processor.imgdata.params.user_qual = settings.user_qual;
  }
  processor.imgdata.params.four_color_rgb = settings.four_color_rgb;
  processor.imgdata.params.green_matching = settings.green_matching;
  processor.imgdata.params.med_passes = settings.med_passes;
  processor.imgdata.params.adjust_maximum_thr = settings.adjust_maximum_thr;

  result = processor.dcraw_process();
  if (result != LIBRAW_SUCCESS) {
    *error_message = std::string("LibRaw dcraw_process failed: ") + libraw_strerror(result);
    processor.recycle();
    return false;
  }

  int mem_error = LIBRAW_SUCCESS;
  libraw_processed_image_t* processed = processor.dcraw_make_mem_image(&mem_error);
  if (processed == nullptr || mem_error != LIBRAW_SUCCESS) {
    *error_message = std::string("LibRaw dcraw_make_mem_image failed: ") + libraw_strerror(mem_error);
    LibRaw::dcraw_clear_mem(processed);
    processor.recycle();
    return false;
  }

  if (processed->bits != 16 || processed->colors != 3) {
    *error_message = "LibRaw produced an unexpected processed image format";
    LibRaw::dcraw_clear_mem(processed);
    processor.recycle();
    return false;
  }

  output->width = processed->width;
  output->height = processed->height;
  output->colors = processed->colors;
  output->bits = processed->bits;

  const size_t sample_count = static_cast<size_t>(processed->width) *
                              static_cast<size_t>(processed->height) *
                              static_cast<size_t>(processed->colors);
  output->pixels.resize(sample_count);

  const auto* source_pixels = reinterpret_cast<const uint16_t*>(processed->data);
  std::copy(source_pixels, source_pixels + sample_count, output->pixels.begin());

  LibRaw::dcraw_clear_mem(processed);
  processor.recycle();
  return true;
}

bool BuildLinearDngPayload(const std::string& source_path,
                           const SourceLinearDngMetadata& metadata,
                           LinearDngPayload* payload,
                           std::string* error_message) {
  const RenderSettings raw_settings = BuildRawRenderSettings(metadata);
  const RenderSettings preview_settings = BuildPreviewRenderSettings(metadata);

  if (!RenderLibRawImage(source_path, raw_settings, &payload->raw_image, error_message)) {
    return false;
  }

  if (!RenderLibRawImage(source_path, preview_settings, &payload->rendered_preview_source, error_message)) {
    return false;
  }

  return true;
}

PreviewImage BuildPreviewImage(const RasterImage& source, uint32_t max_dimension) {
  PreviewImage preview;
  preview.colors = source.colors;

  if (source.width == 0 || source.height == 0 || source.colors == 0) {
    return preview;
  }

  const uint32_t longest_edge = std::max(source.width, source.height);
  if (longest_edge <= max_dimension) {
    preview.width = source.width;
    preview.height = source.height;
  } else if (source.width >= source.height) {
    preview.width = max_dimension;
    preview.height = std::max<uint32_t>(1, static_cast<uint32_t>((static_cast<uint64_t>(source.height) * max_dimension) / source.width));
  } else {
    preview.height = max_dimension;
    preview.width = std::max<uint32_t>(1, static_cast<uint32_t>((static_cast<uint64_t>(source.width) * max_dimension) / source.height));
  }

  preview.pixels.resize(static_cast<size_t>(preview.width) * preview.height * preview.colors);

  for (uint32_t row = 0; row < preview.height; ++row) {
    const uint32_t source_row = std::min<uint32_t>(source.height - 1,
                                                   static_cast<uint32_t>((static_cast<uint64_t>(row) * source.height) / preview.height));
    for (uint32_t col = 0; col < preview.width; ++col) {
      const uint32_t source_col = std::min<uint32_t>(source.width - 1,
                                                     static_cast<uint32_t>((static_cast<uint64_t>(col) * source.width) / preview.width));
      const size_t source_index = (static_cast<size_t>(source_row) * source.width + source_col) * source.colors;
      const size_t preview_index = (static_cast<size_t>(row) * preview.width + col) * preview.colors;
      for (uint32_t channel = 0; channel < preview.colors; ++channel) {
        preview.pixels[preview_index + channel] = static_cast<uint8_t>(source.pixels[source_index + channel] >> 8);
      }
    }
  }

  return preview;
}

AutoPtr<dng_image> MakeUint16Image(dng_host& host, const RasterImage& image) {
  dng_rect bounds(image.height, image.width);
  AutoPtr<dng_image> dng_image_ptr(host.Make_dng_image(bounds,
                                                       image.colors,
                                                       ttShort));
  dng_pixel_buffer buffer(bounds,
                          0,
                          image.colors,
                          ttShort,
                          pcInterleaved,
                          const_cast<uint16_t*>(image.pixels.data()));
  dng_image_ptr->Put(buffer);
  return AutoPtr<dng_image>(dng_image_ptr.Release());
}

AutoPtr<dng_image> MakeUint8Image(dng_host& host, const PreviewImage& image) {
  dng_rect bounds(image.height, image.width);
  AutoPtr<dng_image> dng_image_ptr(host.Make_dng_image(bounds,
                                                       image.colors,
                                                       ttByte));
  dng_pixel_buffer buffer(bounds,
                          0,
                          image.colors,
                          ttByte,
                          pcInterleaved,
                          const_cast<uint8_t*>(image.pixels.data()));
  dng_image_ptr->Put(buffer);
  return AutoPtr<dng_image>(dng_image_ptr.Release());
}

void AttachLinearSrgbProfile(dng_negative& negative) {
  AutoPtr<dng_camera_profile> profile(new dng_camera_profile());
  profile->SetName("hiraco-linear-srgb");
  profile->SetCalibrationIlluminant1(lsD65);
  profile->SetColorMatrix1(dng_matrix_3by3(
      3.2404542, -1.5371385, -0.4985314,
      -0.9692660, 1.8760108, 0.0415560,
      0.0556434, -0.2040259, 1.0572252));
  profile->SetForwardMatrix1(dng_matrix_3by3(
      0.4124564, 0.3575761, 0.1804375,
      0.2126729, 0.7151522, 0.0721750,
      0.0193339, 0.1191920, 0.9503041));
  negative.AddProfile(profile);
  negative.SetAsShotProfileName("hiraco-linear-srgb");
}

void AttachOm3AdobeLikeProfile(dng_negative& negative) {
  AutoPtr<dng_camera_profile> profile(new dng_camera_profile());
  profile->SetName("Adobe Standard");
  profile->SetCalibrationIlluminant1(lsStandardLightA);
  profile->SetCalibrationIlluminant2(lsD65);
  profile->SetColorMatrix1(dng_matrix_3by3(
      1.0602, -0.5977, 0.0574,
      -0.3109, 1.1296, 0.2063,
      0.0002, 0.0521, 0.6609));
  profile->SetColorMatrix2(dng_matrix_3by3(
      0.9090, -0.3591, -0.0756,
      -0.3252, 1.1396, 0.2109,
      -0.0318, 0.1059, 0.5606));
  profile->SetForwardMatrix1(dng_matrix_3by3(
      0.4436, 0.4158, 0.1049,
      0.1597, 0.8100, 0.0304,
      0.0503, 0.0022, 0.7725));
  profile->SetForwardMatrix2(dng_matrix_3by3(
      0.4504, 0.3593, 0.1546,
      0.2285, 0.7248, 0.0467,
      0.1088, 0.0045, 0.7118));
  negative.AddProfile(profile);
  negative.SetAsShotProfileName("Adobe Standard");
}

void SetSourceCameraNeutral(const SourceLinearDngMetadata& metadata,
                            dng_negative& negative) {
  if (!metadata.has_as_shot_neutral) {
    return;
  }

  dng_vector camera_neutral(3);
  camera_neutral[0] = metadata.as_shot_neutral[0];
  camera_neutral[1] = metadata.as_shot_neutral[1];
  camera_neutral[2] = metadata.as_shot_neutral[2];
  negative.SetCameraNeutral(camera_neutral);
}

void PopulateLinearRawNegative(dng_host& host,
                               const std::string& source_path,
                               const RasterImage& raw_image,
                               const SourceLinearDngMetadata& metadata,
                               dng_negative& negative) {
  const std::string model_name = metadata.unique_camera_model.empty() ? "hiraco" : metadata.unique_camera_model;
  const std::string local_name = metadata.model.empty() ? "hiraco" : metadata.model;

  negative.SetModelName(model_name.c_str());
  negative.SetLocalName(local_name.c_str());
  negative.SetOriginalRawFileName(std::filesystem::path(source_path).filename().string().c_str());
  negative.SetColorChannels(raw_image.colors);
  negative.SetBaseOrientation(dng_orientation::Normal());
  if (metadata.has_default_crop) {
    negative.SetDefaultCropOrigin(metadata.default_crop_origin_h, metadata.default_crop_origin_v);
    negative.SetDefaultCropSize(metadata.default_crop_width, metadata.default_crop_height);
  } else {
    negative.SetDefaultCropOrigin(0, 0);
    negative.SetDefaultCropSize(raw_image.width, raw_image.height);
  }
  negative.SetDefaultScale(dng_urational(1, 1), dng_urational(1, 1));
  negative.SetRawDefaultCrop();
  negative.SetRawDefaultScale();
  negative.SetRawBestQualityScale();
  if (ShouldApplyOm3SourceDrivenLinearTransform(metadata, raw_image) && metadata.has_black_level) {
    negative.SetBlackLevel(metadata.black_level);
  } else {
    negative.SetBlackLevel(0.0);
  }
  negative.SetWhiteLevel((1u << raw_image.bits) - 1);
  if (ShouldApplyOm3SourceDrivenLinearTransform(metadata, raw_image) && metadata.has_black_level) {
    negative.SetBaselineExposure(0.37);
  } else {
    negative.SetBaselineExposure(0.0);
  }
  negative.SetLinearResponseLimit(1.0);
  negative.UpdateDateTimeToNow();

  dng_vector analog_balance(raw_image.colors);
  analog_balance.SetIdentity(raw_image.colors);
  negative.SetAnalogBalance(analog_balance);

  if (ShouldUseOm3AdobeMetadata(metadata, raw_image)) {
    AttachOm3AdobeLikeProfile(negative);
    SetSourceCameraNeutral(metadata, negative);
  } else {
    AttachLinearSrgbProfile(negative);
    dng_vector camera_neutral(3);
    camera_neutral.SetIdentity(3);
    negative.SetCameraNeutral(camera_neutral);
    negative.SetCameraWhiteXY(D50_xy_coord());
  }

  AutoPtr<dng_image> image = MakeUint16Image(host, raw_image);
  negative.SetStage3Image(image);
  negative.SynchronizeMetadata();
}

void AppendImagePreview(dng_host& host,
                        const PreviewImage& preview_image,
                        dng_preview_list* preview_list) {
  if (preview_image.width == 0 || preview_image.height == 0) {
    return;
  }

  AutoPtr<dng_preview> preview(new dng_image_preview());
  preview->fInfo.fApplicationName.Set("hiraco");
  preview->fInfo.fSettingsName.Set("Default");
  preview->fInfo.fColorSpace = previewColorSpace_sRGB;

  AutoPtr<dng_image> preview_dng_image = MakeUint8Image(host, preview_image);
  preview->SetImage(host, preview_dng_image);
  preview_list->Append(preview);
}

}  // namespace

DngWriterRuntimeSummary BuildDngWriterRuntimeSummary(const std::string& compression) {
  DngWriterRuntimeSummary summary;
  summary.enabled = true;

  try {
    dng_host host;
    summary.host_created = true;

    ConfigureHost(host, compression);

    AutoPtr<dng_negative> negative(host.Make_dng_negative());
    summary.negative_created = negative.Get() != nullptr;
    summary.status = summary.negative_created
        ? "Adobe runtime initialized host and dng_negative successfully"
        : "Adobe runtime created host but returned a null dng_negative";
  } catch (const dng_exception& exc) {
    summary.status = exc.what();
  } catch (const std::exception& exc) {
    summary.status = exc.what();
  }

  return summary;
}

DngWriteResult WriteLinearDngFromRaw(const std::string& source_path,
                                     const std::string& output_path,
                                     const std::string& compression,
                                     const SourceLinearDngMetadata& metadata) {
  DngWriteResult result;

  if (!IsSupportedWriteCompression(compression)) {
    result.message = UnsupportedCompressionMessage(compression);
    return result;
  }

  try {
    LinearDngPayload payload;
    std::string render_error;
    if (!BuildLinearDngPayload(source_path, metadata, &payload, &render_error)) {
      result.message = render_error;
      return result;
    }

    ApplyPredictedDetailGain(metadata, &payload.raw_image);
    ApplyLinearDngRasterTransform(metadata, &payload.raw_image);

    PreviewImage preview_image = BuildPreviewImage(payload.rendered_preview_source, 1024);

    dng_host host;
    ConfigureHost(host, compression);

    AutoPtr<dng_negative> negative(host.Make_dng_negative());
    PopulateLinearRawNegative(host, source_path, payload.raw_image, metadata, *negative.Get());

    dng_preview_list preview_list;
    AppendImagePreview(host, preview_image, &preview_list);

    dng_file_stream stream(output_path.c_str(), true);
    dng_image_writer writer;
    if (compression == "jpeg-xl") {
      negative->LosslessCompressJXL(host, writer, false);
    }

    writer.WriteDNG(host,
                    stream,
                    *negative.Get(),
                    &preview_list,
                    DngVersionForCompression(compression),
                    compression == "uncompressed");
    stream.Flush();

    result.ok = true;
    result.message = "native linear DNG write succeeded";
  } catch (const dng_exception& exc) {
    result.message = exc.what();
  } catch (const std::exception& exc) {
    result.message = exc.what();
  }

  return result;
}

DngWriteResult WriteSyntheticLinearDng(const std::string& output_path,
                                       const std::string& compression) {
  DngWriteResult result;

  if (!IsSupportedWriteCompression(compression)) {
    result.message = UnsupportedCompressionMessage(compression);
    return result;
  }

  try {
    RasterImage processed = MakeSyntheticRgbImage();
    PreviewImage preview_image = BuildPreviewImage(processed, 256);

    dng_host host;
    ConfigureHost(host, compression);

    AutoPtr<dng_negative> negative(host.Make_dng_negative());
    PopulateLinearRawNegative(host, "synthetic-gradient.raw", processed, SourceLinearDngMetadata(), *negative.Get());

    dng_preview_list preview_list;
    AppendImagePreview(host, preview_image, &preview_list);

    dng_file_stream stream(output_path.c_str(), true);
    dng_image_writer writer;
    if (compression == "jpeg-xl") {
      negative->LosslessCompressJXL(host, writer, false);
    }

    writer.WriteDNG(host,
                    stream,
                    *negative.Get(),
                    &preview_list,
                    DngVersionForCompression(compression),
                    compression == "uncompressed");
    stream.Flush();

    result.ok = true;
    result.message = "synthetic linear DNG write succeeded";
  } catch (const dng_exception& exc) {
    result.message = exc.what();
  } catch (const std::exception& exc) {
    result.message = exc.what();
  }

  return result;
}

#else

DngWriterRuntimeSummary BuildDngWriterRuntimeSummary(const std::string&) {
  DngWriterRuntimeSummary summary;
  summary.enabled = false;
  summary.status = "Adobe DNG SDK integration not enabled in this build";
  return summary;
}

DngWriteResult WriteLinearDngFromRaw(const std::string&, const std::string&, const std::string&, const SourceLinearDngMetadata&) {
  DngWriteResult result;
  result.message = "Adobe DNG SDK integration not enabled in this build";
  return result;
}

DngWriteResult WriteSyntheticLinearDng(const std::string&, const std::string&) {
  DngWriteResult result;
  result.message = "Adobe DNG SDK integration not enabled in this build";
  return result;
}

#endif