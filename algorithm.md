# hiraco Processing Pipeline

This document describes the current implementation in the repository. It is not a long-term formal specification; it is a practical description of what the code does today.

## 1. Scope

`hiraco` is a shared C++ processing core with two frontends:

- `hiraco-cli` for direct conversion
- `hiraco-gui` for preview, crop inspection, parameter tuning, and batch conversion

Current inputs are selected Olympus / OM SYSTEM `.ORF` and `.ORI` files. The output is a rendered Linear DNG written through the Adobe DNG SDK.

The project currently contains:

- a custom high-resolution raw-domain reconstruction path
- a LibRaw-based render path for sources that do not go through the custom reconstruction
- shared preview, crop-preview, enhancement, and DNG writing logic used by both frontends

## 2. High-Level Architecture

The main core entry points are:

1. `PrepareSource`
2. `RenderOriginalPreview`
3. `RenderConvertedCrop`
4. `ConvertToDng`

They all operate on `PreparedSource`, which holds:

- the source path and source name
- base metadata extracted from LibRaw
- image dimensions
- shared cached state for original preview, enhancement metadata, preview brightness, and crop-processing cache
- an optional highlight-recovery companion path detected beside a supported
  high-resolution ORF

This is important for the GUI: opening a file, moving the crop box, and running a conversion all reuse the same core data rather than rebuilding everything from scratch.

## 3. Preparation And Metadata

### 3.1 Base Metadata

`PrepareSource` does the lightweight, reusable setup work:

- opens the source with LibRaw
- decodes a compact summary of sensor dimensions and camera information
- builds base metadata such as:
  - make and model
  - raw and image dimensions
  - black and white level
  - color matrix and RGB camera matrix when available
  - as-shot neutral
  - default crop metadata

This step intentionally does not do the heavier vendor-specific enhancement work anymore.

### 3.2 Lazy Enhancement Metadata

Enhancement metadata is built only when a converted crop or full conversion needs it.

`BuildEnhancementMetadata` currently performs:

- vendor MakerNote parsing
- working-geometry extraction when present
- stack-guidance extraction for supported high-resolution captures

The enhancement metadata is cached inside `PreparedSourceData` after the first build.

### 3.3 Stack Guidance Maps

For supported high-resolution captures, MakerNote data is unpacked into low-resolution guidance maps, including:

- `stack_stability`
- `stack_guide`
- `stack_tensor_x`
- `stack_tensor_y`
- `stack_tensor_coherence`
- `stack_alias`

These maps are later sampled inside the raw-domain reconstruction path to modulate sharpening, motion sensitivity, directionality, and alias handling.

## 4. Preview And Crop Workflow

### 4.1 Original Preview

`RenderOriginalPreview` builds the GUI's large preview image and caches it.

Current behavior:

- preview generation goes through LibRaw
- the result is cached in `PreparedSourceData`
- the GUI can restore a cached original preview immediately when the user revisits an already loaded file

### 4.2 Converted Crop Preview

`RenderConvertedCrop` drives the live crop-preview panel in the GUI.

Current behavior:

- enhancement metadata is ensured lazily
- preview auto-bright gain is estimated once and cached
- a `ProcessingCache` is reused when the current crop stays inside the already prepared region
- if necessary, the cache is rebuilt for an expanded region around the requested crop
- the crop preview then runs through the same enhancement logic used by the full conversion path
- when `HL Rec` is enabled, the recovery pass is also applied to the crop after
  enhancement.  Its companion render is not currently cached, so this option
  can make a cold crop preview substantially slower.

This is why crop inspection is much faster after the first cache build than it is from a cold start.

## 5. Full Conversion Flow

`ConvertToDng` performs the full conversion path:

1. Ensure enhancement metadata exists.
2. Reuse the cached original preview if one is already available.
3. Build the rendered Linear DNG payload.
4. Apply enhancement when the metadata requests predicted detail gain.
5. Optionally apply ORI-assisted highlight recovery.
6. Restore the camera-space black-level pedestal and write the final DNG with
   the Adobe DNG SDK.

