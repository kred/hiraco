# High-Resolution RAW Compilation & Algorithm Pipeline

This document details the architectural processes, design logic, and underlying algorithmic implementations used in `hiraco` to convert high-resolution sensor-shift composite `.ORF` files into pristine, well-formed `.dng` files.

---

## 1. Concise Summary: The Pipeline

The overarching goal of `hiraco` is to maximize detail extraction directly from High-Res `.ORF` files, aiming for optimal optical fidelity. We achieve this by abandoning standard interpolative pathways from routine raw decoders in favor of heavy spatial math targeting the specialized native geometry.

1. **Raw Payload Ingestion & Payload Bounding**: Identify the `.ORF` payload as an OM-3 High Resolution structure. We extract proprietary Internal Blocks from the MakerNote (specifically Block 1 for physical geometry bounding and Block 3 for spatial multi-shot guidance matrices) to perfectly align and guide the high-resolution extraction extraction.
2. **Pedestal Management (Black Level)**: We mathematically normalize the camera's baseline absolute black (e.g. `1020` pedestal) to `0`. This zero-bound state is mandatory for spatial transformation algorithms which would otherwise calculate harsh artifacts around the mathematical offsets.
3. **Deconvolution (Optical Base Recovery)**: Because 8+ physical sub-frames inevitably undergo sensor-shift overlap, lens diffraction, and micro-vibrations, the intrinsic image is mathematically blurred. We utilize `FFTW` (Fast Fourier Transform) to apply a Wiener Deconvolution filter that acts as the inverse of the assumed Point Spread Function (PSF), reassigning scattered photon limits to their origin points.
4. **Multi-Scale À Trous Wavelet Enhancement**: High-frequency textures and micro-contrast are lifted using non-decimated "à trous" wavelet passes. This algorithm dynamically boosts micro-edges at specific structural bounds without destructively amplifying raw noise blocks.
5. **Restoration & Packaging**: The pedestal is re-added, returning the array to physically accurate sensor geometry. The buffer is then packaged directly into an Adobe Linear RGB Matrix (with perfectly mirrored color-channel coefficients via `CameraNeutral` definitions to avoid extreme color-casts), making it highly robust for immediate processing in software like Lightroom and DxO.

---

## 2. Full Detailed Explanation

### 2.1 The High-Resolution State

Standard Bayer arrays rely on a `2x2` CFA (Color Filter Array) grid, demanding mathematically complex demosaicing (calculating unknown colors from neighbors) which fundamentally degrades maximum spatial constraints. 

Conversely, Modern Vendor high-resolution `.ORF` files are the result of rapid sequential burst triggers combined with in-body image stabilization (IBIS) micro-shifts. When merged, every physical pixel well on the grid effectively receives full Red, Green, and Blue samples. Demosaicing is bypassed. The resulting native block acts as a true, demosaic-free multi-channel bitmap. 

The primary challenge isn't color interpolation; it is the **resolution of sub-pixel overlaps**, preventing the matrix from looking artificially bloated or soft. 

### 2.2 Proprietary MakerNote Decoding: Internal Blocks 1 & 3

The "secret sauce" behind correctly forming and deconvolving the composite array without introducing artifacts relies heavily on extracting proprietary metadata payload fragments embedded within the OEM MakerNotes. These data structures instruct the algorithm on handling boundary limits and movement disparities natively computed by the camera:

- **Internal Block 1 (`unknown_block_1`)**: Generalized raw decoding libraries (like `libraw`) often struggle with bounding boxes for non-standard multi-shot modes. `hiraco` directly parses the internal bytes of Block 1 (specifically targeting `uint32` data at byte offset 23). This provides the exact boundary configurations (`working_rect_width` and `working_rect_height`) intended for the active High-Res crop. Without it, the final rendered Linear DNG exhibits mismatched optical centers, distorted edges, and corrupt black level margins.

