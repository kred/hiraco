#pragma once

#include <cstdint>
#include <string>
#include <vector>

struct StackGuidanceMaps {
  static constexpr uint32_t kTileWidth = 64;
  static constexpr uint32_t kTileHeight = 31;

  bool ok = false;
  std::string error;

  std::vector<float> stability;        // kTileHeight * kTileWidth
  std::vector<float> mean;             // kTileHeight * kTileWidth
  std::vector<float> guide;            // kTileHeight * kTileWidth
  std::vector<float> tensor_x;         // kTileHeight * kTileWidth
  std::vector<float> tensor_y;         // kTileHeight * kTileWidth
  std::vector<float> tensor_coherence; // kTileHeight * kTileWidth
  std::vector<float> alias;            // kTileHeight * kTileWidth
};

// Compute stack guidance maps from UnknownBlock3 payload (63488 bytes).
// This is the C++ equivalent of Python extract_om3_stack_guidance().
StackGuidanceMaps ComputeStackGuidance(const std::vector<uint8_t>& unknown_block_3);