Compression modes currently exposed by the CLI and GUI are:

- `uncompressed`
- `deflate`
- `jpeg-xl`

## 6. High-Resolution Raw-Domain Reconstruction

The most specialized part of the project is the high-resolution path in `RenderOm3RawDomainImage`.

At a high level it performs:

1. Load the raw mosaic and CFA layout directly from LibRaw.
2. Normalize raw values into a 16-bit working domain using black and white level information.
3. Load low-resolution guidance maps from the enriched metadata.
4. Sample those maps through precomputed bilinear samplers rather than materializing large upsampled full-frame buffers.
5. Reconstruct a base green plane.
6. Refine that green plane with guide-aware directional logic.
7. Apply stability-driven green detail lift.
8. Constrain every positive green reconstruction adjustment by its local linear
   headroom. An input below sensor white remains below sensor white; genuinely
   clipped source samples remain identifiable as such rather than being expanded
   into a larger clipped area.
9. Reconstruct final RGB output from the green plane plus red/blue difference interpolation.
10. Preserve the linear camera-space signal through the reconstruction and
   enhancement stages.  Do not apply a pixel-domain highlight shoulder.  The
   DNG's Camera Neutral metadata carries white-balance information; it is not a
   raw per-channel highlight limit.
11. When `HL Rec` is enabled and an eligible companion has been detected, render
   the companion in the same camera-space domain.  The recovery pass:
   - searches a global integer offset from -2 to +2 companion pixels using
     green-channel mean absolute error;
   - builds a low-frequency RGB base by sampling at a 16-pixel stride and
     applying two separable 5-tap blur passes;
   - considers pixels whose brightest high-resolution channel is above 55% of
     its signal range, with full strength at 85%; and
   - only reduces a candidate when the companion's blurred luma is lower.  It
     multiplies each high-resolution channel by a capped companion/high-resolution
     base ratio, thereby retaining high-resolution local detail.

   Recovery runs after the main detail pipeline but before the camera black
   pedestal is restored.  It is strictly subtractive: it can retain a lower,
   unclipped companion base but cannot invent signal, brighten a sample, or turn
   an originally safe sample into a new clipped one.

### 6.1 Highlight-Recovery Companion Detection

This feature is available only when the input is recognized as a supported
high-resolution `.ORF`.  During preparation, `hiraco` looks in the same directory
for the first existing filename in this order:

1. `<stem>.ORI`
2. `<stem>.ori`
3. `<stem>_.ORI`
4. `<stem>_.ori`
5. `<stem>_<source extension>` (for example, `<stem>_.ORF`)
6. `<stem>_.<opposite-case extension>` (the current fallback contains two dots;
   for example, `<stem>_..orf`)

The GUI exposes this as a per-file `HL Rec` switch, which is **off by default**.
The label is `n/a` if no candidate file was found.  The CLI does not currently
expose a switch for this feature.

Detection is intentionally only a same-directory filename lookup today.  It does
not yet validate capture time, exposure, ISO, camera identity, dimensions, or
other metadata.  A detected file is therefore a *candidate*, not proof that it is
the matching lower-resolution exposure; users should enable the option only when
the companion is known to belong to the same capture.

This path is where most high-res specific image recovery happens before the later enhancement stages.

## 7. Enhancement Pipeline

The enhancement pass is implemented in `ApplyPredictedDetailGain` and is only enabled when the source metadata requests predicted detail gain.

Internally the processing is staged, but the GUI now exposes user-facing section names.

### 7.1 Detail Recovery

User-facing controls:

- `Blur Radius`
- `Noise Protection`

Implementation summary:

- build a luma channel from the RGB render
- build a confidence map from the CFA guide image when available
- apply FFTW-based Wiener deconvolution
- model the blur using both the optical PSF and the physical sensor-shift integration limit

This is the part of the pipeline that tries to recover lost fine detail while limiting unstable amplification.

### 7.2 Multi-scale Detail

User-facing controls:

- `Denoise`
- `Small Detail`
- `Medium Detail`
- `Large Detail`