- **Internal Block 3 (`unknown_block_3` / Stack Guidance)**: High-resolution shots computed in-camera ("Hand-held high resolution") embed a dense 63,488-byte spatial metadata payload. `hiraco` unpacks this binary blob into complex floating-point masking matrices known as **Stack Guidance Maps**:
  - `stack_stability`: Motion detection arrays indicating structural shifts between rapid sequential exposures (e.g., moving foliage, running water).
  - `stack_guide`: Foundational structural edge gradients natively captured by hardware.
  - `stack_tensor_{x,y}` & `stack_tensor_coherence`: Multi-directional high-frequency spatial detail isolates.
  - `stack_alias`: Tracks localized moiré and aliasing error generation probabilities.

By upsampling these 63KB arrays to match the colossal physical geometry, operations like the Deconvolution matrix are dynamically heavily throttled and instructed by the guidance masks on a per-pixel basis. For instance, aggressive sharpening operations bypass the regions flagged by `stack_stability` as "in motion", preventing classical and devastating multi-camera "ghosting" or smearing.

### 2.3 Mathematical Engine & Spatial Recoveries (`ApplyPredictedDetailGain`)

When the extracted OM-3 `camera_space` array is intercepted from `LibRaw`, it has standard baseline offsets. If left unaltered, editing software will treat it as a generic flat bitmap, rendering the sub-pixel details muddy.

#### Step 1: Normalization
Before advanced mathematical filters can process structural properties, the sensor’s baseline "Dark Current" must be offset.
```cpp
const double pedestal = metadata.black_level;
// Normalize values toward zero boundary for FFTW operations.
pixel_value -= pedestal;
```

#### Step 2: FFTW Wiener Deconvolution
Sensor-shift merging inherently introduces a localized low-pass convolution (a mild blur known as the Point Spread Function or PSF). We counteract this aggressively. Using `FFTW3`, the image domain is transposed into frequency maps.
- A **Wiener filter** algorithm is processed over these frequencies.
- Modulated against a calculated Signal-to-Noise Ratio (SNR), the algorithm reverses the overlapping blur, retrieving distinct physical limits inside the sensor's sub-pixels.
- The frequencies are linearly restored to image matrices (Inverse FFT).

#### Step 3: À Trous Wavelet Multi-Scale Lifting
To restore micro-contrast and local fidelity:
1. `hiraco` passes the matrix through a Multi-Scale À Trous wavelet algorithm using B3-spline convolutions.
2. The image is split into multiple spatial scales (from high-frequency fine details down to larger structural foundations).
3. The smallest scales (those representing raw optical micro-textures) are amplified using highly tuned predictive coefficients.
4. Because the à trous transform is non-decimated (maintains exact dimensional correlation), recombinations perfectly re-align to the core geometry, fundamentally avoiding classical halo or edge-clipping artifacts commonly associated with "unsharp masking".

#### Step 4: Normalization Reversal
With the matrix completely redefined logically, the original Black Level threshold is re-added.
```cpp
// Restore strict bounds and return to traditional sensor physics
pixel_value = clamp(pixel_value + pedestal);
```

### 2.4 The DNG Matrix Translation

Once the mathematical pass restores the exact limits of the optical resolution, `hiraco` transfers the matrix to the Adobe DNG SDK formatting runtime.

The output `dng_negative` is tagged strictly as a Rendered Linear RGB array. 
- **Color Identity Handling**: Proprietary OEM MakerNote tagging (which routinely corrupts third-party interpretations) is discarded. Specifically, `hiraco` writes perfectly calibrated `SetCameraNeutral` offsets to perfectly map toward the exact green/blue optical scaling the pipeline generated. This prevents the DNG from displaying the notorious "extreme Green Tint" in non-Adobe applications. 
- **Delivery**: Using DNG specifications (either `1.6.0.0` or `1.7.1.0`), the fully developed 16-bit blocks are delivered using exact bounds, resulting in a single raw structural file retaining the optical fidelity intended by the sensor hardware.
