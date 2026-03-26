# High-Resolution RAW Compilation & Conversion Pipeline

Status note as of 2026-03-25:

- The OM-3 hand-held high-res `.ORF` used in `reference3` should currently be treated as a Bayer/CFA mosaic (`8172x6132`) with crop margins, not as a demosaic-free full-color bitmap.
- The companion `.ORI` files seen in `reference2` should be treated as fallback single-frame captures, not as the target geometry for Olympus high-res reconstruction. Their crop geometry matches the standard 20 MP frame (`5184x3888`), while the high-res `.ORF` and the OM Workspace TIFF align on the high-res crop (`8160x6120`).
- The OM-3 high-res path in `hiraco` now defaults to a CFA-domain camera-space reconstruction before DNG packaging. The older `LibRaw decode/render -> optional detail processing -> linear DNG packaging` flow is still available as a fallback path.
- The first generation of OM-3 raw-domain reconstruction had a raster/metadata domain mismatch. That contract issue is now corrected: the custom OM-3 path stays in camera space and writes a pedestal-bearing stage-3 image that matches the attached camera profile metadata.
- `UnknownBlock3` in OM-3 hand-held high-res files is not opaque random metadata. Interpreted as a `124x128` little-endian `u32` grid, it contains eight aligned low-resolution scene tiles (`4x2` layout of `31x64` tiles), which appear to summarize the source stack.
- Across the checked OM-3 samples, the hidden maker-note blocks split into distinct roles:
  - `UnknownBlock2` is constant across standard, tripod high-res, hand-held high-res, and `.ORI` fallback files, so it is likely static calibration or process metadata rather than capture-specific guidance.
  - `UnknownBlock3` and `UnknownBlock4` are duplicated between a high-res `.ORF` and its paired fallback `.ORI`, so they appear to be capture-wide stack metadata rather than file-format-specific output state.
  - `UnknownBlock1` changes between the high-res `.ORF` and the paired `.ORI`, even when `UnknownBlock3/4` stay identical, so it is a strong candidate for per-file geometry, mode, or processing-state metadata.
- `hiraco` now extracts three guidance signals from those eight hidden tiles on the Python side and passes them into the native CFA-domain renderer:
  - a coarse stability/confidence map,
  - a sharpness-weighted low-resolution guide image,
  - an alias/detail-potential map derived from inter-tile variation,
  - and low-resolution structure-tensor orientation maps.
- The current OM-3 reconstruction experiments use those maps as guide-aware priors for CFA interpolation and detail lifting. This is still well short of OM Workspace quality, but it is now using single-ORF Olympus-specific stack information instead of relying only on LibRaw's rendered Bayer composite.
- `UnknownBlock1` is now looking like a structured per-file geometry/state header rather than opaque metadata. The strongest hypotheses so far are:
  - `u32[0]` / byte `0x00`: ASCII magic `SDMO` (`0x4f4d4453` little-endian).
  - `u32[17]` / byte `0x44`: capture-state word. The low halfword is `0x16`, `0x17`, or `0x1c` across the sample set, so it likely encodes burst/session index or mode subtype rather than geometry.
  - `u32[18]` / byte `0x48`: mostly constant `0x1e3c` (`7740`), with the tripod sample setting the high byte to `0x01`; likely a mode/state flag plus a fixed geometry constant.
  - `u32[19]` / byte `0x4c`: `0x01000000` only on hand-held high-res ORFs, zero on tripod ORFs and ORIs; very likely a high-res mode bit.
  - `u32[22]` / byte `0x58`: clear mode code / bitfield. Observed values are `0x2000` (hand-held high-res), `0x4000` (tripod high-res), and `0x0008` (ORI fallback).
  - `u32[23]` and `u32[47]` / bytes `0x5c` and `0xbc`: packed geometry tuples. The high halfword matches the raw image height exactly (`6132`, `7792`, `3912`), while the low halfword looks like an internal working stride / padded width (`8280`, `10440`, `5360`).
  - `u32[41]` / byte `0xa4`: exact half-resolution geometry, e.g. `4086x3066`, `5192x3896`, `2604x1956`. This strongly suggests a pyramid or intermediate working-level size.
  - `u32[43..45]` / bytes `0xac..0xb4`: repeated full-size geometry tuple. Interpretable as `1, width, 1, height, width` for the working raster.
  - `u32[55]` / byte `0xdc`: crop dimensions in 16-pixel blocks. It matches `crop_width / 16` and `crop_height / 16` on the reference files (`510x382`, `648x486`, `324x244`).
  - `u32[25]` / byte `0x64`: deliberate sentinel `0x76543210`.
  - `u32[26..33]` / bytes `0x68..0x87`: repeated `0x03FC03FC` pedestal table, matching the ORF black level `1020`.
  - The remaining words in the `u32[40..63]` region appear to be secondary geometry / grid descriptors. They vary systematically with mode, but I have not fully decoded them yet.
