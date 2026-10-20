# Hiraco Project Review

A verified, code-grounded account of what Hiraco is trying to achieve and how the
current implementation actually achieves it. Written 2026-10-09 against commit
`d6f5e2f` ("UI improvements").

## 1. Scope And Method

This review covers the repository as it stands at the audited commit: `src/`,
`tests/`, `CMakeLists.txt`, `CMakePresets.json`, `README.md`, `algorithm.md`,
`design/mockups/`, `.github/agents/`, and git history.

Every non-trivial claim below carries a file anchor and an evidence class:

| Class | Meaning |
| --- | --- |
| **executed** | Observed by building and running the code on this machine (macOS, Apple M-series, 12 logical cores). |
| **read** | Read directly from source; not independently executed. |
| **inferred** | Derived from executed or read evidence; reasoning is stated. |

No product code was modified during this review. All build and conversion
artifacts were written under `tmp/audit/` and `build/`, both gitignored.

## 2. Product Goal And Intended Outcome

**Goal.** Hiraco is a native C++ toolchain that converts selected Olympus / OM
SYSTEM RAW files — with a custom emphasis on high-resolution sensor-shift
captures — into rendered Linear DNG files. (`README.md`, **read**)

**Intended outcome.** A photographer shooting an OM-3 in its 50 MP hand-held or
80 MP tripod high-resolution modes gets a single Linear DNG that:

1. Reconstructs the full-resolution image in the raw domain, using the camera's
   own stack-guidance MakerNote data to direct the reconstruction.
2. Recovers fine detail that the sensor-shift integration destroys, without
   inventing signal or clipping recoverable highlights.
3. Optionally borrows a lower highlight base from the matching single-frame ORI
   to rescue blown highlights — strictly subtractively.
4. Writes a self-contained Linear DNG that downstream software renders
   correctly, with correct camera neutral, crop, and orientation metadata.

**Why this matters.** The alternative — letting LibRaw or a raw developer
demosaic the high-res frame — produces a visibly softer result, because those
tools do not know about the 8-way sensor shift or the guidance maps the camera
embeds. Hiraco's premise is that the camera already solved the hard part and
the solution is in the MakerNote. (`algorithm.md` §1, §3.3, §6, **read**;
premise **inferred** from the architecture).

**Two frontends share one core.** `hiraco-cli` for scripted conversion and
`hiraco-gui` (wxWidgets) for interactive preview, tuning, and batch conversion.
(`README.md`, **read**; both binaries built and the CLI executed, **executed**).

## 3. Repo Layout And Build

| Path | Contents | LoC |
| --- | --- | --- |
| `src/hiraco_core.cpp` / `.h` | Shared orchestration: prepare, metadata, previews, caches, conversion | 1302 / 140 |
| `src/dng_writer_bridge.cpp` / `.h` | Reconstruction, enhancement, DNG packaging | 4734 / 93 |
| `src/hiraco_gui.cpp` | wxWidgets frontend | 3531 |
| `src/vendor_makernote.cpp` / `.h` | Olympus MakerNote parser | 506 / 32 |
| `src/stack_guidance.cpp` / `.h` | Stack-guidance map unpacker | 295 / 25 |
| `src/halide/*.cpp` | Halide AOT generators (à trous wavelet, guided filter) | 212 |
| `src/dng_sdk_bridge.cpp` | DNG SDK capability summary | 98 |
| `src/dng_jxl_stubs.cpp` | JXL decoder stubs for the non-JXL build path | 61 |
| `src/main.cpp` | CLI entry point | 84 |
| `src/hiraco_types.h` | Shared types, defaults, cache structs | 230 |
| `src/hiraco_timing.h` | `HIRACO_TIMING` scoped timers | 83 |
| `tests/hiraco_core_tests.cpp` | Core regression tests | 648 |

CMake targets (**read**, `CMakeLists.txt`): `hiraco-xmp-core`, `hiraco-dng-sdk`,
`hiraco-core` (static), `hiraco-cli`, `hiraco-gui`, `hiraco-core-tests`.
Options `HIRACO_BUILD_CLI`, `HIRACO_BUILD_GUI`, `HIRACO_BUILD_TESTS` all
default `ON`. Presets: `release`, `relwithdebinfo` (Ninja, `build/<preset>`).

**Build result (executed).** `cmake --preset release && cmake --build --preset
release` → 263/263 targets, all binaries linked. Only benign warnings
(`ld: warning: ignoring duplicate libraries: '-lc++'`). No missing dependencies;
the Adobe DNG SDK 1.7.1 bundle is present under `dng_sdk_1_7_1/`.

## 4. End-To-End Pipeline As Built

### 4.1 Prepare — `PrepareSource`

`src/hiraco_core.cpp:622-670`. Steps:

1. Check the source exists.
2. `DecodeWithLibRaw` (`:206-298`) opens the file with LibRaw and populates a
   `DecodeSummary` (`:20-50`): make/model, `libraw_flip`, raw and image
   dimensions, black/white level, `color_matrix1`, `rgb_cam`, as-shot neutral
   (normalized so green = 1), default crop from `raw_inset_crops[0]`, and EXIF
   ISO / shutter / aperture. If required header metadata is missing it calls
   `unpack()` and re-reads.
3. `BuildMetadataFromLibRaw` (`:300-364`) converts the summary into
   `SourceLinearDngMetadata`. White level is normalized to 65535 with a
   bit-shift of the black level when the raw white level is not already 16-bit.
   **Note:** `has_predicted_detail_gain` is set to `true` and
   `predicted_detail_gain` to `1.8` **unconditionally** (`:359-360`).
