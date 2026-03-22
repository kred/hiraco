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
  float gamma_power = 1.0f;
  float gamma_slope = 1.0f;
};

void ConfigureHost(dng_host& host, const std::string& compression) {
  host.SetSaveDNGVersion(dngVersion_SaveDefault);
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
                           LinearDngPayload* payload,
                           std::string* error_message) {
  const RenderSettings raw_settings = {
      0,
      0,
      0,
      0,
  1,
  0,
  1.0f,
  1.0f,
  };
  const RenderSettings preview_settings = {
      1,
      1,
      1,
      0,
  0,
  0,
  0.45f,
  4.5f,
  };

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

void PopulateLinearRawNegative(dng_host& host,
                               const std::string& source_path,
                               const RasterImage& raw_image,
                               dng_negative& negative) {
  negative.SetModelName("OM-3");
  negative.SetLocalName("OM-3");
  negative.SetOriginalRawFileName(std::filesystem::path(source_path).filename().string().c_str());
  negative.SetColorimetricReference(crSceneReferred);
  negative.SetColorChannels(raw_image.colors);
  negative.SetBaseOrientation(dng_orientation::Normal());
  negative.SetDefaultCropOrigin(0, 0);
  negative.SetDefaultCropSize(raw_image.width, raw_image.height);
  negative.SetDefaultScale(dng_urational(1, 1), dng_urational(1, 1));
  negative.SetRawDefaultCrop();
  negative.SetRawDefaultScale();
  negative.SetRawBestQualityScale();
  negative.SetCameraWhiteXY(D65_xy_coord());
  negative.SetBlackLevel(0.0);
  negative.SetWhiteLevel((1u << raw_image.bits) - 1);
  negative.SetBaselineExposure(0.0);
  negative.SetLinearResponseLimit(1.0);
  negative.UpdateDateTimeToNow();

  dng_vector analog_balance(raw_image.colors);
  analog_balance.SetIdentity(raw_image.colors);
  negative.SetAnalogBalance(analog_balance);

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
                                     const std::string& compression) {
  DngWriteResult result;

  if (!IsSupportedWriteCompression(compression)) {
    result.message = UnsupportedCompressionMessage(compression);
    return result;
  }

  try {
    LinearDngPayload payload;
    std::string render_error;
    if (!BuildLinearDngPayload(source_path, &payload, &render_error)) {
      result.message = render_error;
      return result;
    }

    PreviewImage preview_image = BuildPreviewImage(payload.rendered_preview_source, 1024);

    dng_host host;
    ConfigureHost(host, compression);

    AutoPtr<dng_negative> negative(host.Make_dng_negative());
    PopulateLinearRawNegative(host, source_path, payload.raw_image, *negative.Get());

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
                    dngVersion_SaveDefault,
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
    PopulateLinearRawNegative(host, "synthetic-gradient.raw", processed, *negative.Get());

    dng_preview_list preview_list;
    AppendImagePreview(host, preview_image, &preview_list);

    dng_file_stream stream(output_path.c_str(), true);
    dng_image_writer writer;
    writer.WriteDNG(host,
                    stream,
                    *negative.Get(),
                    &preview_list,
                    dngVersion_SaveDefault,
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

DngWriteResult WriteLinearDngFromRaw(const std::string&, const std::string&, const std::string&) {
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