- Any section below that assumes Olympus high-res ORFs "nullify the need for Bayer demosaicing" should be read as historical investigation context, not current ground truth.

This document details the architectural processes, design logic, and underlying algorithmic implementations used in `hiraco` to convert Olympus/OM-System High-Resolution sensor-shift composite `.ORF` files into well-formed, widely readable `.dng` files using LibRaw and the Adobe DNG SDK.

## 1. High-Resolution Image Reconstruction
Olympus and OM SYSTEM cameras generate "High Res" images by utilizing the in-body image stabilization (IBIS) mechanism to shift the sensor by sub-pixel increments. Over a series of 8 (or more) rapid exposures, the camera captures full color information (Red, Green, Blue) at every pixel location, effectively nullifying the need for Bayer demosaicing and vastly increasing spatial and color resolution.

*   **In-Camera Processing:** The camera synthesizes these 8 sub-frames into a single large, composite raw file (`.ORF`).
*   **Pipeline Input:** Unlike standard Bayer RAWs which consist of interspersed R, G, B pixels in a 2x2 grid, the High-Res `.ORF` acts as a pristine, high-depth bitmap, possessing highly distinct radiometric traits.

## 2. Passing & Processing Bitmaps into the Adobe DNG SDK
To make this unique composite format readable by software like Adobe Lightroom, Luminar Neo, and DxO, the image must be repackaged into the universal Digital Negative (DNG) standard.

### Step 1: Pixel Extraction via LibRaw
```cpp
// LibRaw unpacks the proprietary `.ORF` structure
processor.open_file(source_path);
processor.unpack();
processor.dcraw_process(); 
```
LibRaw interacts with the `.ORF` file and extracts a large 16-bit linear buffer. However, the exact pipeline state returned by LibRaw dictates our downstream needs.

### Step 2: Linear Raster Transformation (Radiometric Scaling)
Because standard raw editors expect to process DNG files as un-manipulated sensor dumps, we must emulate the "raw" physics mathematically in C++ using `ApplyLinearDngRasterTransform`.

1.  **Pedestal Application (Black Level):** We re-apply the intrinsic optical sensor black level (e.g., `pedestal` offsets) so that absolute black acts as a baseline rather than 0.
2.  **Un-White-Balancing:** LibRaw often extracts colors aligned towards an equilibrium. True raw senor data natively biases toward Green (due to quantum efficiency and CFA glass dynamics). We artificially "un-white-balance" the linear data by multiplying the channels by the camera's `as_shot_neutral` coefficients.

### Step 3: DNG Image Matrix Construction
We construct an empty `dng_simple_image` bounded by the exact dimensions of the extracted high-res payload. We then iterate through the image buffer (row by row, column by column) and commit the 16-bit integer values:
```cpp
dng_pixel_buffer buffer;
image->GetPixelBuffer(row, col, buffer);
// Map LibRaw array indices -> Adobe DNG buffer offsets
```

### Step 4: Metadata Binding & DNG Negative Compilation
Instead of raw arrays resting in a void, a `dng_negative` object is established to frame the image context:
*   We attach the `D50_xy_coord()` to establish the core illumination source.
*   We build a `LinearSrgbProfile` because the High-Res composite is already reconstructed spatially and natively projects linear color, making complex custom matrix profiles less necessary.
*   We pass the negative through `host.Make_dng_image_writer()` which writes the final TIFF/DNG structure, optionally compressing the blocks with `Deflate` or `JPEG-XL`.

---

## 3. Spatial Detail Processing: Deconvolution & High-Pass Filtering

