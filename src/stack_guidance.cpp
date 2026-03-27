#include "stack_guidance.h"

#include <algorithm>
#include <cmath>
#include <cstring>
#include <numeric>

namespace {

static constexpr uint32_t kGridHeight = 124;
static constexpr uint32_t kGridWidth = 128;
static constexpr uint32_t kTileRows = 4;
static constexpr uint32_t kTileCols = 2;
static constexpr uint32_t kNumTiles = kTileRows * kTileCols;  // 8
static constexpr uint32_t kTileH = StackGuidanceMaps::kTileHeight;  // 31
static constexpr uint32_t kTileW = StackGuidanceMaps::kTileWidth;   // 64
static constexpr uint32_t kTilePixels = kTileH * kTileW;            // 1984

// Index into a 2D array stored row-major.
inline size_t Idx(uint32_t row, uint32_t col, uint32_t width) {
  return static_cast<size_t>(row) * width + col;
}

// Compute finite-difference gradients along rows (axis=1) and columns (axis=2)
// for a 3D array of shape [tiles, height, width].
// grad_y[t][r][c] = central difference along rows, grad_x[t][r][c] = along cols.
void ComputeGradients(const std::vector<float>& stack,
                      uint32_t tiles, uint32_t height, uint32_t width,
                      std::vector<float>& grad_y,
                      std::vector<float>& grad_x) {
  const size_t plane_size = static_cast<size_t>(height) * width;
  grad_y.resize(tiles * plane_size);
  grad_x.resize(tiles * plane_size);

  for (uint32_t t = 0; t < tiles; ++t) {
    const size_t base = t * plane_size;

    // Gradient along rows (axis=1): central diff interior, forward/backward at edges.
    for (uint32_t r = 0; r < height; ++r) {
      for (uint32_t c = 0; c < width; ++c) {
        float gy;
        if (r == 0) {
          gy = stack[base + Idx(1, c, width)] - stack[base + Idx(0, c, width)];
        } else if (r == height - 1) {
          gy = stack[base + Idx(height - 1, c, width)] - stack[base + Idx(height - 2, c, width)];
        } else {
          gy = 0.5f * (stack[base + Idx(r + 1, c, width)] - stack[base + Idx(r - 1, c, width)]);
        }
        grad_y[base + Idx(r, c, width)] = gy;
      }
    }

    // Gradient along columns (axis=2).
    for (uint32_t r = 0; r < height; ++r) {
      for (uint32_t c = 0; c < width; ++c) {
        float gx;
        if (c == 0) {
          gx = stack[base + Idx(r, 1, width)] - stack[base + Idx(r, 0, width)];
        } else if (c == width - 1) {
          gx = stack[base + Idx(r, width - 1, width)] - stack[base + Idx(r, width - 2, width)];
        } else {
          gx = 0.5f * (stack[base + Idx(r, c + 1, width)] - stack[base + Idx(r, c - 1, width)]);
        }
        grad_x[base + Idx(r, c, width)] = gx;
      }
    }
  }
}

float Percentile(std::vector<float>& values, float p) {
  if (values.empty()) return 0.0f;
  const float index = p / 100.0f * static_cast<float>(values.size() - 1);
  const size_t lo = static_cast<size_t>(std::floor(index));
  const size_t hi = std::min(lo + 1, values.size() - 1);
  const float frac = index - static_cast<float>(lo);

  std::nth_element(values.begin(), values.begin() + static_cast<ptrdiff_t>(lo), values.end());
  const float lo_val = values[lo];
  if (lo == hi) return lo_val;

  std::nth_element(values.begin() + static_cast<ptrdiff_t>(lo + 1),
                   values.begin() + static_cast<ptrdiff_t>(hi), values.end());
  const float hi_val = values[hi];
  return lo_val + frac * (hi_val - lo_val);
}

}  // namespace