Implementation summary:

- run a non-decimated à trous wavelet decomposition
- use Halide AOT code for the main wavelet filtering work
- drive the finest wavelet band from the `Small Detail` control with extra sensitivity so tiny texture remains adjustable without a dedicated fourth slider
- estimate per-scale noise and apply capped adaptive thresholding so fine bands stay responsive
- blend adjacent scale gains in both directions before reconstruction so the controls overlap more like practical detail shaping instead of isolated bins
- re-add weighted detail according to the configured scale gains

This is the main texture and micro-contrast shaping stage.

### 7.3 Edge Refinement

User-facing controls:

- `Edge Radius`
- `Edge Gain`

Implementation summary:

- run a guided-filter based refinement pass
- use Halide AOT code for the guided filter
- sharpen in an edge-aware way to reduce halos and keep stronger boundaries under control

### 7.4 Ratio Transfer

After the luma pipeline is complete, the enhanced luma is transferred back onto RGB through a multiplicative ratio step. Positive ratio gain is limited by the brightest input channel's remaining linear headroom. In normalized form, a channel at `x` can rise no higher than `2x - x²`; this is strictly below white for `0 < x < 1`, so detail enhancement cannot turn an originally recoverable highlight into a clipped DNG sample.

This part is not surfaced as a separate GUI section, but it is still part of the internal enhancement pipeline.

## 8. DNG Packaging

After reconstruction and enhancement, `hiraco` writes a rendered Linear DNG through the Adobe DNG SDK.

Current responsibilities in this stage include:

- building the DNG image payload
- configuring compression
- carrying corrected metadata forward
- optionally reusing the cached original preview when one is available

The output is intended to behave as a robust rendered linear image in downstream software rather than as a preserved raw CFA mosaic.

## 9. Performance Design In The Current Codebase

The current implementation includes several performance-focused design choices:

- OpenMP over large per-pixel loops
- FFTW threading and wisdom reuse
- Halide AOT kernels for wavelet and guided-filter work
- lazy enhancement metadata generation
- cached original preview reuse
- cached crop-processing reuse for crop preview
- optional timing logs through `HIRACO_TIMING=1` or CLI `--debug`

ORI-assisted highlight recovery is deliberately applied outside the high-resolution
processing cache so that the per-file switch takes effect immediately.  The tradeoff
is that the companion is prepared and rendered again for each recovery-enabled crop
preview or conversion; it is not yet cached.

Recent raw-domain optimizations also removed several large full-frame temporary map upsample buffers and replaced them with on-demand bilinear sampling from the original low-resolution guidance maps.

## 10. Current Limits And Assumptions

- The strongest custom path is the high-resolution workflow.
- ORI-assisted highlight recovery is an opt-in, subtractive correction, not a
  replacement for a matched HDR merge.  Its companion is found by filename only
  and is not metadata-validated.
- Recovery registration is a global integer offset of at most two companion
  pixels.  It does not compensate for sub-pixel displacement, local motion,
  rotation, or scale differences.  A poor match can produce a locally inaccurate
  highlight base even though the pass cannot brighten or clip the output.
- Crop previews use a padded high-resolution cache, but recovery is recomputed
  for the preview region.  Full-conversion and crop-preview recovery should be
  considered visually comparable rather than bit-identical until a
  recovery-enabled crop/full regression test exists.
- The Adobe DNG SDK bundle must exist locally under `dng_sdk_1_7_1/` or be provided through `HIRACO_DNG_SDK_ROOT`.
- The GUI controls are live processing overrides for inspection and conversion, not a non-destructive editing history system.
- Linux and Windows build paths exist, but the most frequently exercised environment in this repository is macOS.

## 11. Related Files

- `README.md` for build and usage instructions
- `src/hiraco_core.cpp` for shared preparation, preview, crop, and conversion orchestration
- `src/dng_writer_bridge.cpp` for reconstruction, enhancement, and DNG writing implementation
- `src/hiraco_gui.cpp` for the wxWidgets frontend