Due to the physical nature of sensor-shift technology, accumulating 8+ sub-frames often introduces a microscopic low-pass convolution effect. This occurs because sub-pixel overlaps, lens diffraction limits, and mechanical micro-vibrations inherently soften the native edge transitions. To combat this and rival OEM outputs (like OM Workspace), the pipeline utilizes advanced spatial filtering algorithms to extract and emphasize the optical detail.

### Deconvolution (Optical Blur Reversal)
To undo the structural blurring introduced by the lens geometry and the physical sensor stepping mechanism, a spatial deconvolution algorithm calculates the inverse of the assumed Point Spread Function (PSF).
*   Instead of simply expanding contrast bounds like traditional sharpening, deconvolution mathematically reassigns scattered light energy back to its theoretical origin point.
*   This restores the pristine optical resolution that the High-Res mode intended to capture before physical forces interfered.

### High-Pass Filtering & Detail Extraction
Following base reconstruction, the image buffer can utilize High-Pass Filtering to strictly isolate the high-frequency structures (edges, micro-textures, and fine visual noise limits).
1.  **Frequency Separation:** A blurred (low-pass) proxy of the image is computed and subtracted from the original buffer, leaving behind merely the optical edges.
2.  **Detail Gain Multiplication:** This isolated high-pass map is multiplied by a highly-tuned coefficient (e.g., the `predicted_detail_gain`, empirically scaling around `~1.8x` for Olympus outputs based on our predictive reference models).
3.  **Recombination:** The amplified high-frequency map is linearly blended back into the baseline image matrix. This fundamentally prevents the "muddy" or "soft" aesthetic that raw sensor-shift dumps suffer from when read naively, restoring perceptual acutance and micro-contrast.

---

## 4. Root Cause Analysis: The "Green Cast" Defect
During early testing, DNG files converted by `hiraco` interpreted successfully in Lightroom but displayed an overwhelming **Green Cast** in other commercial editors (Luminar Neo, ON1).

### The Scientific Root Cause
Raw camera sensors naturally capture significantly more green light than red or blue. In our `ApplyLinearDngRasterTransform`, we successfully simulated this raw physical state by intentionally "un-white-balancing" the image via channel multipliers (raising the green data bounds).

However, the DNG SDK was improperly configured on the metadata front:
1.  Our C++ pipeline defaulted the `SetCameraNeutral` tag to strictly `[1.0, 1.0, 1.0]` (The Identity Matrix), which implied "This image requires no white balance offsets."
2.  Our secondary Python script (`metadata_copy.py`) later forcefully invoked `exiftool` to overwrite the `ProfileName` with a dummy string `"hiraco-linear-srgb"`, which sabotaged the DNG SDK's internal color matrix mappings.

**The Result:** Software like Luminar loaded an artificially skewed raw matrix (Green mathematically ~60% larger than Red/Blue) but received `AsShotNeutral = 1,1,1`. The software interpreted the files *literally* without applying a White Balance correction pass, yielding a severely green image. 

---

## 5. Implementation Details of the Fix
To correct the algorithm, the visual metadata had to perfectly map the underlying radiometric transformation logic. 

**Fix 1: Propagating Hardware Multipliers (C++)**
In `dng_writer_bridge.cpp`, we actively loaded the dynamically extracted hardware multipliers (e.g., `0.426, 1.0, 0.595`) array into the specific `dng_vector`:
```cpp
dng_vector camera_neutral(3);
camera_neutral[0] = metadata.as_shot_neutral[0]; // Red
camera_neutral[1] = metadata.as_shot_neutral[1]; // Green
camera_neutral[2] = metadata.as_shot_neutral[2]; // Blue
negative->SetCameraNeutral(camera_neutral);
```
This forces the resultant DNG to output exactly the `[IFD0] AsShotNeutral` EXIF flag that matches the pixel logic.

**Fix 2: Eliminating EXIF Sabotage**
Removed the invasive `IFD0:ProfileName=hiraco-linear-srgb` command from ExifTool in `src/hiraco/metadata_copy.py`. This ensures that the highly tailored `CameraCalibration` matrices naturally authored by the DNG SDK remain intact and are recognized by third-party raw decoders.

**Validation**
Utilizing Python's `rawpy` and pure NumPy operations (`rgb.mean(axis=0)` on the raw arrays), we mathematically validated that third-party processors now successfully parse the `CameraNeutral` matrices and render back an image matching equilibrium (e.g., Red: ~18000, Green: ~18000, Blue: ~18000).
