#include "dng_writer_bridge.h"
#include "hiraco_core.h"

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <filesystem>
#include <iostream>
#include <stdexcept>
#include <string>
#include <vector>

namespace {

void Expect(bool condition, const std::string& message) {
  if (!condition) {
    throw std::runtime_error(message);
  }
}

RasterImage MakeSyntheticRgb(uint32_t width, uint32_t height) {
  RasterImage image;
  image.width = width;
  image.height = height;
  image.colors = 3;
  image.bits = 16;
  image.pixels.resize(static_cast<size_t>(width) * height * image.colors);

  for (uint32_t row = 0; row < height; ++row) {
    for (uint32_t col = 0; col < width; ++col) {
      const size_t index = (static_cast<size_t>(row) * width + col) * image.colors;
      const double fx = static_cast<double>(col) / std::max<uint32_t>(1, width - 1);
      const double fy = static_cast<double>(row) / std::max<uint32_t>(1, height - 1);
      image.pixels[index + 0] = static_cast<uint16_t>(std::clamp(2000.0 + 26000.0 * fx + 5000.0 * std::sin(fy * 7.0), 0.0, 65535.0));
      image.pixels[index + 1] = static_cast<uint16_t>(std::clamp(2500.0 + 28000.0 * fy + 4000.0 * std::cos(fx * 9.0), 0.0, 65535.0));
      image.pixels[index + 2] = static_cast<uint16_t>(std::clamp(3000.0 + 16000.0 * fx * fy + 3500.0 * std::sin((fx + fy) * 11.0), 0.0, 65535.0));
    }
  }

  return image;
}

RasterImage MakeSyntheticGuide(uint32_t width, uint32_t height) {
  RasterImage image;
  image.width = width;
  image.height = height;
  image.colors = 4;
  image.bits = 16;
  image.pixels.resize(static_cast<size_t>(width) * height * image.colors);

  for (uint32_t row = 0; row < height; ++row) {
    for (uint32_t col = 0; col < width; ++col) {
      const size_t index = (static_cast<size_t>(row) * width + col) * image.colors;
      const double base = 4096.0 + 12000.0 * static_cast<double>(col) / width;
      image.pixels[index + 0] = 0;
      image.pixels[index + 1] = static_cast<uint16_t>(std::clamp(base + 1200.0 * std::sin(row * 0.03), 0.0, 65535.0));
      image.pixels[index + 2] = 0;
      image.pixels[index + 3] = static_cast<uint16_t>(std::clamp(base + 900.0 * std::cos(col * 0.05), 0.0, 65535.0));
    }
  }

  return image;
}

RasterImage CropRaster(const RasterImage& source, const CropRect& crop) {
  RasterImage cropped;
  cropped.width = crop.width;
  cropped.height = crop.height;
  cropped.colors = source.colors;
  cropped.bits = source.bits;
  cropped.pixels.resize(static_cast<size_t>(crop.width) * crop.height * source.colors);

  for (uint32_t row = 0; row < crop.height; ++row) {
    for (uint32_t col = 0; col < crop.width; ++col) {
      const size_t src_index =
          (static_cast<size_t>(crop.y + row) * source.width + crop.x + col) * source.colors;
      const size_t dst_index =
          (static_cast<size_t>(row) * crop.width + col) * cropped.colors;
      for (uint32_t channel = 0; channel < cropped.colors; ++channel) {
        cropped.pixels[dst_index + channel] = source.pixels[src_index + channel];
      }
    }
  }

  return cropped;
}

PreviewImage ToPreview(const RasterImage& source) {
  PreviewImage preview;
  preview.width = source.width;
  preview.height = source.height;
  preview.colors = source.colors;
  preview.bits = 8;
  preview.pixels.resize(static_cast<size_t>(source.width) * source.height * source.colors);

  for (size_t index = 0; index < preview.pixels.size(); ++index) {
    preview.pixels[index] = static_cast<uint8_t>(source.pixels[index] >> 8);
  }
  return preview;
}

void ApplyPreviewTransform(const SourceLinearDngMetadata& metadata, RasterImage* image) {
  if (image == nullptr || image->colors < 3 || !metadata.has_black_level || !metadata.has_as_shot_neutral) {
    return;
  }

  const double pedestal = metadata.black_level;
  const double gain = (65535.0 - pedestal) / 65535.0;
  const double exposure_gain = std::pow(2.0, 0.37);
  const size_t pixel_count = static_cast<size_t>(image->width) * image->height;
  for (size_t index = 0; index < pixel_count; ++index) {
    for (size_t channel = 0; channel < 3; ++channel) {
      const size_t sample_index = index * image->colors + channel;
      const double neutral = std::max(metadata.as_shot_neutral[channel], 1e-6);
      const double scaled = exposure_gain * (pedestal + gain * image->pixels[sample_index] / neutral);
      image->pixels[sample_index] = static_cast<uint16_t>(std::clamp(scaled, 0.0, 65535.0));
    }
  }
}

SourceLinearDngMetadata MakeHighResMetadata() {
  SourceLinearDngMetadata metadata;
  metadata.has_black_level = true;
  metadata.black_level = 128.0;
  metadata.has_as_shot_neutral = true;
  metadata.as_shot_neutral[0] = 1.1;
  metadata.as_shot_neutral[1] = 1.0;
  metadata.as_shot_neutral[2] = 0.95;
  metadata.has_predicted_detail_gain = true;
  metadata.predicted_detail_gain = 1.8;
  metadata.default_crop_origin_h = 6;
  metadata.default_crop_origin_v = 6;
  metadata.default_crop_width = 8160;
  metadata.default_crop_height = 6120;
  return metadata;
}

void TestResolveOutputPath() {
  const auto resolved = ResolveOutputPath("/tmp/input/scene.ORF", "/tmp/output", "session-a");
  Expect(resolved == std::filesystem::path("/tmp/output/session-a/scene.dng"),
         "output path should use chosen base directory and relative subfolder");
}

void TestOverwriteDecision() {
  {
    const OverwriteDecision decision =
        ResolveOverwriteDecision(OverwritePolicy::kAsk, false, OverwriteResponse::kCancel);
    Expect(decision.should_write, "non-existing targets should write without prompting");
  }
  {
    const OverwriteDecision decision =
        ResolveOverwriteDecision(OverwritePolicy::kAsk, true, OverwriteResponse::kYesToAll);
    Expect(decision.should_write && decision.next_policy == OverwritePolicy::kOverwriteAll,
           "yes to all should switch policy to overwrite all");
  }
  {
    const OverwriteDecision decision =
        ResolveOverwriteDecision(OverwritePolicy::kSkipAll, true, OverwriteResponse::kYes);
    Expect(!decision.should_write, "skip-all policy should skip existing targets");
  }
}

void TestCropHelpers() {
  const CropRect centered = CenterCropRect(1000, 800, 512, 512);
  Expect(centered.x == 244 && centered.y == 144, "center crop should be centered");

  const CropRect clamped = ClampCropRect({900, 700, 512, 512}, 1000, 800);
  Expect(clamped.x == 488 && clamped.y == 288, "crop should clamp inside image bounds");
}

void TestResolvedStageDefaults() {
  const SourceLinearDngMetadata metadata = MakeHighResMetadata();
  const ResolvedStageSettings fifty_mp = ResolveStageSettingsForImage(metadata, 8172, 6132, {});
  const ResolvedStageSettings eighty_mp = ResolveStageSettingsForImage(metadata, 10386, 7792, {});
  Expect(std::abs(fifty_mp.stage1_psf_sigma - 2.0f) < 1e-6f, "50 MP default sigma should be 2.0");
  Expect(std::abs(eighty_mp.stage1_psf_sigma - 2.5f) < 1e-6f, "80 MP default sigma should be 2.5");
}

void TestRealOrfPreviewSmoke() {
  const std::filesystem::path source_path = std::filesystem::path("reference") / "_3210504.ORF";
  if (!std::filesystem::exists(source_path)) {
    return;
  }

  PreparedSource prepared;
  std::string error_message;
  const bool prepare_ok = PrepareSource(source_path.string(), &prepared, &error_message);
  Expect(prepare_ok, "real ORF source preparation should succeed: " + error_message);
  Expect(prepared.IsValid(), "prepared ORF source should report valid dimensions");

  auto original_preview = std::make_shared<PreviewImage>();
  const bool original_ok =
      RenderOriginalPreview(&prepared, original_preview, {}, {}, {}, &error_message);
  Expect(original_ok, "real ORF original preview should succeed: " + error_message);
  Expect(original_preview->width > 0 && original_preview->height > 0,
         "real ORF original preview should produce non-empty image data");

  const CropRect crop = CenterCropRect(prepared.image_width, prepared.image_height, 512, 512);
  auto crop_preview = std::make_shared<PreviewImage>();
  const bool crop_ok =
      RenderConvertedCrop(&prepared, crop, {}, crop_preview, {}, {}, {}, &error_message);
  Expect(crop_ok, "real ORF converted crop preview should succeed: " + error_message);
  Expect(crop_preview->width == 512 && crop_preview->height == 512,
         "real ORF crop preview should produce the requested 512x512 crop");
}

void TestRealOrfCropPreviewFollowsCropRect() {
  const std::filesystem::path source_path = std::filesystem::path("reference") / "_3210504.ORF";
  if (!std::filesystem::exists(source_path)) {
    return;
  }

  PreparedSource prepared;
  std::string error_message;
  const bool prepare_ok = PrepareSource(source_path.string(), &prepared, &error_message);
  Expect(prepare_ok, "real ORF crop-follow test should prepare source: " + error_message);

  auto center_preview = std::make_shared<PreviewImage>();
  const CropRect center_crop = CenterCropRect(prepared.image_width, prepared.image_height, 512, 512);
  const bool center_ok =
      RenderConvertedCrop(&prepared, center_crop, {}, center_preview, {}, {}, {}, &error_message);
  Expect(center_ok, "real ORF center crop preview should succeed: " + error_message);

  const CropRect offset_crop =
      ClampCropRect({256, 256, 512, 512}, prepared.image_width, prepared.image_height);
  auto offset_preview = std::make_shared<PreviewImage>();
  const bool offset_ok =
      RenderConvertedCrop(&prepared, offset_crop, {}, offset_preview, {}, {}, {}, &error_message);
  Expect(offset_ok, "real ORF offset crop preview should succeed: " + error_message);
  Expect(center_preview->width == offset_preview->width && center_preview->height == offset_preview->height,
         "real ORF crop previews should share dimensions");

  int total_abs_diff = 0;
  const size_t sample_count = std::min(center_preview->pixels.size(), offset_preview->pixels.size());
  for (size_t index = 0; index < sample_count; ++index) {
    total_abs_diff += std::abs(static_cast<int>(center_preview->pixels[index]) -
                               static_cast<int>(offset_preview->pixels[index]));
  }
  Expect(total_abs_diff > 50000,
         "real ORF crop previews should change when crop rect changes");
}

void TestRealOrfPreviewAutoBrightGainEstimate() {
  const std::filesystem::path source_path = std::filesystem::path("reference") / "_3210504.ORF";
  if (!std::filesystem::exists(source_path)) {
    return;
  }

  PreparedSource prepared;
  std::string error_message;
  const bool prepare_ok = PrepareSource(source_path.string(), &prepared, &error_message);
  Expect(prepare_ok, "real ORF auto-bright test should prepare source: " + error_message);

  double gain = 1.0;
  const bool gain_ok = EstimatePreviewAutoBrightGainFromRaw(source_path.string(),
                                                            prepared.metadata,
                                                            {},
                                                            &gain,
                                                            &error_message);
  Expect(gain_ok, "real ORF auto-bright gain estimate should succeed: " + error_message);
  Expect(gain > 1.1,
         "real ORF auto-bright gain should brighten the preview (gain=" +
             std::to_string(gain) + ")");
}

void TestCropPreviewMatchesFullImageCrop() {
  const SourceLinearDngMetadata metadata = MakeHighResMetadata();
  ProcessingCache cache;
  cache.raw_image = MakeSyntheticRgb(900, 900);
  cache.cfa_guide_image = MakeSyntheticGuide(900, 900);
  cache.raw_image_is_camera_space = true;

  const CropRect crop = {160, 144, 512, 512};
  const ResolvedStageSettings settings =
      ResolveStageSettingsForImage(metadata, cache.raw_image.width, cache.raw_image.height, {});

  RasterImage full_processed = cache.raw_image;
  for (uint16_t& sample : full_processed.pixels) {
    sample = static_cast<uint16_t>(std::clamp(static_cast<double>(sample) - metadata.black_level, 0.0, 65535.0));
  }
  std::string error_message;
  const bool full_ok = ApplyResolvedStageSettingsForTesting(metadata,
                                                            settings,
                                                            &cache.cfa_guide_image,
                                                            &full_processed,
                                                            {},
                                                            {},
                                                            &error_message);
  Expect(full_ok, "full-image enhancement should succeed");
  for (uint16_t& sample : full_processed.pixels) {
    sample = static_cast<uint16_t>(std::clamp(static_cast<double>(sample) + metadata.black_level, 0.0, 65535.0));
  }
  ApplyPreviewTransform(metadata, &full_processed);
  const PreviewImage expected = ToPreview(CropRaster(full_processed, crop));

  auto actual = std::make_shared<PreviewImage>();
  const bool crop_ok = RenderConvertedCropPreview(metadata, cache, crop, {}, actual, {}, {}, &error_message);
  Expect(crop_ok, "ROI crop enhancement should succeed");
  Expect(actual->width == expected.width && actual->height == expected.height,
         "crop preview dimensions should match expected crop dimensions");

  int max_abs_diff = 0;
  for (size_t index = 0; index < expected.pixels.size(); ++index) {
    max_abs_diff = std::max(max_abs_diff,
                            std::abs(static_cast<int>(expected.pixels[index]) -
                                     static_cast<int>(actual->pixels[index])));
  }
  Expect(max_abs_diff <= 150,
         "ROI crop preview should stay close to full-image crop output (max diff=" +
             std::to_string(max_abs_diff) + ")");
}

}  // namespace

int main() {
  try {
    TestResolveOutputPath();
    TestOverwriteDecision();
    TestCropHelpers();
    TestResolvedStageDefaults();
    TestRealOrfPreviewSmoke();
    TestRealOrfCropPreviewFollowsCropRect();
    TestRealOrfPreviewAutoBrightGainEstimate();
    TestCropPreviewMatchesFullImageCrop();
  } catch (const std::exception& exception) {
    std::cerr << "Test failure: " << exception.what() << "\n";
    return 1;
  }

  std::cout << "hiraco core tests passed\n";
  return 0;
}
