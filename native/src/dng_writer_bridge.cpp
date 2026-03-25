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
#include <cmath>
#include <cstdlib>
#include <filesystem>
#include <fftw3.h>
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
  RasterImage cfa_guide_image;
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
    settings.user_qual = 12;
    settings.four_color_rgb = 0;
    settings.green_matching = 0;
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
    settings.user_qual = 12;
    settings.four_color_rgb = 0;
    settings.green_matching = 0;
  }

  ApplyLibRawEnvironmentOverrides(&settings);
  return settings;
}

RenderSettings BuildCfaGuideRenderSettings(const SourceLinearDngMetadata& metadata) {
  return BuildRawRenderSettings(metadata);
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
                              const RasterImage* cfa_guide_image,
                              RasterImage* image) {
  if (!ShouldApplyPredictedDetailGain(metadata, *image)) {
    return;
  }

  const double base_gain = std::clamp(metadata.predicted_detail_gain, 1.0, 5.0);
  const uint32_t width = image->width;
  const uint32_t height = image->height;
  const uint32_t colors = image->colors;
  const size_t pixel_count = static_cast<size_t>(width) * height;

  if (colors != 3) {
    return;
  }

  // BT.709 luma coefficients for linear light.
  constexpr double kLumaR = 0.2126;
  constexpr double kLumaG = 0.7152;
  constexpr double kLumaB = 0.0722;

  // --- Stage 0: Extract luma channel (BT.709) ---
  std::vector<double> luma(pixel_count);
  for (size_t i = 0; i < pixel_count; ++i) {
    const size_t px = i * colors;
    luma[i] = kLumaR * image->pixels[px] +
              kLumaG * image->pixels[px + 1] +
              kLumaB * image->pixels[px + 2];
  }

  // Keep a copy of the original luma for ratio transfer.
  std::vector<double> original_luma(luma);

  std::vector<double> confidence(pixel_count, 1.0);
  if (cfa_guide_image != nullptr &&
      cfa_guide_image->width == width &&
      cfa_guide_image->height == height &&
      cfa_guide_image->colors == 4) {
    auto blur3 = [&](const std::vector<double>& src, std::vector<double>& dst) {
      std::vector<double> tmp(pixel_count);
      for (uint32_t row = 0; row < height; ++row) {
        const size_t row_off = static_cast<size_t>(row) * width;
        for (uint32_t col = 0; col < width; ++col) {
          const uint32_t c0 = (col > 0) ? col - 1 : 0;
          const uint32_t c2 = (col + 1 < width) ? col + 1 : width - 1;
          tmp[row_off + col] = 0.25 * src[row_off + c0] +
                               0.50 * src[row_off + col] +
                               0.25 * src[row_off + c2];
        }
      }

      for (uint32_t col = 0; col < width; ++col) {
        for (uint32_t row = 0; row < height; ++row) {
          const uint32_t r0 = (row > 0) ? row - 1 : 0;
          const uint32_t r2 = (row + 1 < height) ? row + 1 : height - 1;
          const size_t idx = static_cast<size_t>(row) * width + col;
          dst[idx] = 0.25 * tmp[static_cast<size_t>(r0) * width + col] +
                     0.50 * tmp[idx] +
                     0.25 * tmp[static_cast<size_t>(r2) * width + col];
        }
      }
    };

    std::vector<double> green_split(pixel_count);
    std::vector<double> green_mean(pixel_count);
    for (size_t i = 0; i < pixel_count; ++i) {
      const size_t px = i * cfa_guide_image->colors;
      const double green_a = cfa_guide_image->pixels[px + 1];
      const double green_b = cfa_guide_image->pixels[px + 3];
      green_split[i] = std::abs(green_a - green_b);
      green_mean[i] = 0.5 * (green_a + green_b);
    }

    std::vector<double> smooth_split(pixel_count);
    std::vector<double> smooth_green(pixel_count);
    blur3(green_split, smooth_split);
    blur3(green_mean, smooth_green);

    std::vector<double> green_residual(pixel_count);
    for (size_t i = 0; i < pixel_count; ++i) {
      const double split_hp = std::max(green_split[i] - smooth_split[i], 0.0);
      const double texture_scale = std::max(smooth_green[i], 256.0);
      green_residual[i] = split_hp / texture_scale;
    }

    std::vector<double> sorted_residual(green_residual);
    std::nth_element(sorted_residual.begin(),
                     sorted_residual.begin() + pixel_count / 2,
                     sorted_residual.end());
    const double median_residual = sorted_residual[pixel_count / 2];
    const double residual_scale = std::max(8.0 * median_residual, 0.0035);

    for (size_t i = 0; i < pixel_count; ++i) {
      const double norm = green_residual[i] / residual_scale;
      const double atten = std::exp(-(norm * norm));
      confidence[i] = std::clamp(atten, 0.15, 1.0);
    }

    // Suppress single-pixel mask speckle.
    std::vector<double> smooth_conf(pixel_count);
    blur3(confidence, smooth_conf);
    blur3(smooth_conf, confidence);
  }

  // --- Stage 1: Wiener deconvolution (FFT-based) ---
  // The sensor-shift composite has a near-Gaussian PSF with no frequency-
  // domain zeros, making Wiener deconvolution well-posed.
  // Uses mirror/reflection padding to avoid boundary discontinuities that
  // produce periodic stipple artifacts with zero-padding.
  {
    const double psf_sigma = 1.0;
    const double nsr = 0.004;

    // Mirror-pad margins — enough to cover the PSF support and avoid
    // wrap-around artefacts.  32 px border is ample for σ ≤ 2.
    constexpr uint32_t kPadMargin = 32;
    const uint32_t padded_w = width + 2 * kPadMargin;
    const uint32_t padded_h = height + 2 * kPadMargin;

    // Round up to efficient FFT size (next power of 2).
    uint32_t fft_w = 1;
    while (fft_w < padded_w) fft_w <<= 1;
    uint32_t fft_h = 1;
    while (fft_h < padded_h) fft_h <<= 1;

    const size_t fft_n = static_cast<size_t>(fft_w) * fft_h;
    const size_t complex_n = static_cast<size_t>(fft_h) * (fft_w / 2 + 1);

    double* fft_in = fftw_alloc_real(fft_n);
    fftw_complex* fft_out = fftw_alloc_complex(complex_n);

    if (fft_in && fft_out) {
      // Fill the padded buffer using reflection at the image boundaries.
      std::fill(fft_in, fft_in + fft_n, 0.0);
      for (uint32_t pr = 0; pr < padded_h; ++pr) {
        // Reflect row index into [0, height-1].
        int sr = static_cast<int>(pr) - static_cast<int>(kPadMargin);
        if (sr < 0) sr = -sr;
        if (sr >= static_cast<int>(height)) sr = 2 * static_cast<int>(height) - 2 - sr;
        sr = std::clamp(sr, 0, static_cast<int>(height) - 1);

        const size_t dst_row = static_cast<size_t>(pr) * fft_w;
        const size_t src_row = static_cast<size_t>(sr) * width;

        for (uint32_t pc = 0; pc < padded_w; ++pc) {
          int sc = static_cast<int>(pc) - static_cast<int>(kPadMargin);
          if (sc < 0) sc = -sc;
          if (sc >= static_cast<int>(width)) sc = 2 * static_cast<int>(width) - 2 - sc;
          sc = std::clamp(sc, 0, static_cast<int>(width) - 1);

          fft_in[dst_row + pc] = luma[src_row + sc];
        }
        // Remaining columns (padded_w..fft_w-1) stay zero — they are
        // outside the reflected region but the FFT size may be larger.
      }

      fftw_plan plan_fwd = fftw_plan_dft_r2c_2d(
          static_cast<int>(fft_h), static_cast<int>(fft_w),
          fft_in, fft_out, FFTW_ESTIMATE);

      if (plan_fwd) {
        fftw_execute(plan_fwd);
        fftw_destroy_plan(plan_fwd);

        // Apply Wiener filter in frequency domain, combined with a CFA
        // frequency notch to suppress Bayer pattern residual.
        //
        // The demosaiced green channel retains Gr/Gb residual at the CFA
        // frequency (0.5 cycles/pixel in each axis).  Since luma is ~71%
        // green, the Wiener gain would amplify this into a visible dotted
        // pattern.  A narrow Gaussian notch at the three CFA frequencies
        // — (0.5, 0), (0, 0.5), (0.5, 0.5) — kills the pattern while
        // leaving all other frequencies untouched.
        const double two_pi2_sigma2 = 2.0 * M_PI * M_PI * psf_sigma * psf_sigma;
        const uint32_t half_w = fft_w / 2 + 1;

        // CFA notch: Gaussian dip centered at normalized freq 0.5 in
        // each axis.  σ_notch controls width; 0.04 ≈ ±4 % of Nyquist.
        constexpr double kNotchSigma = 0.04;
        constexpr double kNotchInvTwoSigma2 = 1.0 / (2.0 * kNotchSigma * kNotchSigma);

        for (uint32_t row = 0; row < fft_h; ++row) {
          double fy = static_cast<double>(row);
          if (fy > fft_h / 2.0) fy -= fft_h;
          fy /= fft_h;  // normalized: [-0.5, 0.5)

          for (uint32_t col = 0; col < half_w; ++col) {
            const double fx = static_cast<double>(col) / fft_w;

            // Wiener filter for PSF deconvolution.
            const double freq_sq = fx * fx + fy * fy;
            const double H = std::exp(-two_pi2_sigma2 * freq_sq);
            const double H2 = H * H;
            const double wiener = H / (H2 + nsr);

            // CFA notch: suppress (0.5,0), (0,0.5), (0.5,0.5).
            const double dfx_half = fx - 0.5;
            const double dfy_half = std::abs(fy) - 0.5;  // fy or -fy → both handled
            const double notch_h  = std::exp(-(dfx_half * dfx_half) * kNotchInvTwoSigma2);  // near (0.5, 0)
            const double notch_v  = std::exp(-(dfy_half * dfy_half) * kNotchInvTwoSigma2);  // near (0, 0.5)
            const double notch_d  = std::exp(-(dfx_half * dfx_half + dfy_half * dfy_half) * kNotchInvTwoSigma2);  // near (0.5, 0.5)
            // Combined notch: 1 − max(individual notches)
            const double notch_atten = 1.0 - std::max({notch_h * (std::exp(-(fy * fy) * kNotchInvTwoSigma2)),
                                                         notch_v * (std::exp(-(fx * fx) * kNotchInvTwoSigma2)),
                                                         notch_d});
            const double cfa_notch = std::max(notch_atten, 0.0);

            const size_t idx = static_cast<size_t>(row) * half_w + col;
            fft_out[idx][0] *= wiener * cfa_notch;
            fft_out[idx][1] *= wiener * cfa_notch;
          }
        }

        fftw_plan plan_inv = fftw_plan_dft_c2r_2d(
            static_cast<int>(fft_h), static_cast<int>(fft_w),
            fft_out, fft_in, FFTW_ESTIMATE);

        if (plan_inv) {
          fftw_execute(plan_inv);
          fftw_destroy_plan(plan_inv);

          // Extract the central (un-padded) region, normalised.
          const double norm = 1.0 / static_cast<double>(fft_n);
          for (uint32_t row = 0; row < height; ++row) {
            const size_t src_offset =
                static_cast<size_t>(row + kPadMargin) * fft_w + kPadMargin;
            const size_t dst_offset = static_cast<size_t>(row) * width;
            for (uint32_t col = 0; col < width; ++col) {
              luma[dst_offset + col] = fft_in[src_offset + col] * norm;
            }
          }

          for (size_t i = 0; i < pixel_count; ++i) {
            luma[i] = original_luma[i] + confidence[i] * (luma[i] - original_luma[i]);
          }
        }
      }
    }

    if (fft_out) fftw_free(fft_out);
    if (fft_in) fftw_free(fft_in);
  }

  // --- Stage 2: Multi-scale à trous wavelet detail enhancement ---
  // Operates at full resolution at every scale using dilated B3-spline
  // convolution.  Each scale captures a different frequency band and
  // receives an independent gain.
  {
    constexpr int kNumScales = 4;
    // Fine-scale gains — slightly reduced from v1 to avoid noise amplification.
    const double scale_gains[kNumScales] = {1.4, 1.25, 1.1, 1.0};

    // B3-spline 1D kernel: [1, 4, 6, 4, 1] / 16
    constexpr int kHalf = 2;
    constexpr double kKernel[5] = {1.0 / 16.0, 4.0 / 16.0, 6.0 / 16.0,
                                   4.0 / 16.0, 1.0 / 16.0};

    std::vector<double> approx_prev(luma);
    std::vector<double> detail_accum(pixel_count, 0.0);
    std::vector<double> h_pass(pixel_count);
    std::vector<double> approx_cur(pixel_count);

    for (int scale = 0; scale < kNumScales; ++scale) {
      const int step = 1 << scale;  // dilation: 1, 2, 4, 8

      // Horizontal pass.
      for (uint32_t row = 0; row < height; ++row) {
        const size_t row_off = static_cast<size_t>(row) * width;
        for (uint32_t col = 0; col < width; ++col) {
          double sum = 0.0;
          for (int k = -kHalf; k <= kHalf; ++k) {
            int sc = static_cast<int>(col) + k * step;
            if (sc < 0) sc = 0;
            if (sc >= static_cast<int>(width)) sc = static_cast<int>(width) - 1;
            sum += kKernel[k + kHalf] * approx_prev[row_off + sc];
          }
          h_pass[row_off + col] = sum;
        }
      }

      // Vertical pass.
      for (uint32_t col = 0; col < width; ++col) {
        for (uint32_t row = 0; row < height; ++row) {
          double sum = 0.0;
          for (int k = -kHalf; k <= kHalf; ++k) {
            int sr = static_cast<int>(row) + k * step;
            if (sr < 0) sr = 0;
            if (sr >= static_cast<int>(height)) sr = static_cast<int>(height) - 1;
            sum += kKernel[k + kHalf] * h_pass[static_cast<size_t>(sr) * width + col];
          }
          approx_cur[static_cast<size_t>(row) * width + col] = sum;
        }
      }

      // Detail = previous_approx - current_approx.
      // Apply soft noise thresholding (BayesShrink-style) at finest
      // scales to suppress residual Wiener deconvolution noise.
      const double extra_gain = scale_gains[scale] - 1.0;
      if (extra_gain > 1e-6) {
        // Estimate noise σ at this scale using robust MAD estimator.
        // σ_noise ≈ median(|detail|) / 0.6745
        std::vector<double> abs_details(pixel_count);
        for (size_t i = 0; i < pixel_count; ++i) {
          abs_details[i] = std::abs(approx_prev[i] - approx_cur[i]);
        }
        std::nth_element(abs_details.begin(),
                         abs_details.begin() + pixel_count / 2,
                         abs_details.end());
        const double median_abs = abs_details[pixel_count / 2];
        const double sigma_noise = median_abs / 0.6745;
        // Soft threshold = σ²_noise / σ_signal
        // with σ_signal estimated from the detail coefficients.
        double sum_sq = 0.0;
        for (size_t i = 0; i < pixel_count; ++i) {
          const double d = approx_prev[i] - approx_cur[i];
          sum_sq += d * d;
        }
        const double sigma_sq_total =
            sum_sq / static_cast<double>(pixel_count);
        const double sigma_sq_noise = sigma_noise * sigma_noise;
        const double sigma_sq_signal =
            std::max(sigma_sq_total - sigma_sq_noise, 1e-10);
        // Use 60% of BayesShrink threshold for gentler denoising that
        // preserves more genuine detail.
        const double threshold =
            0.6 * sigma_sq_noise / std::sqrt(sigma_sq_signal);

        for (size_t i = 0; i < pixel_count; ++i) {
          double detail = approx_prev[i] - approx_cur[i];
          // Soft-threshold: shrink toward zero.
          if (detail > threshold)
            detail -= threshold;
          else if (detail < -threshold)
            detail += threshold;
          else
            detail = 0.0;

          detail_accum[i] += extra_gain * confidence[i] * detail;
        }
      }

      approx_prev.swap(approx_cur);
    }

    // Add accumulated wavelet detail to the (Wiener-deconvolved) luma.
    for (size_t i = 0; i < pixel_count; ++i) {
      luma[i] += detail_accum[i];
    }
  }

  // --- Stage 3: Guided-filter based edge-aware refinement ---
  // Applies a final sharpening pass using a guided filter (self-guided)
  // as the smoothing base, avoiding halos at strong edges.
  {
    constexpr int kGfRadius = 6;
    constexpr double kGfEps = 0.001 * 65535.0 * 65535.0;
    const double gf_gain = 0.35 * (base_gain - 1.0);

    // Integral images for O(1)-per-pixel box filtering.
    const size_t integral_w = static_cast<size_t>(width) + 1;
    const size_t integral_h = static_cast<size_t>(height) + 1;
    const size_t integral_n = integral_w * integral_h;

    std::vector<double> sum_I(integral_n, 0.0);
    std::vector<double> sum_II(integral_n, 0.0);

    // Build integral images for luma (I) and I*I.
    for (uint32_t row = 0; row < height; ++row) {
      double row_sum_I = 0.0;
      double row_sum_II = 0.0;
      for (uint32_t col = 0; col < width; ++col) {
        const double val = luma[static_cast<size_t>(row) * width + col];
        row_sum_I += val;
        row_sum_II += val * val;
        const size_t idx = static_cast<size_t>(row + 1) * integral_w + (col + 1);
        sum_I[idx] = row_sum_I + sum_I[idx - integral_w];
        sum_II[idx] = row_sum_II + sum_II[idx - integral_w];
      }
    }

    // Box-filter helper: sum over rect [r0..r1, c0..c1] inclusive.
    auto box_sum = [&](const std::vector<double>& integral,
                       int r0, int c0, int r1, int c1) -> double {
      r0 = std::max(r0, 0);
      c0 = std::max(c0, 0);
      r1 = std::min(r1, static_cast<int>(height) - 1);
      c1 = std::min(c1, static_cast<int>(width) - 1);
      const size_t a = static_cast<size_t>(r1 + 1) * integral_w + (c1 + 1);
      const size_t b = static_cast<size_t>(r0) * integral_w + (c1 + 1);
      const size_t c = static_cast<size_t>(r1 + 1) * integral_w + c0;
      const size_t d = static_cast<size_t>(r0) * integral_w + c0;
      return integral[a] - integral[b] - integral[c] + integral[d];
    };

    // Compute guided-filter smoothed luma and apply residual gain.
    std::vector<double> gf_a(pixel_count);
    std::vector<double> gf_b(pixel_count);

    for (uint32_t row = 0; row < height; ++row) {
      for (uint32_t col = 0; col < width; ++col) {
        const int r0 = static_cast<int>(row) - kGfRadius;
        const int c0 = static_cast<int>(col) - kGfRadius;
        const int r1 = static_cast<int>(row) + kGfRadius;
        const int c1 = static_cast<int>(col) + kGfRadius;

        const int cr0 = std::max(r0, 0);
        const int cc0 = std::max(c0, 0);
        const int cr1 = std::min(r1, static_cast<int>(height) - 1);
        const int cc1 = std::min(c1, static_cast<int>(width) - 1);
        const double count = static_cast<double>((cr1 - cr0 + 1)) * (cc1 - cc0 + 1);

        const double mean_I = box_sum(sum_I, r0, c0, r1, c1) / count;
        const double mean_II = box_sum(sum_II, r0, c0, r1, c1) / count;
        const double var_I = mean_II - mean_I * mean_I;

        const size_t idx = static_cast<size_t>(row) * width + col;
        gf_a[idx] = var_I / (var_I + kGfEps);
        gf_b[idx] = mean_I * (1.0 - gf_a[idx]);
      }
    }

    // Build integral images for a and b to average them.
    std::vector<double> sum_a(integral_n, 0.0);
    std::vector<double> sum_b(integral_n, 0.0);
    for (uint32_t row = 0; row < height; ++row) {
      double rs_a = 0.0, rs_b = 0.0;
      for (uint32_t col = 0; col < width; ++col) {
        const size_t idx = static_cast<size_t>(row) * width + col;
        rs_a += gf_a[idx];
        rs_b += gf_b[idx];
        const size_t ii = static_cast<size_t>(row + 1) * integral_w + (col + 1);
        sum_a[ii] = rs_a + sum_a[ii - integral_w];
        sum_b[ii] = rs_b + sum_b[ii - integral_w];
      }
    }

    for (uint32_t row = 0; row < height; ++row) {
      for (uint32_t col = 0; col < width; ++col) {
        const int r0 = static_cast<int>(row) - kGfRadius;
        const int c0 = static_cast<int>(col) - kGfRadius;
        const int r1 = static_cast<int>(row) + kGfRadius;
        const int c1 = static_cast<int>(col) + kGfRadius;

        const int cr0 = std::max(r0, 0);
        const int cc0 = std::max(c0, 0);
        const int cr1 = std::min(r1, static_cast<int>(height) - 1);
        const int cc1 = std::min(c1, static_cast<int>(width) - 1);
        const double count = static_cast<double>((cr1 - cr0 + 1)) * (cc1 - cc0 + 1);

        const double mean_a = box_sum(sum_a, r0, c0, r1, c1) / count;
        const double mean_b = box_sum(sum_b, r0, c0, r1, c1) / count;

        const size_t idx = static_cast<size_t>(row) * width + col;
        const double smoothed = mean_a * luma[idx] + mean_b;
        const double detail = luma[idx] - smoothed;

        // Adaptive gain: more gain in textured areas (higher local variance),
        // less in smooth areas; and intensity modulation for shadow boost.
        const double intensity_factor = std::pow(
            std::max(luma[idx], 1.0) / 65535.0, -0.3);
        const double clamped_ifactor = std::clamp(intensity_factor, 0.5, 3.0);

        luma[idx] += gf_gain * clamped_ifactor * confidence[idx] * detail;
      }
    }
  }

  // --- Stage 4: Multiplicative ratio transfer to RGB ---
  // CFA residual has been suppressed in Stage 1 (frequency-domain notch),
  // so the per-pixel ratio is clean.
  constexpr double kMinLuma = 1.0;
  constexpr double kMinRatio = 0.3;
  constexpr double kMaxRatio = 4.0;

  for (size_t i = 0; i < pixel_count; ++i) {
    const double orig_y = std::max(original_luma[i], kMinLuma);
    const double enhanced_y = std::max(luma[i], 0.0);
    double ratio = enhanced_y / orig_y;
    ratio = std::clamp(ratio, kMinRatio, kMaxRatio);

    const size_t px = i * colors;
    for (uint32_t ch = 0; ch < colors; ++ch) {
      const double val = static_cast<double>(image->pixels[px + ch]) * ratio;
      image->pixels[px + ch] = static_cast<uint16_t>(std::clamp(val, 0.0, 65535.0));
    }
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

bool ExtractCfaGuideImage(const std::string& source_path,
                          const SourceLinearDngMetadata& metadata,
                          RasterImage* guide,
                          std::string* error_message) {
  guide->width = 0;
  guide->height = 0;
  guide->colors = 0;
  guide->bits = 0;
  guide->pixels.clear();

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

  const ushort* raw = processor.imgdata.rawdata.raw_image;
  const uint32_t width = processor.imgdata.sizes.raw_width;
  const uint32_t height = processor.imgdata.sizes.raw_height;
  if (raw == nullptr || width == 0 || height == 0) {
    *error_message = "LibRaw raw mosaic not available for CFA guide extraction";
    processor.recycle();
    return false;
  }

  guide->width = width;
  guide->height = height;
  guide->colors = 4;
  guide->bits = 16;
  guide->pixels.resize(static_cast<size_t>(width) * height * guide->colors, 0);

  const double black_level = metadata.has_black_level ? metadata.black_level : 0.0;

  auto raw_sample = [&](int row, int col, int target_color) -> double {
    row = std::clamp(row, 0, static_cast<int>(height) - 1);
    col = std::clamp(col, 0, static_cast<int>(width) - 1);
    if (processor.COLOR(row, col) != target_color) {
      return -1.0;
    }
    const size_t idx = static_cast<size_t>(row) * width + col;
    return std::max(static_cast<double>(raw[idx]) - black_level, 0.0);
  };

  for (uint32_t row = 0; row < height; ++row) {
    for (uint32_t col = 0; col < width; ++col) {
      const size_t px = (static_cast<size_t>(row) * width + col) * guide->colors;

      double green_a = 0.0;
      double green_b = 0.0;

      const int color = processor.COLOR(static_cast<int>(row), static_cast<int>(col));

      if (color == 1) {
        green_a = raw_sample(static_cast<int>(row), static_cast<int>(col), 1);
        const double d1 = raw_sample(static_cast<int>(row) - 1, static_cast<int>(col) - 1, 3);
        const double d2 = raw_sample(static_cast<int>(row) - 1, static_cast<int>(col) + 1, 3);
        const double d3 = raw_sample(static_cast<int>(row) + 1, static_cast<int>(col) - 1, 3);
        const double d4 = raw_sample(static_cast<int>(row) + 1, static_cast<int>(col) + 1, 3);
        green_b = 0.25 * (std::max(d1, 0.0) + std::max(d2, 0.0) + std::max(d3, 0.0) + std::max(d4, 0.0));
      } else if (color == 3) {
        green_b = raw_sample(static_cast<int>(row), static_cast<int>(col), 3);
        const double d1 = raw_sample(static_cast<int>(row) - 1, static_cast<int>(col) - 1, 1);
        const double d2 = raw_sample(static_cast<int>(row) - 1, static_cast<int>(col) + 1, 1);
        const double d3 = raw_sample(static_cast<int>(row) + 1, static_cast<int>(col) - 1, 1);
        const double d4 = raw_sample(static_cast<int>(row) + 1, static_cast<int>(col) + 1, 1);
        green_a = 0.25 * (std::max(d1, 0.0) + std::max(d2, 0.0) + std::max(d3, 0.0) + std::max(d4, 0.0));
      } else if (color == 0) {
        const double g1l = raw_sample(static_cast<int>(row), static_cast<int>(col) - 1, 1);
        const double g1r = raw_sample(static_cast<int>(row), static_cast<int>(col) + 1, 1);
        const double g2u = raw_sample(static_cast<int>(row) - 1, static_cast<int>(col), 3);
        const double g2d = raw_sample(static_cast<int>(row) + 1, static_cast<int>(col), 3);
        green_a = 0.5 * (std::max(g1l, 0.0) + std::max(g1r, 0.0));
        green_b = 0.5 * (std::max(g2u, 0.0) + std::max(g2d, 0.0));
      } else {
        const double g1u = raw_sample(static_cast<int>(row) - 1, static_cast<int>(col), 1);
        const double g1d = raw_sample(static_cast<int>(row) + 1, static_cast<int>(col), 1);
        const double g2l = raw_sample(static_cast<int>(row), static_cast<int>(col) - 1, 3);
        const double g2r = raw_sample(static_cast<int>(row), static_cast<int>(col) + 1, 3);
        green_a = 0.5 * (std::max(g1u, 0.0) + std::max(g1d, 0.0));
        green_b = 0.5 * (std::max(g2l, 0.0) + std::max(g2r, 0.0));
      }

      guide->pixels[px + 1] = static_cast<uint16_t>(std::clamp(green_a, 0.0, 65535.0));
      guide->pixels[px + 3] = static_cast<uint16_t>(std::clamp(green_b, 0.0, 65535.0));
    }
  }

  processor.recycle();
  return true;
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
                       std::string* error_message,
                       int expected_colors = 3) {
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

  if (processed->bits != 16 ||
      (expected_colors > 0 && processed->colors != expected_colors)) {
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

  if (ShouldApplyPredictedDetailGain(metadata, payload->raw_image)) {
    std::string cfa_guide_error;
    if (ExtractCfaGuideImage(source_path,
                             metadata,
                             &payload->cfa_guide_image,
                             &cfa_guide_error)) {
      // guide available
    } else {
      payload->cfa_guide_image = RasterImage();
    }
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

    ApplyPredictedDetailGain(metadata, &payload.cfa_guide_image, &payload.raw_image);
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