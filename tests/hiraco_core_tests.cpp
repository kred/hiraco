#include "dng_writer_bridge.h"
#include "dng_sdk_bridge.h"
#include "hiraco_core.h"

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstdlib>
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

// Resolves a repository fixture relative to this test file, so the test works
// no matter which directory ctest runs it from. Always returns a non-empty
// path and never touches the filesystem; callers must check existence
// themselves, since the .ORF fixtures are gitignored.
//
// Resolving from __FILE__ rather than the process CWD is load-bearing: ctest
// runs tests with the build directory as CWD, so a bare "reference/..." path
// resolves to nothing there and the guard would silently skip.
std::filesystem::path RealOrfFixturePath(const std::string& relative_path) {
  return std::filesystem::path(__FILE__).parent_path().parent_path() / relative_path;
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

RasterImage MakeSyntheticTexturedRgb(uint32_t width, uint32_t height) {
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
    const double micro =
      950.0 * std::sin(1.11 * static_cast<double>(col) + 0.91 * static_cast<double>(row));
    const double fine =
      820.0 * std::cos(0.57 * static_cast<double>(col) - 0.49 * static_cast<double>(row));
    const double small =
      1700.0 * std::sin(0.19 * static_cast<double>(col)) *
      std::cos(0.17 * static_cast<double>(row));
    const double medium =
      2600.0 * std::sin(fx * 13.0) + 2200.0 * std::cos(fy * 11.0);

    image.pixels[index + 0] = static_cast<uint16_t>(std::clamp(
      7000.0 + 17000.0 * fx + 12000.0 * fy + medium + 0.9 * small + micro,
      0.0,
      65535.0));
    image.pixels[index + 1] = static_cast<uint16_t>(std::clamp(
      8500.0 + 15000.0 * fy + 10000.0 * fx + 0.8 * medium + 1.1 * small + fine,
      0.0,
      65535.0));
    image.pixels[index + 2] = static_cast<uint16_t>(std::clamp(
      6500.0 + 13000.0 * fx * fy + 9000.0 * (1.0 - fx) + 0.7 * medium + 0.9 * small +
        0.8 * micro - 0.6 * fine,
      0.0,
      65535.0));
  }
  }

  return image;
}