4. `DetectHighlightRecoverySourcePath` (`:98-127`) looks for an ORI companion
   (see §4.7).
5. `enable_highlight_recovery` is set to `false` (`:664`) — the feature is
   off by default, as documented.
6. `image_width`/`image_height` fall back from raw to image dimensions.
7. Allocates the `PreparedSourceData` cache (`:497-508`).

### 4.2 Lazy Enhancement Metadata — `BuildEnhancementMetadata`

`src/hiraco_core.cpp:408-465`. Built on first use and cached in
`PreparedSourceData::enhancement_metadata` under a mutex (`EnsureEnhancementMetadata`,
`:515-552`).

1. `ReadVendorMakerNote` (`src/vendor_makernote.cpp`) parses the TIFF IFD chain
   to the MakerNote and extracts: TIFF make/model (overriding LibRaw's),
   `StackedImage` label, `UnknownBlock1` (working geometry at offset 23), and
   `UnknownBlock3`.
2. Working geometry is taken from `UnknownBlock1` when present.
3. Stack guidance is extracted when the default crop marks an OM-3 high-res
   frame (origin 6 with width 8160, or origin 8 with width 10368) **and**
   `UnknownBlock3` is exactly 63488 bytes.
4. `ApplyStackGuidanceMaps` (`:366-406`) injects **seven** maps into the
   metadata: `stability`, `mean`, `guide`, `tensor_x`, `tensor_y`,
   `tensor_coherence`, `alias` (`:377` for `stack_mean`). Each is a 64×31 tile
   (`src/stack_guidance.h:8-9`); `ComputeStackGuidance`
   (`src/stack_guidance.cpp:89`) rejects any payload that is not exactly 63488
   bytes.

The code comment at `:437-440` records a real design decision: the label gate
was too narrow, because tripod ORFs and ORIs carry byte-identical `UnknownBlock3`
but different stacked labels — so the gate is now geometry-plus-size only.

### 4.3 Original Preview — `RenderOriginalPreview`

`src/hiraco_core.cpp:673-718`. Returns a cached copy when the LibRaw overrides
match; otherwise `BuildOriginalPreviewFromRaw`
(`src/dng_writer_bridge.cpp:3939-3976`) renders through LibRaw with
`BuildPreviewRenderSettings`, scales to the requested size, and applies
`ApplyLibRawOrientationToPreview` (`:3552`) so the preview respects the sensor
flip. Cached in `PreparedSourceData::original_preview`.

### 4.4 Converted Preview — `RenderConvertedFullPreview`

`src/hiraco_core.cpp:878-1110`. This is the GUI's responsive full-frame preview
and the most intricate orchestration in the core.

1. Cache key is `(max_dimension, stage_overrides, libraw_overrides,
   highlight_recovery_enabled)`; a matching cached preview is returned as-is.
2. Ensures enhancement metadata and the preview auto-bright gain
   (`EnsurePreviewAutoBrightGain`, `:554-589`, which estimates a gain from the
   99th-percentile maximum of a linear probe render, clamped to [1, 64]).
3. Computes the preview crop from the default crop when present.
4. Reuses the full-resolution `ProcessingCache` if it covers the crop
   (`ProcessingCacheCoversCrop`, `:181`); otherwise builds one via
   `BuildProcessingCacheFromRaw` (`src/dng_writer_bridge.cpp:4030-4121`).
5. Builds a **display-resolution derivative** via `BuildDisplayProcessingCache`
   (`:4123-4165`) — the expensive enhancement work is bounded by the canvas, not
   by the 50/80 MP source.
6. Maps the crop into display coordinates and computes
   `RecoveryImageCoordinates` so the ORI companion can be sampled in source
   coordinates even though the preview runs at display resolution.
7. Calls `RenderConvertedCropPreview` (`:4184-4351`) on the display cache.

`RenderConvertedCropPreview` applies the same enhancement and recovery stages as
full conversion, then `BuildDisplayPreviewFromRaster` (`:3607`) produces the
8-bit oriented preview.

### 4.5 Full Conversion — `ConvertToDng`

`src/hiraco_core.cpp:1112-1148`. Reuses the cached original preview as the
embedded DNG preview and the cached auto-bright gain, then calls
`WriteLinearDngFromRaw` (`src/dng_writer_bridge.cpp:4377-4553`):

1. Validate compression.
2. Build or reuse the embedded preview (scaled to 1024 px, oriented).
3. `BuildLinearDngPayload` (`:3373-3418`) — see §4.6.
4. Resolve stage settings.
5. **Camera-space path** (OM-3 high-res): subtract the pedestal, run
   `ApplyPredictedDetailGain` with `pixel_ceiling = 65535 - pedestal`, run
   `ApplyOriAssistedHighlightRecovery` if enabled, then
   `ReapplyCameraSpacePedestal`.
6. **LibRaw path** (everything else): run `ApplyPredictedDetailGain` with
   `pixel_ceiling = 65535`, then `ApplyLinearDngRasterTransform` (`:1096`).
7. Write the DNG through the Adobe SDK.

### 4.6 High-Resolution Raw-Domain Reconstruction

Two entry points: `RenderOm3RawDomainImage` (`src/dng_writer_bridge.cpp:2126`
wrapper, `:2592-3372` implementation) and `BuildLinearDngPayload`
(`:3373-3418`), which also extracts a 4-channel CFA guide image concurrently
via `std::async` (`:3391-3400`) when enhancement will need it.

`RenderOm3RawDomainImage` stages:

1. **Load** (`:2611-2692`). LibRaw `open_file` + `unpack`, then
   `ApplyOm3HighResCfaPrecondition` (`:1930`) adjusts the LibRaw processor for
   the OM-3 high-res CFA. The raw mosaic is normalized into a 16-bit working
   domain: `(raw - raw_black) * scale` clamped to `[0, 65535 - pedestal]`
   (`:2660-2686`).
2. **Green base plane** (`:2814-2872`). Green sites pass through; non-green
   sites get a gradient-weighted directional interpolation
   (`horiz`/`vert` estimates blended by inverse-gradient weights, with a 1.8:1
   anisotropy threshold that picks one direction outright).
3. **Guide-aware refinement** (`:2900-3095`). When a `stack_guide` map exists,
   the guide is calibrated against the reconstructed green by least squares
   (`gain = cov/var`, clamped to [0.05, 64], plus a bias term), then upsampled
   with a Gaussian-weighted bilinear kernel whose sigma adapts to local green
   level. The refined green blends the guide estimate in, weighted by stability
   and alias confidence, with tensor penalties
   (`1 + 0.85 * coherence * tensor`) suppressing unreliable directions.
4. **Stability-driven detail lift** (`:3185-3245`). A 5-tap binomial blur of
   green gives a detail signal, modulated by stability, a green-split
   confidence term, alias confidence, and guide edge confidence. The detail is
   capped at `±(0.45 * cap_scale) * local_scale + 320`.
5. **RGB reconstruction** (`:3252-3368`). Red/blue are recovered as
   `green + difference`, with the difference interpolated by guide-weighted
   axis and diagonal estimators.

**Headroom invariant.** `limit_highlight_increase` (`:2702-2718`) is applied to
both the green refinement and the detail lift. For a normalized original
`n = original / max_signal`, the candidate is capped at
`max_signal * (2n - n²)`, which is strictly below `max_signal` for `0 < n < 1`.
So a sample that was below sensor white cannot be pushed to white by
reconstruction, and an already-clipped sample stays identifiable as clipped.
This is the mechanism behind `algorithm.md` §6 step 8, and it is the same
invariant the enhancement stage uses (§4.8).

**Guidance-map sampling.** Five of the seven maps (`stability`, `alias`,
`tensor_x`, `tensor_y`, `tensor_coherence`) are sampled through precomputed
`RegionMapSampler`s (`MakeRegionMapSampler`, `:726`) that bilinearly interpolate
the 64×31 tiles on demand (`:2778-2806`). The `stack_guide` map is the exception: it is
materialized into a full-resolution `upsampled_stack_guide` buffer (`:2978`)
because the refinement pass needs guide values at every neighbour of every
pixel. See drift D5.

### 4.7 ORI-Assisted Highlight Recovery

`ApplyOriAssistedHighlightRecovery` (`src/dng_writer_bridge.cpp:2187-2590`).
Gated on: image has ≥3 colours, a companion path is present,
`IsOm3HighResMetadata`, black level and as-shot neutral are known. Returns
`true` (no-op) otherwise.

1. **Companion cache.** A single-entry global cache
   (`g_highlight_recovery_companion_cache`, `:208-211`) holds the last rendered
   companion, so repeated previews and a following conversion do not rebuild
   it. On a miss, the companion is prepared and rendered through
   `RenderOm3RawDomainImage` and cached.
2. **Registration** (`:2291-2336`). Searches integer offsets `dx, dy ∈ [-2, 2]`
   and picks the one minimizing green-channel mean absolute error, sampled on a
   64-pixel stride over the companion's active rect.
3. **Low-frequency base** (`:2352-2452`). Both the target and the companion are
   sampled on a 16-pixel stride into per-channel grids, then each grid gets two
   separable 5-tap `[1,4,6,4,1]/16` blur passes. Bilinear sampling of the
   blurred grids gives the low-frequency base for each image.
4. **Application** (`:2484-2562`). For each pixel whose brightest channel is
   above 55% of the signal range (ramping to full strength at 85%):
   - skip if the companion's green base is below 32;
   - skip if the companion base is dark while the target base is bright
     (misalignment guard, `:2344-2345`);
   - skip if the companion's blurred luma is not lower than the target's;
   - otherwise scale each channel by `1 + blend * (min(base_ratio, 1) - 1)`,
     clamped to `[0, 1]`.

**Subtractive by construction.** `channel_scale` is clamped to `[0, 1]`
(`:2561`) and `base_ratio` is capped at 1.0 (`:2557`), so the pass can only
reduce a channel. It cannot
brighten a sample, invent signal, or turn a safe sample into a clipped one.
This matches `algorithm.md` §6 step 11 and §10.

**Companion detection order** (`src/hiraco_core.cpp:98-127`), first existing
winning: `<stem>.ORI`, `<stem>.ori`, `<stem>_.ORI`, `<stem>_.ori`,
`<stem>_<ext>`, `<stem>_.<opposite-case ext>`. Only `.ORF`/`.orf` sources are
considered, and only when `IsOm3HighResSummary` matches (50 MP: crop origin
(6,6), 8160×6120; 80 MP: crop origin (8,8), 10368×7776 — `:84-96`).

### 4.8 Enhancement Pipeline

`ApplyPredictedDetailGain` (`src/dng_writer_bridge.cpp:1103-1802`). Five
stages, all operating on a BT.709 luma channel in linear light:

| Stage | Lines | What it does |
| --- | --- | --- |
| 0 | 1145-1388 | Extract luma; build a confidence map from the CFA guide (green-phase disagreement, robustly normalized, 4 blur passes) and a signal-weight map that lifts enhancement in brighter regions. |
| 1 | 1390-1583 | **Wiener deconvolution.** Mirror-padded 32 px, FFTW-efficient sizes, cached plans. The blur model is `H_optics * H_diode` where `H_diode = sinc(2fx) * sinc(2fy)` — the 2× sensor-shift integration box, which naturally notches the Bayer-residual frequencies. A gradient gate suppresses ringing at strong edges; a CFA-residual smoothing blend handles clipped regions. |
| 2 | 1584-1723 | **Multi-scale à trous wavelet.** 4 scales, dilated B3-spline via Halide AOT. Per-scale MAD noise estimation, BayesShrink threshold with floor/ceiling, Poisson-scaled local threshold, and bidirectional gain blending (0.68/0.32, `:1594-1616`) so the bands overlap. The finest band's gain is derived from the `Small Detail` control with 1.6× sensitivity (`DeriveFineScaleGainFromSmall`, `:133`). |
| 3 | 1724-1744 | **Guided-filter edge refinement**, Halide AOT, self-guided with the stage-0 confidence as guidance and an adaptive gain of `(base_gain - 1)`. |
| 4 | 1745-1800 | **Ratio transfer.** `ratio = enhanced_luma / original_luma`, clamped to [0.3, 4.0], blended toward 1.0 below luma 1500, then capped by `min(ratio, 2 - n)` where `n = max_channel / pixel_ceiling`. |

The stage-4 cap is the same `2n - n²` invariant as the reconstruction path,
applied per channel against the brightest input channel. The code comment at
`:1760-1766` states the reasoning explicitly: clamping at the ceiling would
turn every overshoot into a newly clipped sample, which is irreversible in a
linear DNG.

**Gating.** `ShouldApplyPredictedDetailGain` (`:924-930`) requires
`has_predicted_detail_gain && predicted_detail_gain > 1.0001 && colors == 3 &&
IsOm3HighResMetadata(metadata)`. Because of the geometry gate, **standard
20 MP frames receive no enhancement at all** — verified empirically (§7).

### 4.9 DNG Packaging

`PopulateLinearRawNegative` (`src/dng_writer_bridge.cpp:3798-3873`) and
`WriteLinearDngFromRaw` (`:4377-4553`).

- Model/local names, original raw filename, colour channels, orientation
  (via `LibRawFlipToTiffOrientation`, `src/hiraco_types.h:184`), default crop
  origin and size, default scale 1:1.
- `SetBlackLevel(metadata.black_level)` only when
  `ShouldApplyOm3SourceDrivenLinearTransform` (`:877-902`) — which accepts 50 MP,
  80 MP, **and** standard 20 MP geometry. Otherwise black level 0.
- `SetWhiteLevel(65535)`, `SetBaselineExposure(0)`,
  `SetLinearResponseLimit(1)`. The comment at `:3829-3831` is explicit that
  `preview_auto_bright_gain` is a GUI display convenience only and is
  deliberately **not** embedded, because doing so would make downstream apps
  render the DNG several EV brighter than the original raw.
- Profile: `Adobe Standard` with the camera's as-shot neutral when
  `ShouldUseOm3AdobeMetadata` (`:932-957`) matches; otherwise
  `hiraco-linear-srgb` with an identity camera neutral and D50 white point.
- EXIF make/model/ISO/shutter/aperture carried forward; date set to now.
- An 8-bit sRGB preview is embedded (`AppendImagePreview`, `:3875`).
- Compression: `uncompressed` and `deflate` → DNG 1.6.0.0 requested;
  `jpeg-xl` → `dngVersion_SaveDefault` with `LosslessCompressJXL`
  (`:4528`). `ConfigureHost` (`:1810-1814`) sets the DNG version, linear-DNG
  flag, and JXL flags.

## 5. Performance And Memory Design As Built

| Mechanism | Anchor | Evidence |
| --- | --- | --- |
| OpenMP over large per-pixel loops | throughout `dng_writer_bridge.cpp` (`:1150`, `:1290`, `:1470`, `:1500`, `:1600`, `:1660`, `:1750`, `:2660`, `:2810`, `:3190`, `:3250`) | read |
| FFTW threading + wisdom reuse | `InitFftwThreads` `:217`, `FftwWisdomPath` `:227` (`~/.hiraco_fftw_wisdom`), `LoadFftwWisdom`/`SaveFftwWisdom` `:234`/`:242`, plan cache keyed by dimensions `:250-294` | read; "FFTW using 12 threads" **executed** |
| Halide AOT kernels | `src/halide/wavelet_generator.cpp` (à trous, dilated B3-spline, repeat-edge), `src/halide/guided_filter_generator.cpp` (box-filter guided filter with confidence input) | read |
| Lazy enhancement metadata | `EnsureEnhancementMetadata` `hiraco_core.cpp:515-552` | read |
| Cached original preview reuse | `RenderOriginalPreview` `:673-718`; reused as the embedded DNG preview in `ConvertToDng` `:1125-1135` | read |
| Immutable full-resolution processing cache + display-resolution derivative | `PreparedSourceData` `:497-508`, `BuildDisplayProcessingCache` `dng_writer_bridge.cpp:4123-4165` | read |
| Single bounded GUI processing dispatcher | `ProcessingTaskQueue` `hiraco_gui.cpp:126-208` — one worker thread, `kInteractive`/`kBackground` priority deques, `DiscardPending` | read |
| Latest-wins slider | `hiraco_gui.cpp:2158-2196` — `wxEVT_SLIDER` is settings-only; `wxEVT_SCROLL_THUMBTRACK` begins a drag; `wxEVT_SCROLL_THUMBRELEASE` queues one preview | read |
| 2 GB LRU preview cache budget | `kPreviewCacheBudgetBytes = 2ull*1024*1024*1024` `hiraco_gui.cpp:58`; `EnforcePreviewCacheBudget` `:2592-2628` evicts the oldest-access non-selected item via `ReleasePreviewProcessingCache` | read |
| Batch export releases recomputable caches | `hiraco_gui.cpp:3284-3296` — releases all preview caches before conversion starts while keeping the displayed bitmap | read |
| Single-entry ORI companion cache | `g_highlight_recovery_companion_cache` `dng_writer_bridge.cpp:208-211` | read |
| Optional timing logs | `src/hiraco_timing.h`, `HIRACO_TIMING` env var, CLI `--debug` (`src/main.cpp:33-42`) | read + executed |
| Concurrent CFA guide extraction | `std::async` in `BuildLinearDngPayload` `dng_writer_bridge.cpp:3391-3400` | read |

## 6. GUI As Built Versus `algorithm.md` §11

| §11 item | Status | Anchor |
| --- | --- | --- |
| 11.1 Keep wxWidgets; replace crop panel with full-image converted preview | **Done** | `CompareCanvas` with `kOriginal`/`kConverted`/`kSideBySide`, `hiraco_gui.cpp:681`, `:719-786` |
| 11.2 Left thumbnail strip, central canvas, right inspector | **Done** | thumbnails `:423`, cards `:2462-2560`, `RemoveQueueRows` `:2652`, context menu `:3134`; zoom `kFit`/`k50`/`k100` `:1797-1801` |
| 11.3 `Small`/`Medium`/`Strong` presets, manual edit marks custom | **Done** (see D3) | buttons `:1370-1377`, `:1637-1644`; `BuiltInPresetSettings` `:1853`; `UpdatePresetButtons` `:1822-1832` |
| 11.4 Output Options with three explicit destinations | **Done** | `wxCollapsiblePane` `:1342`, radio buttons `:1346-1358` |
| 11.5.1 Cancelable cached converted full-preview API | **Done** | `RenderConvertedFullPreview` `hiraco_core.cpp:878`; `InvalidateConvertedPreviewRequests` `hiraco_gui.cpp:2090`; atomic cancel `:2849` |
| 11.5.2 Full-image comparison canvas, synced zoom/pan | **Done** | `CompareCanvas` |
| 11.5.3 `wxListCtrl` replaced with thumbnail rows + per-image remove | **Done** | no `wxListCtrl` anywhere in `hiraco_gui.cpp` |
| 11.5.4 Inspector rebuilt around settings, presets, output options | **Done** | right panel sections with per-section reset `:1548` |
| 11.5.5 Benchmark on M4 Pro + core tests for dimensions, orientation, cancellation, cache reuse | **Not done** | no benchmark code or results in the repo; `tests/hiraco_core_tests.cpp` has 12 tests covering dimensions/scaling and crop consistency but **no orientation, cancellation, or cache-reuse test** |

## 7. Empirical Validation

### 7.1 Build And Tests (executed)

| Command | Result |
| --- | --- |
| `cmake --preset release && cmake --build --preset release` | 263/263 targets, exit 0 |
| `ctest --test-dir build/release --output-on-failure` | 1/1 passed, 5.04 s |
| `cmake --preset relwithdebinfo -DHIRACO_BUILD_TESTS=ON` + build `hiraco-core-tests` + `ctest` | 1/1 passed, 4.48 s |

The 12 tests are: `TestResolveOutputPath`, `TestOverwriteDecision`,
`TestCropHelpers`, `TestResolvedStageDefaults`,
`TestEnhancementPreservesHighlightHeadroom`, `TestRealOrfPreviewSmoke`,
`TestRealOrfCropPreviewFollowsCropRect`,
`TestRealOrfPreviewAutoBrightGainEstimate`,
`TestLocalOm3RecoveryFullPreviewSmoke`, `TestCropPreviewMatchesFullImageCrop`,
`TestDisplayProcessingCacheScalesBothInputs`,
`TestStage2SmallMediumLargeGainResponse` (`tests/hiraco_core_tests.cpp:627-646`).

### 7.2 Conversion: OM-3 80 MP High-Res (executed)

Source `sample/_8142490.ORF` — OM-3, 10386×7792, ISO 200, 1/320 s, f/5.0, with
companion `sample/_8142490.ORI` present.

| Compression | Output size | Real time | Compression tag | DNG version |
| --- | --- | --- | --- | --- |
| `uncompressed` | 487,932,530 B | 9.46 s | 1 | 1.4.0.0 |
| `deflate` | 300,605,914 B | 9.08 s | **7** | 1.4.0.0 |
| `jpeg-xl` | 253,823,216 B | **112.87 s** | 52546 | 1.7.0.0 |

All three outputs: PhotometricInterpretation 34892 (Linear Raw), profile
`Adobe Standard`, AsShotNeutral `0.410256 1 0.618357`, BlackLevel `0 0 0`,
WhiteLevel `65535`, BaselineExposure `0`, LinearResponseLimit `1`,
DefaultCropOrigin `8 8`, DefaultCropSize `10368×7776`, UniqueCameraModel
`OM Digital Solutions OM-3`, and an embedded 2,359,512-byte sRGB preview TIFF
labelled `hiraco`.

Timing breakdown (deflate run, ms):

| Phase | Time | Share |
| --- | --- | --- |
| Render raw domain image | 2202.8 | |
| — Load mosaic and CFA | 1395.2 | 63 % |
| — Reconstruct green base plane | 56.4 | |
| — Guide-aware green refinement | 327.3 | |
| — Stability-driven green detail lift | 270.5 | |
| — Reconstruct RGB output | 106.3 | |
| Apply predicted detail gain | 2825.7 | |
| — Stage 0 prepare luma | 1057.9 | |
| — Stage 1 Wiener deconvolution | 1158.8 | |
| — Stage 2 wavelet refinement | 327.8 | |
| — Stage 3 guided refinement | 199.8 | |
| — Stage 4 ratio transfer | 81.2 | |
| Write DNG stream | 2494.8 | |
| **Total** | **8935.7** | |

### 7.3 Conversion: Standard 20 MP (executed)

Source `reference/_3060001.ORF` → 5220×3912, `Render LibRaw payload` 528.1 ms,
`Apply predicted detail gain` **0.0 ms** (skipped), Compression 7, DNG 1.4.0.0,
`Adobe Standard`, AsShotNeutral `0.422442 1 0.603774`, total 1.65 s.

### 7.4 Findings

**F1 — JPEG-XL compression is unmeasured and unreported (performance, high
impact).** The jpeg-xl run took 112,799.7 ms in total, but the timed
`Write DNG stream` block only accounts for 546.8 ms. The missing ~112.2 s is
`negative->LosslessCompressJXL(host, writer, false)` at
`src/dng_writer_bridge.cpp:4528`, which sits **outside** the
`ScopedTimingLog` block that begins at `:4533`. Consequences: the timing logs
under-report jpeg-xl by ~200×, and the progress callback is stuck at 0.92 for
roughly two minutes, so the CLI and GUI give the user no feedback during the
longest phase of the longest mode.

**F2 — `deflate` does not produce deflate (correctness, high impact).**
hiraco never calls `host.SetCompression()`; `ConfigureHost`
(`src/dng_writer_bridge.cpp:1810-1814`) only sets the DNG version, the
linear-DNG flag, and the JXL flags. The DNG SDK chooses the compression code
itself, and `dng_sdk_1_7_1/dng_sdk/source/dng_image_writer.cpp:7640-7658`
selects `ccJPEG` (7) for 16-bit `ttShort` images, reserving `ccDeflate` (8) for
floating-point and 32-bit-integer images. Verified: the `deflate` output carries
Compression tag 7. The user-facing label is wrong, and downstream software will
read the file as lossless-JPEG rather than deflate.

**F3 — Requested DNG version is not the written DNG version (metadata).**
`DngVersionForCompression` (`src/dng_writer_bridge.cpp:1803-1808`) requests
`dngVersion_1_6_0_0` (0x01060000) for every non-JXL mode, but the written
`DNGVersion` tag is `1.4.0.0`. The SDK clamps to the minimum version required
by the features actually used. The code's intent and the file's content
disagree.

**F4 — Enhancement is silently skipped for standard 20 MP frames
(correctness/coverage).** `ShouldApplyPredictedDetailGain`
(`src/dng_writer_bridge.cpp:924-930`) requires `IsOm3HighResMetadata`, so the
20 MP path renders and packages without any detail recovery. `algorithm.md` §7
presents the enhancement pipeline without stating this geometry gate, and
`README.md` implies it applies to supported inputs generally.

**F5 — `Load mosaic and CFA` dominates the raw-domain stage (performance).**
At 1395.2 ms it is 63 % of the raw-domain render and 16 % of total conversion
time. The loop is already OpenMP-parallel (`:2660`), so the cost is likely
LibRaw's `raw_image` access pattern rather than raw throughput.

**F6 — Dead code from the guidance-map optimization (maintainability).**
`UpsampleFloatMap` (`:583`) and `UpsampleFloatMapRegion` (`:631`) are defined
but never called. `Is80MpFrame` (`:121`) is unused in the writer (a separate
overload exists at `hiraco_gui.cpp:221`). `algorithm.md` §9 presents the
removal of full-frame upsample buffers as complete, which is not quite true.

## 8. Doc↔Code Drift Table

Covers `README.md` and `algorithm.md` §1–§12. "Verified" means the claim holds
as written; "Drift" means the code differs; "Unverified" means the claim could
not be confirmed either way.

| # | Doc claim | Location | Status | Code reality |
| --- | --- | --- | --- | --- |
| D1 | Companion candidate 6 "contains two dots; for example, `<stem>_..orf`" | `algorithm.md` §6.1 | **Drift** | `hiraco_core.cpp:118` builds `stem + "_" + ".orf"` → `<stem>_.orf`, **one** dot. |
| D2 | Enhancement "is only enabled when the source metadata requests predicted detail gain" | `algorithm.md` §7 | **Drift** | `has_predicted_detail_gain` is set unconditionally (`hiraco_core.cpp:359`), but `ShouldApplyPredictedDetailGain` (`dng_writer_bridge.cpp:924`) additionally requires `IsOm3HighResMetadata` — so 20 MP frames get none. |
| D3 | Preset values "must be validated against representative 20 MP, 50 MP, and 80 MP sources before they become user-facing defaults" | `algorithm.md` §11.3 | **Drift** | `ResolveStageSettingsForImageImpl` (`dng_writer_bridge.cpp:143-215`) ignores metadata, width, and height; returns static defaults. `Is50MpFrame`/`Is80MpFrame`/`Is20MpFrame` (`hiraco_gui.cpp:211-239`) feed only the resolution label. |
| D4 | "Benchmark full-preview refresh on the M4 Pro and add core regression tests for full-preview dimensions, orientation, cancellation, and cache reuse" | `algorithm.md` §11.5 | **Drift** | No benchmark exists anywhere in the repo. `tests/hiraco_core_tests.cpp:627-646` runs 12 tests covering dimensions/scaling and crop consistency, but not orientation, cancellation, or cache reuse. |
| D5 | "Recent raw-domain optimizations also removed several large full-frame temporary map upsample buffers" | `algorithm.md` §9 | **Drift** | Partly. `UpsampleFloatMap`/`UpsampleFloatMapRegion` (`:583`, `:631`) are now dead code, but `upsampled_stack_guide` (`:2978`) still materializes one full-frame buffer. |
| D6 | "Compression modes currently exposed by the CLI and GUI are: `uncompressed`, `deflate`, and `jpeg-xl`" | `README.md` | **Drift** | `deflate` produces Compression tag 7 (lossless JPEG), not 8 (deflate). See F2. |
| D7 | (implicit) non-JXL output is DNG 1.6.0.0 | `algorithm.md` §8, `dng_sdk_bridge.cpp:52` | **Drift** | Code requests 1.6.0.0; written tag is 1.4.0.0. See F3. |
| D8 | Guidance maps listed: stability, guide, tensor_x, tensor_y, tensor_coherence, alias | `algorithm.md` §3.3 | **Drift** | Seven maps are injected; `stack_mean` is the seventh (`hiraco_core.cpp:377`). |
| D9 | Recovery "searches a global integer offset from -2 to +2 companion pixels using green-channel mean absolute error" | `algorithm.md` §6 step 11 | **Verified, incomplete** | Confirmed (`max_offset = 2`, `:2291`), but the search samples at stride 64, not per pixel (`:2292`). Doc also omits `kMinCompanionGreen = 32` (`:2340`) and the misalignment guard (`kMisalignmentGreenFloor 0.05`, `kMisalignmentTargetFloor 0.55`, `:2344-2345`). |
| D10 | "build a confidence map from the CFA guide image when available" | `algorithm.md` §7.1 | **Verified, incomplete** | Confirmed at `src/dng_writer_bridge.cpp:1279-1383`, but the doc omits the 4-pass confidence blur (`kConfidenceBlurPasses = 4`, `:1360`), the robust median-residual normalization, and the signal-weight lift (`:1346-1355`). |
| D11 | "drive the finest wavelet band from the `Small Detail` control with extra sensitivity" | `algorithm.md` §7.2 | **Verified** | `DeriveFineScaleGainFromSmall` (`:133-135`): `gain0 = 1 + 1.6 * (gain1 - 1)`, clamped to [0.25, 4.0]. |
| D12 | "blend adjacent scale gains in both directions" | `algorithm.md` §7.2 | **Verified** | `:1600-1616`: top-down and bottom-up passes at 0.68/0.32, combined 0.50/0.25/0.25. |
| D13 | "a 2 GB LRU budget for retained GUI preview caches, leaving headroom within a 10 GB working-set allowance" | `algorithm.md` §9 | **Verified** | `kPreviewCacheBudgetBytes = 2ull*1024*1024*1024` (`hiraco_gui.cpp:58`); the 10 GB figure appears in the comment at `:57`. |
| D14 | "one bounded GUI processing dispatcher: interactive preview work takes priority over queued metadata work" | `algorithm.md` §9 | **Verified** | `ProcessingTaskQueue` (`hiraco_gui.cpp:126-208`). |
| D15 | "a slider drag is visual-only; its final value queues one latest-wins preview on release" | `algorithm.md` §9 | **Verified** | `hiraco_gui.cpp:2158-2196`. |
| D16 | "GUI batch export releases recomputable preview-processing caches before it starts" | `algorithm.md` §9 | **Verified** | `hiraco_gui.cpp:3284-3296`. |
| D17 | "one immutable full-resolution ORI companion cache" | `algorithm.md` §9 | **Verified** | `g_highlight_recovery_companion_cache` (`dng_writer_bridge.cpp:208-211`), single entry. |
| D18 | "Converted previews, including recovery-enabled previews, use a display-resolution cache. Full conversion remains full-resolution." | `algorithm.md` §10 | **Verified** | `BuildDisplayProcessingCache` (`:4123`) for previews; `WriteLinearDngFromRaw` renders at full resolution. |
| D19 | "Recovery registration is a global integer offset of at most two companion pixels. It does not compensate for sub-pixel displacement, local motion, rotation, or scale differences." | `algorithm.md` §10 | **Verified** | `max_offset = 2` (`:2291`); integer offsets only. |
| D20 | "The strongest custom path is the high-resolution workflow." | `README.md`, `algorithm.md` §10 | **Verified** | `RenderOm3RawDomainImage` (`src/dng_writer_bridge.cpp:2592`) is reached only via `IsOm3HighResMetadata` (`:78-90`). |
| D21 | "Optional, per-file ORI-assisted highlight recovery … disabled by default and … subtractive" | `README.md` | **Verified** | `enable_highlight_recovery = false` (`hiraco_core.cpp:664`); `channel_scale` clamped to `[0,1]` (`dng_writer_bridge.cpp:2561`). |
| D22 | "The CLI does not currently expose a switch for this feature." | `README.md` | **Verified** | `src/main.cpp` accepts only `convert <source> <output> [--compression …] [--debug]`. |
| D23 | "Cached original previews, full-resolution processing data, and display-resolution processing derivatives" | `README.md` | **Verified** | `PreparedSourceData` (`hiraco_core.cpp:493-508`). |
| D24 | "Optional timing output for performance work via `--debug` or `HIRACO_TIMING=1`" | `README.md` | **Verified** | `src/main.cpp:33-42`, `src/hiraco_timing.h:11-29`. |
| D25 | "Three user-facing processing control groups in the GUI: Detail Recovery, Multi-scale Detail, Edge Refinement" | `README.md` | **Verified** | Right-panel sections with per-section reset (`hiraco_gui.cpp:1548`). |
| D26 | Core entry points are `PrepareSource`, `RenderOriginalPreview`, `RenderConvertedFullPreview`, `ConvertToDng` | `algorithm.md` §2 | **Verified** | All four present (`hiraco_core.cpp:622`, `:673`, `:878`, `:1112`), plus `RenderConvertedCrop` (`:781`) which the doc mentions separately as a regression entry point. |
| D27 | "Restore the camera-space black-level pedestal and write the final DNG" | `algorithm.md` §5 | **Verified, with caveat** | `ReapplyCameraSpacePedestal` (`dng_writer_bridge.cpp:1078`) confirmed, but `metadata.black_level` is 0 for the OM-3, so the pedestal is 0 and the subtract/reapply is a no-op in practice. |
| D28 | "Normalize raw values into a 16-bit working domain using black and white level information" | `algorithm.md` §6 step 2 | **Verified** | `:2668-2685`. |
| D29 | "Sample those maps through precomputed bilinear samplers rather than materializing large upsampled full-frame buffers" | `algorithm.md` §6 step 4 | **Drift** | True for 5 of 7 maps; `stack_guide` is materialized full-frame (`:2978`). See D5. |
| D30 | "Constrain every positive green reconstruction adjustment by its local linear headroom." | `algorithm.md` §6 step 8 | **Verified** | `limit_highlight_increase` (`:2702-2718`). |
| D31 | "Preserve the linear camera-space signal … Do not apply a pixel-domain highlight shoulder." | `algorithm.md` §6 step 10 | **Verified** | `ApplyLinearDngRasterTransform` is called only on the LibRaw path (`:4287`, `:4474`). |
| D32 | "Recovery runs after the main detail pipeline but before the camera black pedestal is restored." | `algorithm.md` §6 step 11 | **Verified** | `WriteLinearDngFromRaw` `:4497-4525`. |
| D33 | "The output is intended to behave as a robust rendered linear image in downstream software" | `algorithm.md` §8 | **Verified** | `PopulateLinearRawNegative` (`:3798-3873`). |
| D34 | "Replace the separate converted-crop panel and movable crop workflow with a full-image converted preview." | `algorithm.md` §11.1 | **Verified** | Done. |
| D35 | Workspace: left strip / central canvas / right inspector, three comparison modes, Fit/50%/100% zoom, pan | `algorithm.md` §11.2 | **Verified** | Done. |
| D36 | Output card with compression plus expandable Output Options offering three mutually exclusive destinations | `algorithm.md` §11.4 | **Verified** | Done. |
| D37 | "The test target currently covers core-library behavior rather than full GUI automation." | `README.md` | **Verified** | 12 core tests (`tests/hiraco_core_tests.cpp:627-646`), no GUI automation. |
| D38 | "Linux and Windows build paths exist, but the most frequently exercised environment … is macOS." | `algorithm.md` §10 | **Verified** | `README.md` documents macOS brew, Linux apt, and Windows vcpkg; only macOS was exercised here. |
| D39 | Requirements: CMake 3.28+, LibRaw, FFTW3 + threads, Halide, Zlib, OpenMP, wxWidgets, DNG SDK 1.7.1 | `README.md` | **Verified** | All present in `CMakeLists.txt` and used; release build succeeded (263/263 targets). |
| D40 | "The repository also expects the bundled Adobe libjxl and XMP toolkit that ship with the DNG SDK bundle" | `README.md` | **Verified** | `dng_sdk_1_7_1/` present; the `hiraco-xmp-core` and `hiraco-dng-sdk` targets built. |

## 9. Current Limits And Assumptions

**Confirmed limits** (as documented, verified against code):

- The strongest custom path is the high-resolution workflow; standard frames go
  through LibRaw and receive no enhancement (D2, F4).
- ORI-assisted highlight recovery is an opt-in, subtractive correction, not a
  matched HDR merge.
- Recovery registration is a global integer offset of at most two companion
  pixels; no sub-pixel, motion, rotation, or scale compensation.
- Converted previews use a display-resolution cache; full conversion remains
  full-resolution.
- The DNG SDK bundle must exist locally under `dng_sdk_1_7_1/` or be provided
  through `HIRACO_DNG_SDK_ROOT`.
- GUI controls are live processing overrides, not a non-destructive editing
  history.
- Linux and Windows build paths exist but only macOS was exercised.

**Newly identified limits** (not documented):

- JPEG-XL conversion is ~12× slower than deflate (112.9 s vs 9.1 s on an 80 MP
  frame) and provides no progress feedback for ~2 minutes (F1).
- The `deflate` label is inaccurate (F2).
- The requested and written DNG versions disagree (F3).
- `metadata.black_level` is 0 for the OM-3, so the camera-space pedestal
  subtract/reapply is currently a no-op (D27).
- `BuildLinearDngPayload` and `BuildProcessingCacheFromRaw` both accept
  highlight-recovery parameters and immediately discard them with `(void)`
  casts (`:3381-3382`, `:4043-4044`). This is deliberate — `algorithm.md` §9
  states recovery is applied outside the processing cache so the per-file
  switch takes effect immediately — but the unused parameters are a
  maintainability hazard.
- `ApplyResolvedStageSettingsForTesting` (`:4167`) exists to let tests drive
  the enhancement pipeline with explicit settings.

## 10. Evidence Appendix

| Artifact | Location |
| --- | --- |
| Release configure log | `tmp/audit/configure-release.log` |
| Release build log | `tmp/audit/build-release.log` |
| RelWithDebInfo configure log | `tmp/audit/configure-relwithdebinfo.log` |
| Test build log | `tmp/audit/build-tests.log` |
| Test run log | `tmp/audit/ctest-relwithdebinfo.log` |
| Conversion outputs (gitignored) | `tmp/audit/out/` |

All logs and outputs are under gitignored paths. `git status --short` shows
only this document and `docs/improvement-backlog.md` as changes.