StackGuidanceMaps ComputeStackGuidance(const std::vector<uint8_t>& unknown_block_3) {
  StackGuidanceMaps result;

  if (unknown_block_3.size() != 63488) {
    result.error = "UnknownBlock3 payload size is not 63488 bytes";
    return result;
  }

  // Interpret as uint32 LE array.
  static_assert(sizeof(uint32_t) == 4);
  const size_t num_u32 = unknown_block_3.size() / 4;
  if (num_u32 != kGridHeight * kGridWidth) {
    result.error = "UnknownBlock3 does not contain 124*128 uint32 values";
    return result;
  }

  // grid[124][128] as float32.
  std::vector<float> grid(kGridHeight * kGridWidth);
  for (size_t i = 0; i < num_u32; ++i) {
    uint32_t val;
    std::memcpy(&val, unknown_block_3.data() + i * 4, 4);
    grid[i] = static_cast<float>(val);
  }

  // Split into 8 tiles (4 rows x 2 cols), each 31x64.
  // stack[8][31][64]
  std::vector<float> stack(kNumTiles * kTilePixels);
  for (uint32_t ty = 0; ty < kTileRows; ++ty) {
    for (uint32_t tx = 0; tx < kTileCols; ++tx) {
      const uint32_t tile_idx = ty * kTileCols + tx;
      for (uint32_t r = 0; r < kTileH; ++r) {
        for (uint32_t c = 0; c < kTileW; ++c) {
          const uint32_t grid_row = ty * kTileH + r;
          const uint32_t grid_col = tx * kTileW + c;
          stack[tile_idx * kTilePixels + Idx(r, c, kTileW)] =
              grid[Idx(grid_row, grid_col, kGridWidth)];
        }
      }
    }
  }

  // mean = stack.mean(axis=0)  → shape [31, 64]
  result.mean.assign(kTilePixels, 0.0f);
  for (uint32_t t = 0; t < kNumTiles; ++t) {
    const size_t base = t * kTilePixels;
    for (size_t i = 0; i < kTilePixels; ++i) {
      result.mean[i] += stack[base + i];
    }
  }
  for (size_t i = 0; i < kTilePixels; ++i) {
    result.mean[i] /= static_cast<float>(kNumTiles);
  }

  // Compute gradients.
  std::vector<float> grad_y, grad_x;
  ComputeGradients(stack, kNumTiles, kTileH, kTileW, grad_y, grad_x);

  // sharpness = sqrt(gx^2 + gy^2), shape [8, 31, 64]
  std::vector<float> sharpness(kNumTiles * kTilePixels);
  for (size_t i = 0; i < sharpness.size(); ++i) {
    sharpness[i] = std::sqrt(grad_x[i] * grad_x[i] + grad_y[i] * grad_y[i]);
  }

  // sharpness_scale = max(sharpness.std(axis=0, keepdims=True), 1e-6)
  // sharpness.std over tiles for each pixel.
  std::vector<float> sharpness_std(kTilePixels, 0.0f);
  {
    std::vector<float> sharpness_mean(kTilePixels, 0.0f);
    for (uint32_t t = 0; t < kNumTiles; ++t) {
      const size_t base = t * kTilePixels;
      for (size_t i = 0; i < kTilePixels; ++i) {
        sharpness_mean[i] += sharpness[base + i];
      }
    }
    for (size_t i = 0; i < kTilePixels; ++i) {
      sharpness_mean[i] /= static_cast<float>(kNumTiles);
    }
    for (uint32_t t = 0; t < kNumTiles; ++t) {
      const size_t base = t * kTilePixels;
      for (size_t i = 0; i < kTilePixels; ++i) {
        const float diff = sharpness[base + i] - sharpness_mean[i];
        sharpness_std[i] += diff * diff;
      }
    }
    for (size_t i = 0; i < kTilePixels; ++i) {
      sharpness_std[i] = std::sqrt(sharpness_std[i] / static_cast<float>(kNumTiles));
    }
  }
  std::vector<float> sharpness_scale(kTilePixels);
  for (size_t i = 0; i < kTilePixels; ++i) {
    sharpness_scale[i] = std::max(sharpness_std[i], 1e-6f);
  }

  // sharpness_offset = sharpness - sharpness.max(axis=0, keepdims=True)
  // sharpness_weights = exp(clamp(4.0 * offset / scale, -12, 0))
  std::vector<float> sharpness_max(kTilePixels, -1e30f);
  for (uint32_t t = 0; t < kNumTiles; ++t) {
    const size_t base = t * kTilePixels;
    for (size_t i = 0; i < kTilePixels; ++i) {
      sharpness_max[i] = std::max(sharpness_max[i], sharpness[base + i]);
    }
  }

  std::vector<float> sharpness_weights(kNumTiles * kTilePixels);
  for (uint32_t t = 0; t < kNumTiles; ++t) {
    const size_t base = t * kTilePixels;
    for (size_t i = 0; i < kTilePixels; ++i) {
      const float offset = sharpness[base + i] - sharpness_max[i];
      const float arg = std::clamp(4.0f * offset / sharpness_scale[i], -12.0f, 0.0f);
      sharpness_weights[base + i] = std::exp(arg);
    }
  }

  // guide = (weights * stack).sum(axis=0) / max(weights.sum(axis=0), 1e-6)
  result.guide.assign(kTilePixels, 0.0f);
  std::vector<float> weight_sum(kTilePixels, 0.0f);
  for (uint32_t t = 0; t < kNumTiles; ++t) {
    const size_t base = t * kTilePixels;
    for (size_t i = 0; i < kTilePixels; ++i) {
      result.guide[i] += sharpness_weights[base + i] * stack[base + i];
      weight_sum[i] += sharpness_weights[base + i];
    }
  }
  for (size_t i = 0; i < kTilePixels; ++i) {
    result.guide[i] /= std::max(weight_sum[i], 1e-6f);
  }

  // std = stack.std(axis=0)
  std::vector<float> stack_std(kTilePixels, 0.0f);
  for (uint32_t t = 0; t < kNumTiles; ++t) {
    const size_t base = t * kTilePixels;
    for (size_t i = 0; i < kTilePixels; ++i) {
      const float diff = stack[base + i] - result.mean[i];
      stack_std[i] += diff * diff;
    }
  }
  for (size_t i = 0; i < kTilePixels; ++i) {
    stack_std[i] = std::sqrt(stack_std[i] / static_cast<float>(kNumTiles));
  }

  // Structure tensor components (mean over tiles).
  result.tensor_x.assign(kTilePixels, 0.0f);
  result.tensor_y.assign(kTilePixels, 0.0f);
  std::vector<float> tensor_xy(kTilePixels, 0.0f);
  for (uint32_t t = 0; t < kNumTiles; ++t) {
    const size_t base = t * kTilePixels;
    for (size_t i = 0; i < kTilePixels; ++i) {
      result.tensor_x[i] += grad_x[base + i] * grad_x[base + i];
      result.tensor_y[i] += grad_y[base + i] * grad_y[base + i];
      tensor_xy[i] += grad_x[base + i] * grad_y[base + i];
    }
  }
  for (size_t i = 0; i < kTilePixels; ++i) {
    result.tensor_x[i] /= static_cast<float>(kNumTiles);
    result.tensor_y[i] /= static_cast<float>(kNumTiles);
    tensor_xy[i] /= static_cast<float>(kNumTiles);
  }

  // tensor_trace, coherence, x_ratio, y_ratio
  result.tensor_coherence.assign(kTilePixels, 0.0f);
  for (size_t i = 0; i < kTilePixels; ++i) {
    const float trace = result.tensor_x[i] + result.tensor_y[i];
    const float safe_trace = std::max(trace, 1e-6f);
    const float diff = result.tensor_x[i] - result.tensor_y[i];
    result.tensor_coherence[i] = std::sqrt(diff * diff + 4.0f * tensor_xy[i] * tensor_xy[i]) / safe_trace;
    result.tensor_x[i] /= safe_trace;
    result.tensor_y[i] /= safe_trace;
  }

  // stability and alias maps
  // relative_std = std / max(mean, 1.0)
  std::vector<float> relative_std(kTilePixels);
  for (size_t i = 0; i < kTilePixels; ++i) {
    relative_std[i] = stack_std[i] / std::max(result.mean[i], 1.0f);
  }

  // scale = percentile(relative_std, 90)
  std::vector<float> rs_copy(relative_std);
  const float scale = Percentile(rs_copy, 90.0f);

  result.stability.assign(kTilePixels, 0.0f);
  result.alias.assign(kTilePixels, 0.0f);

  if (scale <= 1e-6f) {
    std::fill(result.stability.begin(), result.stability.end(), 1.0f);
    // alias stays zero
  } else {
    float alias_max = 0.0f;
    for (size_t i = 0; i < kTilePixels; ++i) {
      const float r = relative_std[i] / scale;
      result.stability[i] = std::exp(-r * r);
      const float a = r * std::exp(-0.5f * r * r);
      result.alias[i] = a;
      alias_max = std::max(alias_max, a);
    }
    if (alias_max > 1e-6f) {
      for (size_t i = 0; i < kTilePixels; ++i) {
        result.alias[i] /= alias_max;
      }
    } else {
      std::fill(result.alias.begin(), result.alias.end(), 0.0f);
    }
  }

  result.ok = true;
  return result;
}