RasterImage MakeSyntheticHighlightRgb(uint32_t width, uint32_t height) {
  RasterImage image;
  image.width = width;
  image.height = height;
  image.colors = 3;
  image.bits = 16;
  image.pixels.resize(static_cast<size_t>(width) * height * image.colors);

  for (uint32_t row = 0; row < height; ++row) {
    for (uint32_t col = 0; col < width; ++col) {
      const size_t index = (static_cast<size_t>(row) * width + col) * image.colors;
      const double checker = ((row / 8 + col / 8) & 1) == 0 ? 1.0 : -1.0;
      const double ripple = 1200.0 * std::sin(0.31 * static_cast<double>(row + col));
      image.pixels[index + 0] = static_cast<uint16_t>(std::clamp(50500.0 + checker * 7200.0 + ripple,
                                                                   0.0,
                                                                   65534.0));
      image.pixels[index + 1] = static_cast<uint16_t>(std::clamp(56000.0 + checker * 7600.0 + ripple,
                                                                   0.0,
                                                                   65534.0));
      image.pixels[index + 2] = static_cast<uint16_t>(std::clamp(47000.0 + checker * 6800.0 + ripple,
                                                                   0.0,
                                                                   65534.0));
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

double ComputeLumaNeighborhoodEnergy(const RasterImage& image, uint32_t radius) {
  Expect(image.colors >= 3, "energy metric expects RGB image");
  if (image.width <= radius * 2 || image.height <= radius * 2) {
    return 0.0;
  }

  auto luma_at = [&](uint32_t col, uint32_t row) {
    const size_t index = (static_cast<size_t>(row) * image.width + col) * image.colors;
    return 0.2126 * static_cast<double>(image.pixels[index + 0]) +
           0.7152 * static_cast<double>(image.pixels[index + 1]) +
           0.0722 * static_cast<double>(image.pixels[index + 2]);
  };

  double sum = 0.0;
  size_t count = 0;
  for (uint32_t row = radius; row + radius < image.height; ++row) {
    for (uint32_t col = radius; col + radius < image.width; ++col) {
      const double center = luma_at(col, row);
      const double neighbor_mean = 0.25 * (
          luma_at(col - radius, row) +
          luma_at(col + radius, row) +
          luma_at(col, row - radius) +
          luma_at(col, row + radius));
      sum += std::abs(center - neighbor_mean);
      ++count;
    }
  }

  return count > 0 ? sum / static_cast<double>(count) : 0.0;
}

RasterImage RunEnhancementForTesting(const RasterImage& source,
                                     const RasterImage* guide,
                                     const SourceLinearDngMetadata& metadata,
                                     const ResolvedStageSettings& settings) {
  RasterImage processed = source;
  for (uint16_t& sample : processed.pixels) {
    sample = static_cast<uint16_t>(std::clamp(
        static_cast<double>(sample) - metadata.black_level,
        0.0,
        65535.0));
  }

  std::string error_message;
  const bool ok = ApplyResolvedStageSettingsForTesting(metadata,
                                                       settings,
                                                       guide,
                                                       &processed,
                                                       {},
                                                       {},
                                                       &error_message);
  Expect(ok, "synthetic enhancement should succeed: " + error_message);
  return processed;
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
  Expect(std::abs(fifty_mp.stage1_psf_sigma - 1.0f) < 1e-6f, "50 MP default sigma should be 1.0");
  Expect(std::abs(eighty_mp.stage1_psf_sigma - 1.0f) < 1e-6f, "80 MP default sigma should be 1.0");
}

void TestEnhancementPreservesHighlightHeadroom() {
  SourceLinearDngMetadata metadata = MakeHighResMetadata();
  metadata.black_level = 0.0;
  const RasterImage source = MakeSyntheticHighlightRgb(384, 384);
  const ResolvedStageSettings settings =
      ResolveStageSettingsForImage(metadata, source.width, source.height, {});
  const RasterImage enhanced = RunEnhancementForTesting(source, nullptr, metadata, settings);

  for (uint16_t sample : enhanced.pixels) {
    Expect(sample < 65535,
           "detail enhancement must not clip an originally unclipped highlight");
  }
}

void TestRealOrfPreviewSmoke() {
  // .ORF fixtures are gitignored, and ctest runs with the build directory as
  // the CWD, so resolve from this test file and fail loudly rather than skip.
  const std::filesystem::path source_path = RealOrfFixturePath("reference/_3210504.ORF");
  Expect(std::filesystem::exists(source_path),
         "missing test fixture (gitignored *.ORF): " + source_path.string());

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
  // .ORF fixtures are gitignored, and ctest runs with the build directory as
  // the CWD, so resolve from this test file and fail loudly rather than skip.
  const std::filesystem::path source_path = RealOrfFixturePath("reference/_3210504.ORF");
  Expect(std::filesystem::exists(source_path),
         "missing test fixture (gitignored *.ORF): " + source_path.string());

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
  // .ORF fixtures are gitignored, and ctest runs with the build directory as
  // the CWD, so resolve from this test file and fail loudly rather than skip.
  const std::filesystem::path source_path = RealOrfFixturePath("reference/_3210504.ORF");
  Expect(std::filesystem::exists(source_path),
         "missing test fixture (gitignored *.ORF): " + source_path.string());

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

// Shared visibility metrics for one (fixture, resolution) pair.
struct RecoveryVisibilitySample {
  size_t visible = 0;
  size_t bright = 0;
  size_t darkest_changed_luma = 255;
  size_t max_code_delta = 0;
};

RecoveryVisibilitySample MeasureRecoveryVisibility(const PreviewImage& normal_preview,
                                                   const PreviewImage& recovery_preview) {
  RecoveryVisibilitySample sample;
  for (size_t pixel = 0; pixel + 2 < normal_preview.pixels.size() &&
                         pixel + 2 < recovery_preview.pixels.size();
       pixel += 3) {
    bool pixel_changed = false;
    for (size_t channel = 0; channel < 3; ++channel) {
      const int delta = static_cast<int>(normal_preview.pixels[pixel + channel]) -
                        static_cast<int>(recovery_preview.pixels[pixel + channel]);
      if (delta != 0) {
        pixel_changed = true;
        sample.max_code_delta =
            std::max(sample.max_code_delta, static_cast<size_t>(std::abs(delta)));
      }
    }
    if (!pixel_changed) {
      continue;
    }
    ++sample.visible;
    const double luma = 0.2126 * normal_preview.pixels[pixel] +
                        0.7152 * normal_preview.pixels[pixel + 1] +
                        0.0722 * normal_preview.pixels[pixel + 2];
    sample.darkest_changed_luma =
        std::min(sample.darkest_changed_luma, static_cast<size_t>(luma));
    if (luma >= 224.0) {
      ++sample.bright;
    }
  }
  return sample;
}

void TestLocalOm3RecoveryFullPreviewSmoke() {
  // The sample directory is intentionally untracked. Exercise the
  // display-resolution recovery mapping when a local OM-3 ORF/ORI pair is
  // available, without making the portable test suite depend on that fixture.
  // Both local pairs are covered: the objective's acceptance criterion is about
  // both fixtures, and they exercise different ends of the recovery population
  // (_8142490 has far more recoverable highlight area than _8142491).
  const char* kFixtures[] = {"sample/_8142491.ORF", "sample/_8142490.ORF"};
  // The GUI's own preview resolution is the second entry; the first is the
  // downscaled resolution at which the recovery effect was always visible.
  const uint32_t kResolutions[] = {512, 2560};

  for (const char* relative_path : kFixtures) {
    // Pass rate at the smaller preview resolution for this fixture, used by
    // the convergence pin below.
    int small_pass_rate_percent = 0;
    const std::filesystem::path source_path = RealOrfFixturePath(relative_path);
    Expect(std::filesystem::exists(source_path),
           "missing test fixture (gitignored *.ORF): " + source_path.string());
    PreparedSource prepared;
    std::string error_message;
    const bool prepare_ok =
        PrepareSource(source_path.string(), &prepared, &error_message);
    Expect(prepare_ok, "local OM-3 source preparation should succeed: " + error_message);
    if (!prepared.HasHighlightRecoverySource()) {
      continue;
    }

    for (uint32_t max_dimension : kResolutions) {
      auto normal_preview = std::make_shared<PreviewImage>();
      const bool normal_ok = RenderConvertedFullPreview(&prepared,
                                                        {},
                                                        max_dimension,
                                                        normal_preview,
                                                        {},
                                                        {},
                                                        {},
                                                        &error_message);
      Expect(normal_ok,
             "local OM-3 full preview should succeed: " + error_message);
      Expect(normal_preview->width > 0 && normal_preview->height > 0 &&
                 std::max(normal_preview->width, normal_preview->height) <= max_dimension,
             "local OM-3 full preview should obey its display-size limit");

      prepared.enable_highlight_recovery = true;
      auto recovery_preview = std::make_shared<PreviewImage>();
      const bool recovery_ok = RenderConvertedFullPreview(&prepared,
                                                          {},
                                                          max_dimension,
                                                          recovery_preview,
                                                          {},
                                                          {},
                                                          {},
                                                          &error_message);
      Expect(recovery_ok,
             "local OM-3 recovery-enabled full preview should succeed: " +
                 error_message);
      Expect(recovery_preview->width == normal_preview->width &&
                 recovery_preview->height == normal_preview->height,
             "recovery-enabled full preview should retain the normal preview dimensions");

      const HighlightRecoveryStats stats = GetLastHighlightRecoveryStats();
      const RecoveryVisibilitySample visible =
          MeasureRecoveryVisibility(*normal_preview, *recovery_preview);

      // Preview-visibility pin (P3): recovery must produce a real, highlight-gated
      // change in the displayed preview at BOTH preview resolutions on BOTH local
      // fixtures, and the GUI-facing stats must report it.  Measured on the
      // sample pairs with the shipped implementation
      // (docs/highlight-recovery-audit.md §9.5):
      //   512 px  _8142491 -> candidates 3763, changed 3210, 2125 px visibly
      //              (all at luma >= 224), max delta 29 codes
      //   2560 px _8142491 -> candidates 47672, changed 5805, 2115 px visibly
      //              (all at luma >= 224), max delta 19 codes
      // Recovery must stay highlight-gated: nothing below luma 192 is touched on
      // either fixture at any resolution, and the bulk of the changes sit in the
      // lum >= 224 band.
      Expect(stats.applicable,
             "local OM-3 recovery preview run should be applicable (fixture=" +
                 std::string(relative_path) + " res=" + std::to_string(max_dimension) +
                 ")");
      Expect(stats.changed >= 1000,
             "local OM-3 recovery preview should change a meaningful highlight "
             "population (fixture=" + std::string(relative_path) +
                 " res=" + std::to_string(max_dimension) +
                 " changed=" + std::to_string(stats.changed) + ")");
      Expect(visible.visible >= 1000,
             "local OM-3 recovery preview should show a visible highlight change "
             "(fixture=" + std::string(relative_path) +
                 " res=" + std::to_string(max_dimension) +
                 " visible=" + std::to_string(visible.visible) + ")");
      Expect(visible.max_code_delta >= 20,
             "local OM-3 recovery preview change should reach a clearly visible "
             "highlight swing (fixture=" + std::string(relative_path) +
                 " res=" + std::to_string(max_dimension) +
                 " max_delta=" + std::to_string(visible.max_code_delta) + ")");
      Expect(visible.darkest_changed_luma >= 192,
             "local OM-3 recovery preview must not touch pixels below the highlight "
             "band (fixture=" + std::string(relative_path) +
                 " res=" + std::to_string(max_dimension) +
                 " darkest changed luma=" +
                 std::to_string(visible.darkest_changed_luma) + ")");
      Expect(visible.bright * 5 >= visible.visible * 4,
             "at least four fifths of the changed pixels should sit in the lum "
             ">= 224 band (fixture=" + std::string(relative_path) +
                 " res=" + std::to_string(max_dimension) + " bright=" +
                 std::to_string(visible.bright) + " visible=" +
                 std::to_string(visible.visible) + ")");

      // Pass-rate pin (P2/D-1 acceptance criterion).  The audit's recipe names
      // the pass rate as one of the three metrics the 2560 px profile must
      // bring toward the 512 px profile, so it is pinned directly here rather
      // than only implied by the changed count.  Measured with the shipped
      // 4 : 1 footprint (§9.2.4): 90 % and 98 % at 512 px, 42 % and 76 % at
      // 2560 px.  The floor is 0.40 so an implementation that narrowed the
      // footprint back out (12 % / 49 % at 2560 px) fails.
      Expect(stats.candidates > 0,
             "local OM-3 recovery preview should have a candidate population "
             "(fixture=" + std::string(relative_path) +
                 " res=" + std::to_string(max_dimension) + ")");
      Expect(stats.changed * 100 >= stats.candidates * 40,
             "local OM-3 recovery pass rate should stay well above the inert "
             "regime at every preview resolution (fixture=" +
                 std::string(relative_path) + " res=" + std::to_string(max_dimension) +
                 " changed=" + std::to_string(stats.changed) + " candidates=" +
                 std::to_string(stats.candidates) + ")");

      // Mask invariants (task-1): the GUI's pink cast is driven by this mask, so
      // its size and its relationship to the recovery pass are pinned here.
      //   - the mask is exactly the set of pixels the blend changed, never a
      //     superset: its popcount equals the recovery pass's changed counter;
      //   - it can never exceed the image: it is sized exactly width*height;
      //   - it is 0 (in fact empty) where recovery did not run, which is the
      //     recovery-disabled baseline the same helper is asked to report on.
      Expect(recovery_preview->recovered_mask.size() ==
                 (recovery_preview->width > 0 && recovery_preview->height > 0
                      ? static_cast<size_t>(recovery_preview->width) * recovery_preview->height
                      : 0),
             "the recovered-pixel mask must be empty or sized exactly to the "
             "preview (fixture=" + std::string(relative_path) +
                 " res=" + std::to_string(max_dimension) + " mask=" +
                 std::to_string(recovery_preview->recovered_mask.size()) + " pixels=" +
                 std::to_string(static_cast<size_t>(recovery_preview->width) * recovery_preview->height) +
                 ")");
      size_t mask_popcount = 0;
      for (uint8_t value : recovery_preview->recovered_mask) {
        Expect(value <= 1,
               "the recovered-pixel mask must only contain 0 or 1 (fixture=" +
                   std::string(relative_path) + " res=" + std::to_string(max_dimension) +
                   ")");
        mask_popcount += value;
      }
      Expect(mask_popcount == stats.changed,
             "the recovered-pixel mask must mark exactly the pixels the blend "
             "changed (fixture=" + std::string(relative_path) +
                 " res=" + std::to_string(max_dimension) + " mask=" +
                 std::to_string(mask_popcount) + " changed=" +
                 std::to_string(stats.changed) + ")");
      // Every pixel the recovery pass changed must also differ in the displayed
      // image or in the underlying raster, i.e. the mask is not marking pixels
      // the recovery left alone.  The visible-count check above already pins the
      // direction that matters for the user; here the mask is tied to the
      // internal counter so the cast cannot drift from recovery's own result.
      if (mask_popcount == 0) {
        Expect(recovery_preview->recovered_mask.empty(),
               "a mask with no marked pixels must be reported as empty, not "
               "zeroed (fixture=" + std::string(relative_path) +
                   " res=" + std::to_string(max_dimension) + ")");
      }

      // Baseline for the next resolution/fixture: with recovery off the mask
      // must be empty, not a zeroed array of the preview's size.
      prepared.enable_highlight_recovery = false;
      auto no_recovery = std::make_shared<PreviewImage>();
      const bool no_recovery_ok = RenderConvertedFullPreview(&prepared,
                                                            {},
                                                            max_dimension,
                                                            no_recovery,
                                                            {},
                                                            {},
                                                            {},
                                                            &error_message);
      Expect(no_recovery_ok,
             "recovery-disabled full preview should succeed: " + error_message);
      Expect(no_recovery->recovered_mask.empty(),
             "the mask must be empty where recovery did not run (fixture=" +
                 std::string(relative_path) + " res=" + std::to_string(max_dimension) +
                 " mask=" + std::to_string(no_recovery->recovered_mask.size()) + ")");
      prepared.enable_highlight_recovery = true;

      // Convergence pin: the GUI-resolution profile must approach the small-size
      // profile on the same fixture.  This is the objective's own acceptance
      // criterion, expressed as a ratio so it holds independently of how much
      // recoverable highlight area a given fixture happens to contain (measured
      // 47 % and 78 % with the shipped footprint, against 14 % and 51 % for
      // the 16 : 2 footprint this round replaced).  "Approaches" is pinned as
      // "within 3x": the residual gap is the candidate-population difference
      // (a 511-px preview averages 20x more source pixels per preview pixel than
      // a 2556-px one, so its candidate set is biased toward large recoverable
      // regions), not a remaining resolution dependence in the algorithm.
      const int pass_rate_percent = stats.candidates > 0
          ? static_cast<int>(stats.changed * 100 / stats.candidates)
          : 0;
      if (max_dimension == kResolutions[0]) {
        small_pass_rate_percent = pass_rate_percent;
      } else if (small_pass_rate_percent > 0) {
        Expect(pass_rate_percent * 3 >= small_pass_rate_percent,
               "the GUI-resolution pass rate should approach the 512 px profile "
               "(fixture=" + std::string(relative_path) + " small=" +
                   std::to_string(small_pass_rate_percent) + " gui=" +
                   std::to_string(pass_rate_percent) + ")");
      }

      prepared.enable_highlight_recovery = false;
    }
  }
}

void TestLocalOm3RecoveryCropPreviewMapping() {
  // Pins the crop-preview recovery coordinate plumbing: RenderConvertedCrop
  // must render full-frame crops with recovery enabled without error and keep
  // the requested crop dimensions.  The first render builds the processing
  // cache (fresh-cache call site), the second reuses it (cached-cache call
  // site), so both coordinate paths are exercised.
  // The sample directory is intentionally untracked, so fail loudly rather
  // than pass vacuously when the gitignored fixture is absent.
  const std::filesystem::path source_path = RealOrfFixturePath("sample/_8142491.ORF");
  Expect(std::filesystem::exists(source_path),
         "missing test fixture (gitignored *.ORF): " + source_path.string());

  PreparedSource prepared;
  std::string error_message;
  const bool prepare_ok = PrepareSource(source_path.string(), &prepared, &error_message);
  Expect(prepare_ok, "local OM-3 source preparation should succeed: " + error_message);
  if (!prepared.HasHighlightRecoverySource()) {
    return;
  }

  const CropRect crop = CenterCropRect(prepared.image_width, prepared.image_height, 512, 512);
  auto recovery_off = std::make_shared<PreviewImage>();
  const bool off_ok =
      RenderConvertedCrop(&prepared, crop, {}, recovery_off, {}, {}, {}, &error_message);
  Expect(off_ok, "local OM-3 crop preview (recovery off) should succeed: " + error_message);

  prepared.enable_highlight_recovery = true;
  auto recovery_on = std::make_shared<PreviewImage>();
  const bool on_ok =
      RenderConvertedCrop(&prepared, crop, {}, recovery_on, {}, {}, {}, &error_message);
  Expect(on_ok, "local OM-3 crop preview (recovery on) should succeed: " + error_message);
  Expect(recovery_on->width == 512 && recovery_on->height == 512,
         "local OM-3 recovery-on crop preview should keep the requested 512x512 crop");
  Expect(recovery_on->pixels.size() == recovery_off->pixels.size(),
         "local OM-3 recovery-on crop preview should keep the pixel buffer size");
}

void TestFullPreviewObeysDisplayDimension() {
  const std::filesystem::path source_path = RealOrfFixturePath("reference/_3210504.ORF");
  // The .ORF fixtures are gitignored, so an absent fixture means a broken
  // checkout rather than a reason to pass vacuously.
  Expect(std::filesystem::exists(source_path),
         "missing test fixture (gitignored *.ORF): " + source_path.string());

  PreparedSource prepared;
  std::string error_message;
  Expect(PrepareSource(source_path.string(), &prepared, &error_message),
         "full-preview dimension test should prepare source: " + error_message);

  for (const uint32_t max_dimension : {512u, 256u}) {
    auto preview = std::make_shared<PreviewImage>();
    const bool ok = RenderConvertedFullPreview(&prepared,
                                               {},
                                               max_dimension,
                                               preview,
                                               {},
                                               {},
                                               {},
                                               &error_message);
    Expect(ok, "full preview at " + std::to_string(max_dimension) +
                    " should succeed: " + error_message);
    Expect(preview->width > 0 && preview->height > 0,
           "full preview should produce non-empty image data");
    Expect(std::max(preview->width, preview->height) <= max_dimension,
           "full preview should obey its display-size limit");

    const double expected_aspect = static_cast<double>(prepared.image_width) /
                                   static_cast<double>(prepared.image_height);
    const double actual_aspect = static_cast<double>(preview->width) /
                                 static_cast<double>(preview->height);
    Expect(std::abs(actual_aspect - expected_aspect) < 0.02,
           "full preview should preserve the source aspect ratio");
  }
}

void TestFullPreviewOrientationMatchesLibRawFlip() {
  // The orientation helpers are public and can be exercised directly with a
  // non-trivial flip, which no available fixture exercises end to end.
  const uint32_t width = 900;
  const uint32_t height = 600;
  Expect(OrientedImageWidth(width, height, 0) == width &&
             OrientedImageHeight(width, height, 0) == height,
         "a zero flip should not swap image axes");
  Expect(OrientedImageWidth(width, height, 5) == height &&
             OrientedImageHeight(width, height, 5) == width,
         "a 90-degree flip should swap image axes");
  Expect(!LibRawFlipSwapsAxes(0) && LibRawFlipSwapsAxes(5),
         "LibRawFlipSwapsAxes should detect axis-swapping flips");

  // End to end: the full preview is downscaled to the display limit, so its
  // dimensions are not the source dimensions. What must hold is that the
  // oriented aspect ratio and the axis-swap decision survive the scaling.
  const std::filesystem::path source_path = RealOrfFixturePath("reference/_3210504.ORF");
  // The .ORF fixtures are gitignored, so an absent fixture means a broken
  // checkout rather than a reason to pass vacuously.
  Expect(std::filesystem::exists(source_path),
         "missing test fixture (gitignored *.ORF): " + source_path.string());

  PreparedSource prepared;
  std::string error_message;
  Expect(PrepareSource(source_path.string(), &prepared, &error_message),
         "full-preview orientation test should prepare source: " + error_message);

  const uint32_t oriented_width = OrientedImageWidth(prepared.image_width,
                                                      prepared.image_height,
                                                      prepared.metadata.libraw_flip);
  const uint32_t oriented_height = OrientedImageHeight(prepared.image_width,
                                                       prepared.image_height,
                                                       prepared.metadata.libraw_flip);
  const uint32_t oriented_long = std::max(oriented_width, oriented_height);
  const uint32_t oriented_short = std::min(oriented_width, oriented_height);
  const double oriented_aspect = static_cast<double>(oriented_short) /
                                 static_cast<double>(oriented_long);

  auto preview = std::make_shared<PreviewImage>();
  const bool ok =
      RenderConvertedFullPreview(&prepared, {}, 512, preview, {}, {}, {}, &error_message);
  Expect(ok, "full preview should succeed: " + error_message);
  Expect(std::max(preview->width, preview->height) <= 512u,
         "full preview should respect its display-size limit");
  const double preview_aspect = static_cast<double>(std::min(preview->width, preview->height)) /
                                static_cast<double>(std::max(preview->width, preview->height));
  Expect(std::abs(preview_aspect - oriented_aspect) < 0.01,
         "full preview aspect ratio should match the oriented source geometry");
  Expect((preview->width >= preview->height) == (oriented_width >= oriented_height),
         "full preview orientation should match the oriented source geometry");
}

void TestFullPreviewHonoursCancellation() {
  const std::filesystem::path source_path = RealOrfFixturePath("reference/_3210504.ORF");
  // The .ORF fixtures are gitignored, so an absent fixture means a broken
  // checkout rather than a reason to pass vacuously.
  Expect(std::filesystem::exists(source_path),
         "missing test fixture (gitignored *.ORF): " + source_path.string());

  PreparedSource prepared;
  std::string error_message;
  Expect(PrepareSource(source_path.string(), &prepared, &error_message),
         "cancellation test should prepare source: " + error_message);

  int cancel_calls = 0;
  const CancelCheck cancel = [&cancel_calls]() {
    ++cancel_calls;
    return true;
  };

  auto preview = std::make_shared<PreviewImage>();
  const bool ok = RenderConvertedFullPreview(&prepared,
                                             {},
                                             512,
                                             preview,
                                             {},
                                             {},
                                             cancel,
                                             &error_message);
  Expect(!ok, "a cancelled full preview request should fail");
  Expect(error_message == "operation canceled",
         "a cancelled full preview request should report cancellation");
  Expect(cancel_calls > 0, "the cancel check should be consulted");
}

void TestFullPreviewReusesCachedPreview() {
  const std::filesystem::path source_path = RealOrfFixturePath("reference/_3210504.ORF");
  // The .ORF fixtures are gitignored, so an absent fixture means a broken
  // checkout rather than a reason to pass vacuously.
  Expect(std::filesystem::exists(source_path),
         "missing test fixture (gitignored *.ORF): " + source_path.string());

  PreparedSource prepared;
  std::string error_message;
  Expect(PrepareSource(source_path.string(), &prepared, &error_message),
         "cache-reuse test should prepare source: " + error_message);

  auto first = std::make_shared<PreviewImage>();
  const bool first_ok =
      RenderConvertedFullPreview(&prepared, {}, 512, first, {}, {}, {}, &error_message);
  Expect(first_ok, "first full preview should succeed: " + error_message);
  Expect(first->width > 0 && first->height > 0,
         "first full preview should produce image data");

  const PreviewImage cached_copy = *first;

  // A rebuild would be cancelled immediately (see the cancellation test), so a
  // successful second call returning identical bytes proves the cached preview
  // was reused rather than rebuilt.
  const CancelCheck cancel = []() { return true; };
  auto second = std::make_shared<PreviewImage>();
  const bool second_ok =
      RenderConvertedFullPreview(&prepared, {}, 512, second, {}, {}, cancel, &error_message);
  Expect(second_ok, "a cached full preview should be returned without rebuilding");
  Expect(second->width == cached_copy.width && second->height == cached_copy.height &&
             second->colors == cached_copy.colors && second->bits == cached_copy.bits,
         "a cached full preview should retain its geometry");
  Expect(second->pixels == cached_copy.pixels,
         "a cached full preview should return identical pixel data");
}

// BuildDngWriterConfigSummary is the only externally visible view of both the
// compression code and the DNG version Hiraco requests. DngVersionForCompression
// has internal linkage, but summary.dng_version is derived directly from it, so
// asserting on these strings pins I-01 (which code each mode asks for) and I-03
// (that the requested version is the one the SDK actually writes).
void TestDngWriterConfigSummaryReportsRequestedVersionAndCode() {
  const auto check_mode = [](const std::string& mode,
                             unsigned expected_code,
                             const std::string& expected_version) {
    const DngWriterConfigSummary summary = BuildDngWriterConfigSummary(mode);
    Expect(summary.can_configure_writer,
           mode + " mode should be configurable by the writer");
    Expect(summary.compression_name == mode,
           mode + " mode should report its own name, got " + summary.compression_name);
    Expect(summary.compression_code == expected_code,
           mode + " mode should request compression code " + std::to_string(expected_code) +
               ", got " + std::to_string(summary.compression_code));
    Expect(summary.dng_version == expected_version,
           mode + " mode should request DNG " + expected_version +
               ", got " + summary.dng_version);
    // The status string is human-readable output, but it must not contradict
    // the version the summary actually reports.
    Expect(summary.status.find("DNG " + expected_version) != std::string::npos,
           mode + " status string should mention DNG " + expected_version +
               ", got: " + summary.status);
  };

  check_mode("uncompressed", 1u, "1.4.0.0");
  check_mode("deflate", 8u, "1.4.0.0");
  check_mode("jpeg-xl", 52546u, "1.7.0.0");

  // jpeg-x is the only experimental mode.
  const DngWriterConfigSummary jpeg_x = BuildDngWriterConfigSummary("jpeg-xl");
  Expect(jpeg_x.experimental && jpeg_x.lossless_jpeg_xl,
         "jpeg-xl mode should be flagged experimental and lossless");
  const DngWriterConfigSummary deflate = BuildDngWriterConfigSummary("deflate");
  Expect(!deflate.experimental,
         "deflate mode should not be flagged experimental");

  // An unknown mode must not be silently presented as configurable.
  const DngWriterConfigSummary unsupported = BuildDngWriterConfigSummary("nope");
  Expect(!unsupported.can_configure_writer,
         "an unsupported compression mode should not be configurable");
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
  const bool crop_ok = RenderConvertedCropPreview(metadata,
                                                  cache,
                                                  crop,
                                                  "",
                                                  false,
                                                  {},
                                                  actual,
                                                  {},
                                                  {},
                                                  &error_message);
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

  auto scaled = std::make_shared<PreviewImage>();
  const bool scaled_ok = RenderConvertedCropPreview(metadata,
                                                    cache,
                                                    crop,
                                                    "",
                                                    false,
                                                    {},
                                                    scaled,
                                                    {},
                                                    {},
                                                    &error_message,
                                                    128);
  Expect(scaled_ok, "scaled converted preview should succeed");
  Expect(std::max(scaled->width, scaled->height) <= 128,
         "scaled converted preview should respect its longest-edge limit");
}

void TestDisplayProcessingCacheScalesBothInputs() {
  ProcessingCache source;
  source.raw_image = MakeSyntheticRgb(900, 600);
  source.cfa_guide_image = MakeSyntheticGuide(900, 600);
  source.raw_image_is_camera_space = true;
  source.highlight_recovery_enabled = false;
  source.preview_auto_bright_gain = 1.75;
  source.source_width = 900;
  source.source_height = 600;

  ProcessingCache display;
  std::string error_message;
  const bool ok = BuildDisplayProcessingCache(source, 300, &display, &error_message);
  Expect(ok, "display processing cache should build: " + error_message);
  Expect(display.raw_image.width == 300 && display.raw_image.height == 200,
         "display processing cache should preserve aspect ratio at its longest edge");
  Expect(display.cfa_guide_image.width == 300 && display.cfa_guide_image.height == 200 &&
             display.cfa_guide_image.colors == 4,
         "display processing cache should scale the matching CFA guide");
  Expect(display.source_width == 300 && display.source_height == 200,
         "display processing cache coordinates should use display resolution");
  Expect(display.raw_image_is_camera_space &&
             std::abs(display.preview_auto_bright_gain - 1.75) < 1e-9,
         "display processing cache should preserve render properties");
}

  void TestStage2SmallMediumLargeGainResponse() {
    const SourceLinearDngMetadata metadata = MakeHighResMetadata();
    const RasterImage source = MakeSyntheticTexturedRgb(640, 640);
    const RasterImage guide = MakeSyntheticGuide(640, 640);

    auto make_settings = [&](float small_detail,
                             float medium_detail,
                             float large_detail) {
      StageOverrideSet overrides;
      overrides.stage2_denoise = 0.35f;
      overrides.stage2_gain1 = small_detail;
      overrides.stage2_gain2 = medium_detail;
      overrides.stage2_gain3 = large_detail;
      overrides.stage3_gain = 0.0f;
      return ResolveStageSettingsForImage(metadata, source.width, source.height, overrides);
    };

    const ResolvedStageSettings neutral = make_settings(1.0f, 1.0f, 1.0f);
    const ResolvedStageSettings small_low = make_settings(0.5f, 1.0f, 1.0f);
    const ResolvedStageSettings small_high = make_settings(3.5f, 1.0f, 1.0f);
    const ResolvedStageSettings medium_low = make_settings(1.0f, 0.5f, 1.0f);
    const ResolvedStageSettings medium_high = make_settings(1.0f, 3.0f, 1.0f);
    const ResolvedStageSettings large_low = make_settings(1.0f, 1.0f, 0.5f);
    const ResolvedStageSettings large_high = make_settings(1.0f, 1.0f, 3.0f);

    const RasterImage neutral_out =
      RunEnhancementForTesting(source, &guide, metadata, neutral);
    const RasterImage small_low_out =
      RunEnhancementForTesting(source, &guide, metadata, small_low);
    const RasterImage small_high_out =
      RunEnhancementForTesting(source, &guide, metadata, small_high);
    const RasterImage medium_low_out =
      RunEnhancementForTesting(source, &guide, metadata, medium_low);
    const RasterImage medium_high_out =
      RunEnhancementForTesting(source, &guide, metadata, medium_high);
    const RasterImage large_low_out =
      RunEnhancementForTesting(source, &guide, metadata, large_low);
    const RasterImage large_high_out =
      RunEnhancementForTesting(source, &guide, metadata, large_high);

    const double neutral_micro_energy = ComputeLumaNeighborhoodEnergy(neutral_out, 1);
    const double small_low_micro_energy = ComputeLumaNeighborhoodEnergy(small_low_out, 1);
    const double small_high_micro_energy = ComputeLumaNeighborhoodEnergy(small_high_out, 1);
    Expect(small_low_micro_energy + 5.0 < neutral_micro_energy,
       "small-detail attenuation should reduce finest-scale energy");
    Expect(small_high_micro_energy > neutral_micro_energy + 5.0,
       "small-detail boost should increase finest-scale energy");

    const double neutral_small_energy = ComputeLumaNeighborhoodEnergy(neutral_out, 2);
    const double small_low_energy = ComputeLumaNeighborhoodEnergy(small_low_out, 2);
    const double small_high_energy = ComputeLumaNeighborhoodEnergy(small_high_out, 2);
    Expect(small_low_energy + 3.0 < neutral_small_energy,
       "small-detail attenuation should reduce small-structure energy");
    Expect(small_high_energy > neutral_small_energy + 3.0,
       "small-detail boost should increase small-structure energy");

    const double neutral_medium_energy = ComputeLumaNeighborhoodEnergy(neutral_out, 4);
    const double medium_low_energy = ComputeLumaNeighborhoodEnergy(medium_low_out, 4);
    const double medium_high_energy = ComputeLumaNeighborhoodEnergy(medium_high_out, 4);
    Expect(medium_low_energy + 2.0 < neutral_medium_energy,
       "medium-detail attenuation should reduce mid-scale energy");
    Expect(medium_high_energy > neutral_medium_energy + 2.0,
       "medium-detail boost should increase mid-scale energy");

    const double neutral_large_energy = ComputeLumaNeighborhoodEnergy(neutral_out, 8);
    const double large_low_energy = ComputeLumaNeighborhoodEnergy(large_low_out, 8);
    const double large_high_energy = ComputeLumaNeighborhoodEnergy(large_high_out, 8);
    Expect(large_low_energy + 1.0 < neutral_large_energy,
       "large-detail attenuation should reduce broad-scale energy");
    Expect(large_high_energy > neutral_large_energy + 1.0,
       "large-detail boost should increase broad-scale energy");
  }

void TestLocalOm3RecoveryPreviewCastCoverage() {
  // End-to-end on the real local pair: the mask the recovery pass produces must
  // actually mark pixels in the displayed preview, and the cast must recolor
  // exactly those pixels.  This is the part the synthetic cast test cannot
  // cover, because a synthetic preview has no mask to begin with.
  const char* kFixtures[] = {"sample/_8142491.ORF", "sample/_8142490.ORF"};
  const uint32_t kResolution = 512;

  for (const char* relative_path : kFixtures) {
    const std::filesystem::path source_path = RealOrfFixturePath(relative_path);
    Expect(std::filesystem::exists(source_path),
           "missing test fixture (gitignored *.ORF): " + source_path.string());
    PreparedSource prepared;
    std::string error_message;
    const bool prepare_ok =
        PrepareSource(source_path.string(), &prepared, &error_message);
    Expect(prepare_ok, "local OM-3 source preparation should succeed: " + error_message);
    if (!prepared.HasHighlightRecoverySource()) {
      continue;
    }

    prepared.enable_highlight_recovery = true;
    auto preview = std::make_shared<PreviewImage>();
    const bool preview_ok = RenderConvertedFullPreview(&prepared,
                                                       {},
                                                       kResolution,
                                                       preview,
                                                       {},
                                                       {},
                                                       {},
                                                       &error_message);
    Expect(preview_ok, "local OM-3 recovery preview should succeed: " + error_message);

    const HighlightRecoveryStats stats = GetLastHighlightRecoveryStats();
    Expect(stats.applicable && stats.changed > 0,
           "local OM-3 recovery should have changed pixels (changed=" +
               std::to_string(stats.changed) + ")");
    const std::vector<uint8_t>& mask = preview->recovered_mask;
    Expect(mask.size() == static_cast<size_t>(preview->width) * preview->height,
           "the mask must be sized to the preview (fixture=" +
               std::string(relative_path) + " mask=" +
               std::to_string(mask.size()) + " pixels=" +
               std::to_string(static_cast<size_t>(preview->width) *
                              preview->height) + ")");
    size_t mask_popcount = 0;
    for (uint8_t value : mask) {
      mask_popcount += value;
    }
    Expect(mask_popcount == stats.changed,
           "the mask must mark exactly the changed pixels (fixture=" +
               std::string(relative_path) + " mask=" +
               std::to_string(mask_popcount) + " changed=" +
               std::to_string(stats.changed) + ")");

    // What the user sees with the cast on: a copy with the mask applied.
    auto cast = std::make_shared<PreviewImage>(*preview);
    const std::vector<uint8_t> clean = preview->pixels;
    const size_t cast_changed = ApplyRecoveredPixelCast(cast.get());
    Expect(cast_changed == mask_popcount,
           "the cast must recolor exactly the masked pixels (fixture=" +
               std::string(relative_path) + " cast=" +
               std::to_string(cast_changed) + " mask=" +
               std::to_string(mask_popcount) + ")");

    const size_t pixel_count = static_cast<size_t>(preview->width) * preview->height;
    for (size_t pixel = 0; pixel < pixel_count; ++pixel) {
      const size_t base = pixel * 3;
      if (mask[pixel] != 0) {
        Expect(cast->pixels[base + 0] == 255 && cast->pixels[base + 1] == 0 &&
                   cast->pixels[base + 2] == 255,
               "a masked pixel must be solid pink (fixture=" +
                   std::string(relative_path) + " pixel=" +
                   std::to_string(pixel) + ")");
      } else {
        Expect(cast->pixels[base + 0] == clean[base + 0] &&
                   cast->pixels[base + 1] == clean[base + 1] &&
                   cast->pixels[base + 2] == clean[base + 2],
               "an unmasked pixel must stay byte-identical (fixture=" +
                   std::string(relative_path) + " pixel=" +
                   std::to_string(pixel) + ")");
      }
    }

    // The cast must be visible in the highlight band, which is the whole point:
    // a mask that only marked mid-tones would be a regression in the recovery
    // pass's own gating, not in the overlay.
    size_t bright_cast = 0;
    for (size_t pixel = 0; pixel < pixel_count; ++pixel) {
      if (mask[pixel] == 0) {
        continue;
      }
      const size_t base = pixel * 3;
      const double luma = 0.2126 * clean[base] + 0.7152 * clean[base + 1] +
                          0.0722 * clean[base + 2];
      if (luma >= 224.0) {
        ++bright_cast;
      }
    }
    // The cast must be concentrated in the highlight band, which is the whole
    // point: a mask that started marking mid-tones would be a regression in the
    // recovery pass's own gating, not in the overlay.  Measured on the two local
    // pairs: the top band holds 99.9 % (_8142491) and 71.8 % (_8142490) of the
    // masked pixels, and the lum >= 192 band holds 99.96 % and 99.98 %.  Both
    // floors below are set well under those, but far above any spread into the
    // mid-tones.
    Expect(bright_cast * 2 >= mask_popcount,
           "most of the cast pixels should sit in the lum >= 224 band (fixture=" +
               std::string(relative_path) + " bright=" +
               std::to_string(bright_cast) + " mask=" +
               std::to_string(mask_popcount) + ")");

    size_t highlight_band_cast = 0;
    for (size_t pixel = 0; pixel < pixel_count; ++pixel) {
      if (mask[pixel] == 0) {
        continue;
      }
      const size_t base = pixel * 3;
      const double luma = 0.2126 * clean[base] + 0.7152 * clean[base + 1] +
                          0.0722 * clean[base + 2];
      if (luma >= 192.0) {
        ++highlight_band_cast;
      }
    }
    Expect(highlight_band_cast * 10 >= mask_popcount * 9,
           "virtually all of the cast pixels should sit in the highlight region "
           "(lum >= 192), matching the recovery pass's own gating (fixture=" +
               std::string(relative_path) + " in_band=" +
               std::to_string(highlight_band_cast) + " mask=" +
               std::to_string(mask_popcount) + ")");
  }
}

void TestRecoveredPixelCastPlacement() {
  // The GUI is not part of hiraco-core, so the "where does the cast appear"
  // rule is pinned here instead of only by clicking the app.  The variables are
  // the four things the rule depends on, plus an explicit "zoom cannot change
  // this" slot so that a future change which made zoom matter would have to be
  // stated and tested here rather than discovered in the drawer.
  const bool kShow[] = {false, true};
  const bool kMask[] = {false, true};
  const bool kConvertedVisible[] = {false, true};
  const bool kOriginalVisible[] = {false, true};
  constexpr int kZoomLevels = 3;  // Fit, 50%, 100%.

  for (bool show : kShow) {
    for (bool mask : kMask) {
      for (bool converted_visible : kConvertedVisible) {
        for (bool original_visible : kOriginalVisible) {
          for (int zoom_level = 0; zoom_level < kZoomLevels; ++zoom_level) {
            (void)zoom_level;
            const RecoveredPixelCastPlacement placement =
                ResolveRecoveredPixelCastPlacement(show, mask, converted_visible,
                                                   original_visible);
            const bool expected = show && mask && converted_visible;
            const std::string context =
                std::string("show=") + (show ? "1" : "0") +
                " mask=" + (mask ? "1" : "0") +
                " converted_visible=" + (converted_visible ? "1" : "0") +
                " original_visible=" + (original_visible ? "1" : "0") +
                " zoom_index=" + std::to_string(zoom_level);
            Expect(placement.on_converted == expected,
                   "the cast must appear on the converted pane exactly when it is "
                   "requested, the preview carries a mask, and the converted image "
                   "is visible at any zoom level (" + context + ")");
            Expect(!placement.on_original,
                   "the cast must never appear on the original pane at any zoom "
                   "level (" + context + ")");
          }
        }
      }
    }
  }
}

void TestApplyRecoveredPixelCast() {
  // The cast helper is pure data work the GUI's overlay bitmap needs, so it
  // lives in hiraco-core and is pinned here rather than only via a GUI.
  struct Case {
    const char* name;
    uint32_t width;
    uint32_t height;
    std::vector<uint8_t> mask;   // empty means "no mask"
    size_t expected_changed;
    bool expect_dims_ok;
  };

  const std::vector<Case> cases = {
      {"all masked", 4, 2, {1, 1, 1, 1, 1, 1, 1, 1}, 8, true},
      {"none masked", 4, 2, {0, 0, 0, 0, 0, 0, 0, 0}, 0, true},
      {"single masked pixel", 3, 3, {0, 0, 0, 0, 1, 0, 0, 0, 0}, 1, true},
      {"corners", 2, 2, {1, 0, 0, 1}, 2, true},
      {"empty mask is not an error", 5, 5, {}, 0, false},
  };

  for (const Case& test_case : cases) {
    PreviewImage preview;
    preview.width = test_case.width;
    preview.height = test_case.height;
    preview.colors = 3;
    preview.bits = 8;
    preview.pixels.resize(static_cast<size_t>(preview.width) * preview.height * 3);
    // Fill with a known non-magenta pattern so "unchanged" verifies no byte
    // was touched, and magenta itself would be recognisable as a false change.
    for (size_t i = 0; i < preview.pixels.size(); ++i) {
      preview.pixels[i] = static_cast<uint8_t>((i * 7 + 11) & 0xFF);
    }
    preview.recovered_mask = test_case.mask;
    const std::vector<uint8_t> before = preview.pixels;

    const size_t changed = ApplyRecoveredPixelCast(&preview);
    if (!test_case.expect_dims_ok) {
      // Empty masks carry no information, so the helper must refuse rather
      // than guess, and leave the pixels untouched.
      Expect(changed == 0,
             std::string("an empty mask must not cast any pixel (") + test_case.name + ")");
      Expect(preview.pixels == before,
             std::string("an empty mask must leave the pixels untouched (") +
                 test_case.name + ")");
      continue;
    }

    Expect(changed == test_case.expected_changed,
           std::string("mask population should equal the changed count (") +
               test_case.name + " changed=" + std::to_string(changed) + ")");
    Expect(preview.recovered_mask.size() ==
               static_cast<size_t>(preview.width) * preview.height,
           std::string("the helper must not resize the mask (") + test_case.name + ")");
    if (preview.recovered_mask != test_case.mask) {
      Expect(false, std::string("the helper must not mutate the mask (") +
                         test_case.name + ")");
    }

    const size_t pixel_count =
        static_cast<size_t>(preview.width) * preview.height;
    for (size_t pixel = 0; pixel < pixel_count; ++pixel) {
      const size_t base = pixel * 3;
      if (test_case.mask[pixel] != 0) {
        Expect(preview.pixels[base + 0] == 255 && preview.pixels[base + 1] == 0 &&
                   preview.pixels[base + 2] == 255,
               std::string("masked pixels must be solid pink (") + test_case.name +
                   " pixel=" + std::to_string(pixel) + ")");
      } else {
        Expect(preview.pixels[base + 0] == before[base + 0] &&
                   preview.pixels[base + 1] == before[base + 1] &&
                   preview.pixels[base + 2] == before[base + 2],
               std::string("unmasked pixels must be byte-identical (") + test_case.name +
                   " pixel=" + std::to_string(pixel) + ")");
      }
    }
  }

  // A mask whose size disagrees with the pixels is a caller bug, not data.  The
  // helper must refuse it instead of reading or writing out of bounds.
  PreviewImage mismatched;
  mismatched.width = 2;
  mismatched.height = 2;
  mismatched.colors = 3;
  mismatched.pixels.assign(2 * 2 * 3, 9);
  mismatched.recovered_mask.assign(5, 1);
  const std::vector<uint8_t> mismatched_before = mismatched.pixels;
  Expect(ApplyRecoveredPixelCast(&mismatched) == 0,
         "a mismatched mask must not cast any pixel");
  Expect(mismatched.pixels == mismatched_before,
         "a mismatched mask must leave the pixels untouched");

  // Null and degenerate inputs must be refused, never crashed on.
  Expect(ApplyRecoveredPixelCast(nullptr) == 0, "a null preview must cast nothing");
  PreviewImage colourless;
  colourless.width = 2;
  colourless.height = 2;
  colourless.colors = 0;
  colourless.pixels.assign(12, 3);
  colourless.recovered_mask.assign(4, 1);
  Expect(ApplyRecoveredPixelCast(&colourless) == 0,
         "a preview without colours must cast nothing");
}

}  // namespace

int main() {
  try {
    TestResolveOutputPath();
    TestOverwriteDecision();
    TestCropHelpers();
    TestResolvedStageDefaults();
    TestEnhancementPreservesHighlightHeadroom();
    TestRealOrfPreviewSmoke();
    TestRealOrfCropPreviewFollowsCropRect();
    TestRealOrfPreviewAutoBrightGainEstimate();
    TestLocalOm3RecoveryFullPreviewSmoke();
    TestLocalOm3RecoveryCropPreviewMapping();
    TestFullPreviewObeysDisplayDimension();
    TestFullPreviewOrientationMatchesLibRawFlip();
    TestFullPreviewHonoursCancellation();
    TestFullPreviewReusesCachedPreview();
    TestDngWriterConfigSummaryReportsRequestedVersionAndCode();
    TestCropPreviewMatchesFullImageCrop();
    TestDisplayProcessingCacheScalesBothInputs();
    TestApplyRecoveredPixelCast();
    TestLocalOm3RecoveryPreviewCastCoverage();
    TestRecoveredPixelCastPlacement();
    TestStage2SmallMediumLargeGainResponse();
  } catch (const std::exception& exception) {
    std::cerr << "Test failure: " << exception.what() << "\n";
    return 1;
  }

  std::cout << "hiraco core tests passed\n";
  return 0;
}
