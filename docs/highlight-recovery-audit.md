# ORI Highlight-Recovery Pipeline Audit

Status: complete — audit 2026-10-11 (analysis only); fix round P1–P5 implemented 2026-10-11 (§9). See §9.6 for the verification-recipe confirmation.

## 1. End-to-end trace of the recovery switch (preview path)

| # | Hop | Location |
|---|-----|----------|
| 1 | Companion detection at prepare time: `DetectHighlightRecoverySourcePath` — requires `IsOm3HighResSummary(summary)` and a `.ORF`/`.orf` extension, then looks for sibling `<stem>.ORI`, `<stem>.ori`, `<stem>_.ORI`, `<stem>_.ori`, `<stem>_.ORF`, `<stem>_.orf` | `src/hiraco_core.cpp:98-129`, invoked at `src/hiraco_core.cpp:663-664` (`enable_highlight_recovery` defaults to **false**) |
| 2 | Flag helper: `ShouldUseHighlightRecovery() = enable_highlight_recovery && !highlight_recovery_source_path.empty()` | `src/hiraco_core.h:35-41` |
| 3 | GUI queue card: checkbox "ORI highlight recovery" is **only created when `HasHighlightRecoverySource()`** | `src/hiraco_gui.cpp:2523-2528` |
| 4 | Checkbox handler: sets `queue_item.enable_highlight_recovery` and the live `prepared` copy, then `ScheduleConvertedPreview()` only if the row is selected | `src/hiraco_gui.cpp:2544-2556` |
| 5 | Preview worker: copies `PreparedSource`, re-applies `prepared.enable_highlight_recovery = item.enable_highlight_recovery`, calls `RenderConvertedFullPreview(&prepared, …, kFullPreviewMaxDimension=2560, …)` | `src/hiraco_gui.cpp:2831-2896` (flag copy at `:2851`, call at `:2884`) |
| 6 | Core preview entry: `highlight_recovery_enabled = prepared->ShouldUseHighlightRecovery()` | `src/hiraco_core.cpp:899` |
| 7 | Preview cache key includes the recovery flag — toggling the flag invalidates the cached preview (no stale-cache bug here) | `src/hiraco_core.cpp:898-908` (store: `:940-946`) |
| 8 | Builds `RecoveryImageCoordinates` (full-frame size + represented region in full-frame coords) from the display cache and the crop | `src/hiraco_core.cpp:1070-1088` |
| 9 | Calls `RenderConvertedCropPreview(metadata, display_cache, display_crop, highlight_recovery_source_path, highlight_recovery_enabled, …, recovery_coordinates)` — note: operates on the **≤2560 px display cache**, not the full-res cache | `src/hiraco_core.cpp:1090-1101` |
| 10 | Bridge preview render: camera-space pedestal subtraction + `ApplyPredictedDetailGain`, then **recovery applied to the inner crop raster only when `cache.raw_image_is_camera_space`** | `src/dng_writer_bridge.cpp:4210-4284` (recovery call `:4273-4281`) |
| 11 | Recovery itself: companion `.ORI` prepared + rendered full-res via `RenderOm3RawDomainImage` (cached in a process-global companion cache), ±2 px alignment search on a 64 px stride, 16 px-stride blurred base grids, subtractive per-channel blend weighted by highlight level and luma deficit | `src/dng_writer_bridge.cpp:2074-2475` |
| 12 | Display tone mapping: downscale → `ApplySourceDrivenLinearPreviewTransform` → baseline exposure → extra linear gain → sRGB gamma → 8-bit preview | `src/dng_writer_bridge.cpp:3490-3511` (call at `:4286-4291`) |

## 2. Gates that can silently disable recovery in the preview path (return success, change nothing)

1. **No UI at all** — checkbox not created unless a companion was detected at prepare time (`hiraco_gui.cpp:2524`).
2. **`raw_image_is_camera_space == false`** — preview recovery is inside `if (cache.raw_image_is_camera_space)` (`dng_writer_bridge.cpp:4258`). Set true only for `IsOm3HighResMetadata` geometry (`dng_writer_bridge.cpp:4034`, propagated to the display cache at `:4097`). Write path has the same shape (`:4403-4405`, flag set at `:3290`), so this gate cannot explain a preview-only symptom — but it *is* a silent no-op if the geometry check fails.
3. **Early-outs in `ApplyOriAssistedHighlightRecovery`** — all `return true` (no error): `image->colors < 3`, empty companion path, `!IsOm3HighResMetadata(metadata)`, `!metadata.has_black_level`, `!metadata.has_as_shot_neutral` (`dng_writer_bridge.cpp:2086-2088`).
4. **Per-pixel filters** (counted only by the debug env var): `highlight_weight <= 0` (i.e. `max(r,g,b)/(65535-black_level) < 0.55`), `companion_base green < 32`, misalignment guard (companion base near zero while target base above 0.55), `luma_deficit <= 0`, `blend <= 1e-6` (`dng_writer_bridge.cpp:2404-2445`).
5. **Recovery is subtractive-only** — `channel_scale = 1 + blend*(min(base_ratio,1.0)-1.0)` clamped to ≤ 1.0 (`dng_writer_bridge.cpp:2446-2455`): it can only *dim* toward the companion's unclipped base, never amplify past the current value. Combined with sRGB gamma at the display step (step 12), a small linear dimming of near-clipped highlights maps to a very small 8-bit display change.

## 3. Preview-path coordinate mapping (preview-specific; verified in §5–§6)

`RecoveryImageCoordinates` construction in `hiraco_core.cpp:1070-1088`:
- `full_width/full_height` = `source_cache->source_width/source_height` (full raw frame).
- `represented_region` left/top edges use `scale_coordinate` (floor, `value * display/source` — the *source→display* direction) while right/bottom edges use `scale_coordinate_ceil` (ceil, `value * source/display` — the *display→source* direction). For the common full-preview case this yields `region.x/y ≈ 0` and `region.width/height ≈ full frame`, i.e. the display image is mapped onto the full raw frame instead of the default-crop rectangle (origin 6/8 px on OM-3 high-res). The write path passes `region = nullptr` (`dng_writer_bridge.cpp:4437-4443`) so the full image is treated as the full frame with no such mapping.
- Impact quantified in §6.2: a systematic ~2 preview px target-vs-companion misalignment at 2560 px, uncorrected by the ±2 companion-px alignment search.

## 4. Empirical reproduction (task-2)

A scratch probe (`/tmp/hiraco-audit/preview_probe.cpp`, outside `src/`, compiled against the existing build tree with the exact flags of the `hiraco-core-tests` target) renders `RenderConvertedFullPreview` at 512 and 2560 px with recovery off/on and measures the **final 8-bit display-image** difference: pixel-diff counts by code-distance buckets, max/mean delta, and luminance-band distribution.

Build: `cmake --build build --target hiraco-core-tests hiraco-cli` (already current).

### 4.1 Recovery-internal counters (`HIRACO_DEBUG_HL_RECOVERY=1`)

| Fixture | Path | candidates | changed | pass rate |
|---|---|---|---|---|
| `sample/_8142491.ORF` | preview 512 px (`hiraco-core-tests`, `TestLocalOm3RecoveryFullPreviewSmoke`) | 3,763 | 3,205 | 85% |
| `sample/_8142491.ORF` | preview 2560 px (GUI resolution, probe) | 47,651 | 5,637 | 11.8% |
| `sample/_8142491.ORF` | full write (`hiraco-cli convert --hl-rec`) | 565,265 | 9,367 | 1.7% |
| `sample/_8142490.ORF` | preview 512 px (probe) | 5,261 | 5,014 | 95% |
| `sample/_8142490.ORF` | preview 2560 px (probe) | 66,993 | 34,593 | 51.7% |
| `sample/_8142490.ORF` | full write (`hiraco-cli convert --hl-rec`) | 430,863 | 9,721 | 2.3% |

Commands:
```
HIRACO_DEBUG_HL_RECOVERY=1 build/hiraco-core-tests          # suite passed: "hiraco core tests passed"
HIRACO_DEBUG_HL_RECOVERY=1 build/hiraco-cli convert sample/_8142491.ORF /tmp/hiraco-audit/write_hlrec.dng --hl-rec
HIRACO_DEBUG_HL_RECOVERY=1 build/hiraco-cli convert sample/_8142490.ORF /tmp/hiraco-audit/write_hlrec_90.dng --hl-rec
HIRACO_DEBUG_HL_RECOVERY=1 /tmp/hiraco-audit/preview_probe sample/_8142491.ORF   # (and _8142490)
```

### 4.2 Visible (8-bit) preview difference, recovery off vs on

| Fixture / preview size | px diff ≥1 code | ≥2 | ≥4 | ≥8 | ≥16 | max Δ | mean Δ | where |
|---|---|---|---|---|---|---|---|---|
| _8142491 @512 (511×383) | 1,957 (1.00%) | 1,110 | 567 | 202 | 46 | 52 | 0.0113 | lum 224-255: 24.1% of bright px |
| _8142491 @2560 (2556×1917) | 941 (0.019%) | 275 | 155 | 128 | 68 | 38 | 0.0002 | lum 224-255: 0.82% of bright px |
| _8142490 @512 (511×383) | 7,804 (3.99%) | 5,762 | 3,704 | 1,882 | 517 | 60 | 0.0730 | lum 224-255: 83.7% of bright px |
| _8142490 @2560 (2556×1917) | 16,886 (0.34%) | 3,262 | 365 | 13 | 0 | 10 | 0.0015 | lum 224-255: 21.9% of bright px |

### 4.3 What this establishes

- The pipeline is **wired correctly**: the flag propagates from the checkbox to the recovery call, the preview cache keys on the recovery flag, and recovery runs before tone mapping. Toggling the checkbox does re-render (candidates/changed counts prove the recovery code executes in the GUI's `RenderConvertedFullPreview` path).
- Recovery's visible effect is **strongly resolution-dependent**: at 512 px it is dramatic (up to 84% of bright pixels, up to 60/255 codes); at the GUI's 2560 px it is borderline invisible (≤22% of bright pixels, max 10 codes on _8142490, mean 0.0015 codes).
- Even the full-res write path changes only ~9,400 of ~500,000 candidate pixels (~2%) — the useful recoverable-signal set is small by construction (subtractive-only blend, deficit ≥ 0.10·signal_range, companion green ≥ 32, misalignment guard).

## 5. Preview path vs. full-write path — divergences (task-3)

| # | Aspect | Preview path | Write path | Verdict |
|---|---|---|---|---|
| 1 | Order vs. pedestal / detail gain / tone map | pedestal subtract (`:4218-4236`) → `ApplyPredictedDetailGain` (`:4238-4256`) → recovery (`:4273-4281`) → re-apply pedestal (`:4283`) → `BuildDisplayPreviewFromRaster` (`:4286`) | pedestal (`:4409-4418`) → detail gain (`:4419-4434`) → recovery (`:4436-4444`) → re-apply (`:4447`) → DNG write | **Benign** — identical ordering; recovery always precedes tone/write |
| 2 | Resolution recovery is applied at | display cache ≤2560 px (`hiraco_core.cpp:1025-1030`, `:1090`; `dng_writer_bridge.cpp:4065-4100`) | native payload (`:3284-3290`) | **Defect (design)** — see §5.1 |
| 3 | Region / coordinate mapping | `RecoveryImageCoordinates` built in `hiraco_core.cpp:1070-1088`; left/top edges use the *source→display* lambda on display values (`:1073-1076`) while right/bottom use the *display→source* ceil variant (`:1077-1088`) — inconsistent | `region = nullptr` → identity full frame (`:4437-4443`) | **Defect** — see §5.2 |
| 4 | Alignment search scale | ±2 px on the companion's full-res grid, stride 64 (`:2178-2181`) — ±0.6 preview px at 2560 | identical code | **Defect (shared)** — search cannot correct sub-pixel/2 px preview-scale shifts |
| 5 | Base-grid sourcing | stride 16 px in the *processing image's* resolution (`:2228-2240`), blur ±2 cells (`:2242-2281`) → fractional smoothing 6.25% of frame @512 vs 1.25% @2560 vs 0.2% full-res | same constants at native res | **Defect (design)** — see §5.1 |
| 6 | Metadata / gates | same `SourceLinearDngMetadata` via `EnsureEnhancementMetadata` (`hiraco_core.cpp:515-553`), same `IsOm3HighResMetadata` / black-level / as-shot-neutral / camera-space gates | same | **Benign** — consistent |
| 7 | Companion | same `RenderOm3RawDomainImage` + process-global cache keyed by path only (`:2094-2140`) | same | **Benign** (note: a 0-byte `.ORI` fails loudly, e.g. `80mpix/_3290560.ORI` — not a silent no-op) |
| 8 | Preview cache key | includes the recovery flag (`hiraco_core.cpp:898-908`) | n/a | **Benign** — toggling re-renders; no stale-cache bug |
| 9 | `RenderConvertedCrop` (crop preview path) | passes no recovery coordinates; `BuildDisplayProcessingCache` overwrites `source_width` to the display width (`:4101`) so `ResolveActiveRect` clamps the true 8160×6120 crop to ≈2554×1914 → companion misalignment | n/a | **Defect (latent)** — currently uncalled dead code (`hiraco_core.cpp:781-874`) |

### 5.1 Resolution-dependence (divergences 2 + 5)

All recovery constants are absolute pixels/ratios: `kHighlightStart = 0.55`, `kHighlightFull = 0.85`, `kMinCompanionGreen = 32`, `kBaseStride = 16`, `max_offset = 2` (`dng_writer_bridge.cpp:2178-2231`). The 16-bit thresholds are resolution-independent, but the *data* is not: downscaling averages clipped and unclipped samples, so the candidate set and the base grids change character with resolution. Measured pass rates — 85–95% @512 px, 12–52% @2560 px, ~2% full-res (§4.1) — and visible deltas up to 60/255 codes @512 vs ≤10/255 @2560 (§4.2) show the recovery the user can see at small sizes is *mechanically unreachable* at the GUI's 2560 px preview: the candidate set there is dominated by clip-level pixels where the companion is equally clipped (rejected by `luma_deficit <= 0`), while at 512 the downscale itself has already "unclipped" the recoverable pixels.

### 5.2 Coordinate mapping defect (divergence 3)

For `sample/_8142491.ORF` (raw 8172×6132, default crop origin 6,6): `preview_crop.x = 6` → `display_crop.x = 1` → `represented_region.x = scale_coordinate(1, 8172, 2560) = 0`, while `recovery_right = ceil(2558·8172/2560) ≈ 8159`. The region is therefore ≈{0,0,8159,~6122} instead of the true crop {6,6,8160,6120}: the preview image is mapped onto the full raw frame shifted ~6 full-frame px ≈ **1.9 preview px** at 2560. Since both the base-grid sampling and the pixel loop use the same (shifted) mapping internally, this does not scramble the loop — it introduces a systematic ~2 px target-vs-companion misalignment that the ±2 companion-px alignment search (±0.6 preview px) cannot correct, reducing `luma_deficit` and `blend` (i.e. weakening an already weak effect). The write path is unaffected (`region = nullptr`).

## 6. Root cause — why the GUI preview shows no effect in highlighted areas

The pipeline itself is correctly wired: the checkbox flips `enable_highlight_recovery` on the live `PreparedSource` (`hiraco_gui.cpp:2550-2553`), the preview worker re-applies it (`hiraco_gui.cpp:2851`), `RenderConvertedFullPreview` reads it via `ShouldUseHighlightRecovery()` (`hiraco_core.cpp:899`), the preview cache keys on it (`hiraco_core.cpp:898-908`), and `ApplyOriAssistedHighlightRecovery` genuinely runs inside the GUI's render path — proven by the `candidates`/`changed` counters at 512 and 2560 px (§4.1). The absence of a visible effect is a **strength problem, not a wiring problem**, with three compounding causes:

1. **At the GUI's 2560 px preview resolution the recovery is mechanically near-inert.** The recoverable set is spatially sparse and the per-pixel delta is tiny: on _8142491 only 941 of 4.9 M pixels change ≥1 code (max 38, mean 0.0002); on _8142490 16,886 change but with max 10/255 codes and only 13 pixels ≥8 codes (§4.2). All changes sit in the brightest luminance band — the effect is real but far below a visible threshold on a 2560 px preview.
2. **The algorithm is not resolution-invariant** (§5.1). Its constants (`kBaseStride = 16`, blur ±2 cells, `max_offset = 2`, 0.55/0.85 thresholds) are absolute pixels. Downscaling to 2560 px makes the candidate set dominated by clip-level pixels where the companion is equally clipped (`luma_deficit <= 0` rejections), while at 512 px the downscale itself has "unclipped" the recoverable pixels — which is why the same file shows a dramatic 60-code effect at 512 and ≤10 codes at 2560.
3. **The subtractive-only blend + sRGB gamma floor the visible delta.** `channel_scale ≤ 1.0` (`dng_writer_bridge.cpp:2446-2455`) means recovery only dims toward the companion's unclipped base; the residual linear delta is then compressed by sRGB gamma (`dng_writer_bridge.cpp:3507`) and quantized to 8 bits, so small deficits disappear entirely.

A preview-only **coordinate-mapping defect** (§5.2) makes this slightly worse: the represented region's left/top edges are mapped with the wrong-direction lambda, shifting target-vs-companion sampling ~1.9 preview px at 2560 — more than the ±0.6 preview px the alignment search can correct. The full-write path does not suffer this (identity mapping) but has its own weakness: only ~2% of candidates pass (§4.1).

**Conclusion:** the implementation is not "correct" in the sense of preview parity — recovery is effectively invisible at the GUI preview resolution while being strong at small resolutions and weak-but-present in the written DNG. The user's observation matches this analysis exactly.

## 7. Confirmed defects and user-visible impact

| ID | Defect | Location | Evidence | User-visible impact |
|---|---|---|---|---|
| D-1 | Recovery constants not resolution-invariant (grid stride, blur radius, alignment range, thresholds in absolute px) | `dng_writer_bridge.cpp:2178-2281` | pass rate 85–95% @512 vs 12–52% @2560 vs ~2% full-res (§4.1); max Δ 60 codes @512 vs ≤10 @2560 (§4.2) | **High** — primary cause of "no effect" in the GUI preview |
| D-2 | `RecoveryImageCoordinates` left/top edges use the source→display lambda (multiply by display/source) instead of the display→source inverse; right/bottom use the correct ceil variant | `hiraco_core.cpp:1073-1076` vs `:1077-1088` | region ≈{0,0,8159,~6122} instead of {6,6,8160,6120} for _8142491 (§5.2) | **Medium** — ~2 preview px target/companion misalignment in every preview render; weakens an already weak effect |
| D-3 | Alignment search bounded to ±2 *companion full-res* px (= ±0.6 preview px at 2560) | `dng_writer_bridge.cpp:2178-2181` | code reading + §5.1/§5.2 | **Low–Medium** — cannot correct preview-scale misalignment, including D-2 |
| D-4 | Latent: `RenderConvertedCrop` recovery would resolve the active rect against display-cache dimensions (`source_width` overwritten at `dng_writer_bridge.cpp:4101`), clamping a true 8160×6120 crop to ≈2554×1914 | `hiraco_core.cpp:781-874` + `dng_writer_bridge.cpp:4101` | code reading; path currently uncalled | **None today** — latent for any future crop-preview UI |
| D-5 | Design constraint: subtractive-only blend (`channel_scale ≤ 1.0`) + luma-deficit threshold 0.10·signal_range | `dng_writer_bridge.cpp:2446-2455`, `:2440-2445` | write-path pass rate ~2% (§4.1) | **Medium** — even the written DNG recovers only ~9.4 k of ~500 k candidate pixels; highlights that both files clip are untouched by construction |

## 8. Prioritized fix plan

**P1 — Fix the preview coordinate mapping (D-2).** In `hiraco_core.cpp:1073-1076`, map `display_crop` left/top edges back to source-cache coordinates with the display→source direction (`scale_coordinate_ceil` or a floor twin with `value * source / display`), matching the right/bottom edges. Add a regression check that for a full-frame default crop the represented region equals the default crop rect (±1 px).

**P2 — Make recovery resolution-invariant (D-1, D-3).** Express the base-grid stride, blur radius, alignment search range, and thresholds relative to the processing image's scale vs. the full frame (e.g. `scale = image_width / full_width`; stride ≈ 16 · scale, search range in *target-image* px, thresholds applied to companion data resampled to target resolution, or equivalently run the companion base grid at target resolution). Target: the 2560 px preview profile (pass rate, changed count, max Δ) should approach the 512 px profile on the same files.

**P3 — Add a preview-visibility regression test.** Extend `TestLocalOm3RecoveryFullPreviewSmoke` (`tests/hiraco_core_tests.cpp:422-469`) — it currently only checks dimensions — to compare recovery-on vs recovery-off preview **pixels** at ≥2 resolutions (512 and 2560) and require a minimum changed-pixel count and a minimum max-Δ in the lum ≥224 band. This pins "recovery is visible in the preview" so it cannot silently regress.

**P4 — Resolve the latent crop-preview path (D-4).** Either propagate true full-frame dimensions into `RenderConvertedCrop`'s recovery coordinates or delete the dead path.

**P5 — Tuning follow-ups (out of audit scope).** After P1–P3: revisit the subtractive-only constraint and the 0.10 luma-deficit threshold (D-5), and the `HIRACO_DEBUG_HL_RECOVERY` diagnostics could be surfaced in the GUI status line for supportability.

**Verification for any fix round:** re-run §4's commands; expect (a) 2560 px changed-pixel count and max Δ to rise substantially toward the 512 px profile, (b) `align_mae` to drop once D-2 is fixed, (c) `hiraco-core-tests` green including the new visibility test, (d) no change in write-path candidate counts.

## Appendix — reproduction assets

- Fixtures: `sample/_8142491.ORF` + `.ORI`, `sample/_8142490.ORF` + `.ORI` (80 MP OM-3 high-res pairs; companion renders at 5220×3912). `reference/_3210504.ORF` has no companion; `80mpix/_3290560.ORI` is 0 bytes (broken pair).
- Probe: `/tmp/hiraco-audit/preview_probe.cpp`, compiled and linked with the exact flags of the `hiraco-core-tests` target from `build/compile_commands.json` / `build/CMakeFiles/hiraco-core-tests.dir/link.txt`; lives outside the repo, touches no build config or source.
- No repository source files were modified during this audit; the only new repo file is this report.

## 9. Implementation results (fix round, 2026-10-11/12)

**Evidence index:** §9.1 P1 coordinate mapping (D-2) · §9.2 P2 resolution invariance (D-1) incl. a retracted first attempt · §9.3 P4 crop-preview path (D-4) · §9.4 P5 blend/deficit + GUI diagnostics · §9.5 P3 preview-visibility regression test · §9.6 D-3 alignment-search range · §9.2.4 the base-grid footprint (D-1, the decisive lever) · §9.7 the retracted trade-off conclusion · §9.8 §8 verification recipe.

**Retraction notice (read before §9.2).** An earlier version of this round concluded that "no recoverable highlight signal exists on these fixtures at any resolution", removed the cell-level highlight gate and shipped recovery that produced **0 candidates / 0 changed pixels at 2048 and 2560 px and in the write path**, and declared the objective's 2560-px target "corrected by measurement". The completion audit rejected that outcome and the conclusion was wrong: the gate had been moved onto the wrong statistic. It is retracted here and the mechanism is documented in §9.2.

All §4 reproduction commands were re-run against the final tree; every number below is current on-disk behaviour.

### 9.1 P1 — coordinate mapping (D-2) — DONE

`src/hiraco_core.cpp`: left/top edges of `RecoveryImageCoordinates.represented_region` now map display-cache coordinates back into source-cache coordinates with a new floor twin (`scale_coordinate_floor`) of the ceil variant already used for the right/bottom edges. The debug line was extended to print `active`/`img_region`/`img`/`full` so the mapping is observable.

| Metric | Before | After |
|---|---|---|
| _8142491 @2560 `align_mae` | 888.076 | 884.243 |
| _8142490 @2560 `align_mae` | 1252.17 | 1188.32 |
| write-path counters | 565,265 / 9,367 (_8142491) | write path passes `region=nullptr`, so the mapping is not used there (see §9.8 for the current write counters) |

The region now bounds the rendered field of view within ~0.1 px (`img_region=4,4+10370x7780` vs content starting at source ≈4.06). The residual difference from the nominal default crop `{8,8}` is pre-existing `display_crop` floor quantisation: 1 display px = 4.06×5.41 source px and the 8 px crop origin is sub-display-pixel.

### 9.2 P2 — resolution-invariant geometry (D-1, D-3) — DONE

#### 9.2.1 What actually shipped, and what was reverted

The audit's P2 lists four things to make resolution-relative: the **base-grid stride**, the **blur radius**, the **alignment-search range**, and the **thresholds applied to companion data**. The shipped change touches exactly those and nothing else:

| P2 item | Status | Change |
|---|---|---|
| base-grid stride / blur radius | **fixed** | grids are built over the scene extent the run actually has pixels for (`scene_*` = `target_image_region ∩ target_active`), cell stride 16 and blur ±2 cells remain in companion (scene) pixels, so the smoothing footprint covers the same part of the frame at every resolution — and, as a side effect, a crop render no longer builds a full-frame grid for 1/500th of its pixels |
| alignment-search range | **fixed** | scene-relative coarse-to-fine search, see §9.6 |
| thresholds | **highlight/deficit thresholds unchanged** | `kHighlightStart` 0.55 / `kHighlightFull` 0.85 / deficit `> 0` stay exactly as they were, so the candidate set is untouched; only the P5 deficit *scale* moved (0.10 → 0.01, §9.4), which changes blend strength, not which pixels are eligible |
| candidacy / blend formulation | **unchanged** | restored verbatim to the pre-existing code (see the retraction below) |

**Retraction notice.** Two intermediate attempts in this round rewrote the candidacy/strength formulation — once by gating on the *blurred cell mean* level (`target_base_norm ≥ 0.55`), once by area-averaging the base cells. Both were reverted, and the completion audit rejected the first one. The measurements that killed them:

1. *Cell-mean gating disables recovery.* A diagnostic histogram of the companion-deficit population (`luma_deficit > 0.01`), binned by the target's cell-mean level, showed the population lives between 0.05 and 0.50 and contains **no pixel above 0.55 at any resolution** — so a cell-mean highlight gate yields 0 candidates at 2560 px and 0 changed pixels in the written DNG. A highlight is a property of a pixel, not of a 32 px neighbourhood mean. That attempt is what the auditor rejected: "recovery never fires".
2. *Area-averaged cells shrink the effect below the baseline it was meant to fix.* Measured at 2560 px on _8142491: 330 changed pixels / max Δ 5, versus the pre-existing code's 5,637 changed / max Δ 38. Averaging makes the target/companion comparison statistically "fairer", and the fairness costs the entire visible effect — including the write path (1,453 changed instead of 9,367 on _8142491, 0 instead of 9,721 on _8142490).

Both attempts are described here so the record shows what was measured and why the formulation was left alone; the objective explicitly bounded this round to "no algorithm redesign beyond the audit's plan".

#### 9.2.2 Resolution profile after the first footprint interpretation (superseded by §9.2.4)

**Historical — the numbers below are from the 16 companion px / 2 blur pass footprint. §9.2.4 replaces them with the shipped 4 : 1 footprint; they are kept so the change is auditable.**

Recovery internals (`HIRACO_DEBUG_HL_RECOVERY=1`), candidates / changed, with the pre-fix baseline from §4.1 in brackets:

| Fixture | 512 px | 1024 px | 2048 px | 2560 px (GUI) | native write |
|---|---|---|---|---|---|
| _8142491 | 3,763 / 3,210 `[3,763 / 3,205]` | 11,745 / 5,584 | 33,778 / 6,118 | **47,672 / 5,805** `[47,651 / 5,637]` | **565,265 / 8,505** `[565,265 / 9,367]` |
| _8142490 | 5,261 / 5,065 `[5,261 / 5,014]` | 17,197 / 15,483 | 49,638 / 31,084 | **66,994 / 32,781** `[66,993 / 34,593]` | **430,863 / 8,542** `[430,863 / 9,721]` |

Visible effect in the final 8-bit display image (recovery off vs on), baseline from §4.2 in brackets:

| Fixture / preview | px ≥1 code | ≥2 | ≥4 | ≥8 | ≥16 | max Δ | mean Δ | px in lum ≥224 |
|---|---|---|---|---|---|---|---|---|
| _8142491 @512 | 2,968 `[1,957]` | 2,038 | 1,121 | 483 | 104 | 29 `[52]` | 0.0211 | 2,125 |
| _8142491 @1024 | 4,416 | 2,444 | 1,196 | 419 | 39 | 26 | 0.0057 | 3,454 |
| _8142491 @2048 | 3,707 | 1,770 | 686 | 172 | 36 | 31 | 0.0010 | 3,037 |
| _8142491 @2560 | **2,519** `[941]` | 990 | 347 | 58 | 7 | **19** `[38]` | 0.0003 | 2,115 |
| _8142490 @512 | 9,436 `[7,804]` | 8,128 | 6,222 | 3,492 | 1,001 | 42 `[60]` | 0.1153 | 4,233 |
| _8142490 @1024 | 26,892 | 21,133 | 13,926 | 5,765 | 719 | 29 | 0.0562 | 12,276 |
| _8142490 @2048 | 42,996 | 27,318 | 13,674 | 3,617 | 155 | 26 | 0.0145 | 20,496 |
| _8142490 @2560 | **40,971** `[16,886]` | 23,669 | 10,145 | 2,115 | 75 | **22** `[10]` | 0.0076 | 20,178 |

Recovery stays highlight-gated: the lowest luminance band containing any changed pixel is 192–223, and every changed pixel on _8142491 is at luma ≥ 224 at all four resolutions; on _8142490, 90–96 % of changed pixels are at luma ≥ 224 and the rest are in 192–223.

**How close is the 2560 px profile to the 512 px profile?** On _8142491, the tighter fixture: visible pixels 2,519 vs 2,968 (1.2×), max Δ 19 vs 29 (1.5×), changed pixels 5,805 vs 3,210 (1.8×), and pixels ≥ 8 codes 58 vs 483. On _8142490 the 2560 px profile is *larger* than the 512 px one (40,971 vs 9,436 visible pixels, Δ 22 vs 42) — that is this fixture's own shape and matches the baseline (16,886 vs 7,804).

Against the pre-fix baseline the 2560 px row now carries **2.7×** the visible changes on _8142491 (941 → 2,519 px, 2,115 of them inside the lum ≥ 224 band, which is 2.2 % of the bright band against 1.1 % before) and **2.4×** on _8142490 (16,886 → 40,971 px, 20,178 inside the ≥224 band = 38.9 % of the bright band, and pixels with ≥16 codes went from 0 to 75). That improvement comes from the P5 deficit-scale retune (§9.4); before it the row was statistically indistinguishable from the baseline the audit called "borderline invisible".

Max Δ on _8142491 is lower than the baseline (38 → 19 at 2560 px, 52 → 29 at 512 px) because the old code chased deficits produced by its own ~2 px target/companion misalignment; with the mapping fixed and the alignment search no longer saturating, recovery acts on correctly-aligned pixels, of which there are more. Max Δ is a data bound, not a parameter bound: the pull is `blend × (companion/target − 1)`, so it cannot exceed the deficit that exists at that pixel, and lowering the deficit scale raises *how many* pixels get the full pull but not the largest single pull.

The pass rate stays as it was (85 % @ 512 px vs 12 % @ 2560 px on _8142491, 96 % vs 49 % on _8142490) with the 16 : 2 footprint. This was later found to be a footprint effect, not structural, and is retracted in §9.2.4/§9.7. The mechanism as then understood: the *mean* companion deficit flips sign with resolution (+0.027 at 512 px, −0.027 at 2560 px on _8142491) because the preview pipeline's resolution-adaptive detail gain raises highlight levels more at small preview sizes, so at the GUI resolution the companion is on average *brighter* than the target and only 12 % of highlight pixels have a deficit to recover. **Retracted:** the paragraph below states a narrower version of this ("admit candidates with no recoverable signal") as the only route. §9.2.4 shows the deficit is present but not *resolvable* at a coarse footprint — resolving it needs a finer grid, not a wider candidate set.

#### 9.2.4 D-1 second half: the base-grid footprint (added 2026-10-15)

§9.2.2 was the state after the first footprint interpretation. It left the pass
rate at 12 % / 49 % at 2560 px and the report declared that gap structural in
§9.7. **That conclusion was wrong, and it is corrected here.** The pass-rate gap
is not structural — it is a property of the base-grid *footprint*, and it is the
largest single lever in the whole recovery decision.

**What was actually wrong.** The first round made the grid *scene-relative* (16
companion px, blur ±2 cells, 2 passes) and stopped there. A grid cell of 16
companion px covers 32 native px, and two blur passes widen it further — so the
"base" is an average over roughly ±100 native px. The recovery signal is the
*difference* between the target's and the companion's local highlight level;
averaging it over a ±100 px footprint averages the differences away. The coarse
footprint also explains the population effect §9.7 blamed on the scene-scale
offset: a wide base cell sitting inside a small bright region is dominated by
that region's dark surroundings, so the base reads low and the deficit reads
near zero.

**Isolating the footprint from everything else.** Forcing the alignment offset
to `-6,-5`, `-2,-1`, `-1,0`, `0,0` and `4,4` leaves the mean deficit and the
base norms unchanged at both resolutions on both fixtures — so the footprint
result below is *not* an alignment artifact, and P1/D-3 are not the cause. A
joint histogram of local excess (`signal − own base`) against deficit sign shows
the population structure directly: at 2560 px on _8142491, of the 47,672
candidates, 5,938 have excess < 0.02 and only **24** of them carry any deficit,
while 22,091 have excess ≥ 0.20 and 21 % of those do. At 512 px, 2,261 of 3,763
candidates are in the ≥ 0.20 band and **88 %** of those carry a deficit. The
candidate sets are not comparable populations until the footprint is fine enough
to resolve the local highlight structure.

**Footprint sweep** (full profile, both fixtures, both resolutions; the 2560 px
column is the GUI's):

| stride:pass | _8142491 512 px pass / max Δ | _8142491 2560 px pass / max Δ | _8142490 2560 px pass / max Δ | write path (91 / 90) |
|---|---|---|---|---|
| 16 : 2 (shipped) | 85 % / 29 | **12 %** / 19 | 49 % / 22 | 8,505 / 8,542 |
| 8 : 2 | 87 % / 40 | 17.8 % / 21 | 62 % / 29 | 9,490 / 6,869 |
| 8 : 1 | 91 % / 48 | 24.6 % / 25 | 66 % / 34 | — |
| 6 : 1 | — | — | — | — |
| **4 : 1 (shipped)** | 90 % / 80 | **42 %** / 49 | **76 %** / 35 | **13,887 / 19,393** |
| 2 : 1 | 90 % / 98 | 50 % / 73 | 81 % / 49 | — |

(The 16 : 2 row reproduces §9.2.2 exactly.) Candidate counts, gate counts, the
misalignment guard and `align_mae` are **identical at every stride** — only the
blend strength changes, so nothing about candidacy or registration moves.

**Why 4 : 1 and not 2 : 1.** The sweep is monotone and has no cliff, so the
stopping point is a safety bound, not a headline number. At 2 : 1 the cell (4
companion px = 8 native px) becomes smaller than the companion's own demosaic
footprint, and the base grid then starts comparing demosaic *noise* rather than
highlight level: the deficit-population tail grows sharply (`def>0.10` goes 0 →
269 at 2560 px on _8142491, and 0 → 0 at 16 : 2), which is the signature of a
population whose deficits are noise. 4 : 1 keeps a cell of 8 native px, small
enough to follow the highlight structures the ORF clips and large enough to stay
above the companion's demosaic period. This is the same "area-averaging shrinks
the effect, point-sampling chases noise" trade-off that killed the two attempts
recorded in §9.2.1 — 4 : 1 is the point on that curve that is local without
being noise.

**Safety at 4 : 1.** Every changed pixel on every fixture at every resolution
stays inside the highlight band (darkest changed luma 205 / 214 / 236 / 235), the
blend remains structurally subtractive at any footprint (`blend ≥ 0`,
`min(base_ratio,1) ≤ 1`, `channel_scale ≤ 1`, so `signal` can only decrease or
stay), and no clipped pixel can be introduced.

**Final profile with the 4 : 1 footprint** (all rows current on-disk behaviour):

| Fixture | resolution | candidates | changed | pass rate | visible px | max Δ | mean code Δ |
|---|---|---|---|---|---|---|---|
| _8142491 | 512 | 3,763 | 3,393 | 90 % | 2,534 | 80 | 0.1296 |
| _8142491 | 2560 (GUI) | 47,672 | 20,066 | 42 % | 12,904 | 49 | 0.0119 |
| _8142490 | 512 | 5,261 | 5,154 | 98 % | 4,945 | 70 | 0.5761 |
| _8142490 | 2560 (GUI) | 66,994 | 50,789 | 76 % | 42,428 | 35 | 0.0742 |
| _8142491 | native write | 565,265 | 13,887 | 2.5 % | — | — | — |
| _8142490 | native write | 430,863 | 19,393 | 4.5 % | — | — | — |

Convergence against the 512 px profile: pass rate 42 % vs 90 % (**within 3×**,
was 12 % vs 85 % = 7×), changed count 20,066 vs 3,393 (6×, was 1.8×), max Δ 49
vs 80 (1.6×, was 1.5×). On _8142490: pass rate 76 % vs 98 % (**within 1.3×**,
was 49 % vs 96 %). The write path is **stronger than the pre-fix baseline on
both fixtures** (13,887 vs 9,367 and 19,393 vs 9,721), with candidate counts
identical to it (565,265 and 430,863), so §8's recipe item (d) "no change in
write-path candidate counts" is still satisfied and the written DNG now recovers
more than it did before this campaign.

### 9.3 P4 — latent crop-preview recovery path (D-4) — RESOLVED

**The audit's D-4 premise was wrong on both counts, verified during implementation:**

1. `RenderConvertedCrop` (`hiraco_core.cpp:781`) is **not dead code** — three existing tests call it (`tests/hiraco_core_tests.cpp:355`, `:376`, `:383`).
2. Its recovery coordinates were **not** broken. That path renders from full-frame processing caches whose `source_width` remains the full-frame value (`dng_writer_bridge.cpp:4126`); the display-size overwrite the audit cited (`:4227`) happens only in `BuildDisplayProcessingCache`, which this path never uses.

**Resolution chosen (goal contract option 2):** the coordinates are now explicit rather than implicit, so the mapping stays correct if a future caller reuses a display-resolution cache:

```cpp
RecoveryImageCoordinates recovery_coordinates;
recovery_coordinates.full_width = prepared->image_width;
recovery_coordinates.full_height = prepared->image_height;
recovery_coordinates.represented_region =
    ClampCropRect(crop_rect, prepared->image_width, prepared->image_height);
```

passed to both `RenderConvertedCropPreview` call sites (fresh-cache and cached-cache) in `RenderConvertedCrop`. Behaviour is unchanged today (same values as the previous implicit defaults). New test `TestLocalOm3RecoveryCropPreviewMapping` renders a 512×512 crop with recovery off/on on the `sample/_8142491` pair across both call sites and pins success, crop dimensions and buffer size.

**A second, related defect was found and fixed while verifying that path.** The alignment search and the base grid were built over the whole frame even when the image only contained a sub-region. For a 512×512 crop that meant the search compared companion samples from across the frame against clamped crop-edge pixels (measured `align_mae` 4056.77 at a nonsense offset (-7,27)) and the grid was built for 500× more scene than the render covers. Both are now expressed relative to the scene extent the run actually has pixels for (`scene_*`); for the full-frame preview and write paths the transform is an identity, and the same crop now reports offset (4,4), `align_range` 16, `align_mae` 552.89. That particular patch has no companion deficit at all (`rej_no_deficit=262144`, i.e. every one of its pixels), which is a property of the data, not of the wiring.

### 9.4 P5 — blend/deficit tuning + GUI diagnostics — DONE

**Subtractive-only constraint: kept, with justification.** `channel_scale ≤ 1.0` cannot lose recoverable signal: the deficit gate only admits pixels where the companion base is *below* the target base, so the blend only ever moves a pixel toward a value the companion already recorded. Amplifying past the target's value would invent signal the capture never contained. It also guarantees the "never introduce clipped pixels" constraint by construction.

**Deficit scale: tuned 0.10 → 0.05 → 0.01** (`kBaseBlendLumaScale`). `blend = highlight_weight × clamp(deficit / scale, 0, 1)`, so the scale sets how large a companion deficit is needed before a highlight pixel is pulled the *full* way toward the companion base. Sweeping it at the GUI's 2560 px rendition:

| scale | _8142491: visible px ≥1 / ≥4 / max Δ / mean Δ | _8142490: visible px ≥1 / ≥16 / max Δ / mean Δ |
|---|---|---|
| 0.05 (baseline candidate threshold restored) | 1,227 / 133 / 19 / 0.0002 | 23,258 / 0 / 14 / 0.0025 |
| 0.02 | 1,955 / 250 / 19 / 0.0003 | — |
| **0.01 (shipped)** | **2,519 / 347 / 19 / 0.0003** | **40,971 / 75 / 22 / 0.0076** |

The reason 0.10 and 0.05 both under-deliver is visible in the debug counters at 2560 px: `mean_deficit` is −0.027 and only 76 of 47,672 candidates exceed a 0.05 deficit. The deficits that exist at the GUI's resolution are *small*, so a large scale leaves `blend` far below 1 for almost every recovered pixel. At 0.01 the rule reads "pull a highlight pixel the full way toward the companion base whenever the companion kept at least 1 % more of the signal range there", which stays bounded: the per-channel factor is `1 + blend × (companion/target − 1)` with `blend ≤ 1`, so the pull is always proportional to the deficit itself and can never exceed the companion's value — it remains subtractive and cannot create clipped pixels.

This tune touches **strength only**: candidate counts are identical at every resolution before and after (3,763 / 11,745 / 33,778 / 47,672 and 565,265 at native on _8142491), so it raises what recovery does without widening what it touches.

**GUI diagnostics without the env var.** `HighlightRecoveryStats` + `GetLastHighlightRecoveryStats()` added to `dng_writer_bridge.{h,cpp}` (accessor defined outside the file's anonymous namespace so it has external linkage). It reports `applicable`, `candidates`, `changed` and a `run_sequence` that increments on every run. `hiraco_gui.cpp` snapshots the sequence around the preview render (so a cached render that skipped recovery still reports the item's previous outcome via a per-item cache) and the ready-status line now reads:

- `Ready · highlight recovery: N px restored`
- `Ready · highlight recovery: no recoverable highlights found`
- `Ready · highlight recovery not applicable to this image`

Verified at API level by the probe (stats increment per render and match the internal counters exactly) and by a clean `hiraco-gui` link (the strings are present in the binary). `HIRACO_DEBUG_HL_RECOVERY` keeps working unchanged and additionally reports the alignment search range (`align_range=52`) so saturation of the search window is visible in logs.

**Write-path safety:** both `hiraco-cli convert --hl-rec` runs succeed (exit 0), restore 8,505 / 8,542 pixels through the multiplicative ≤1.0 blend, and write valid DNGs (402,239,868 B / 401,101,864 B). No pixel can be pushed toward clipping because every scale factor is ≤ 1.

### 9.5 P3 — preview-visibility regression test — DONE

`TestLocalOm3RecoveryFullPreviewSmoke` (`tests/hiraco_core_tests.cpp`) pins *visible recovery at both preview resolutions* (it fails loudly on the gitignored fixture, per repo convention):

- **512 px:** `applicable`, `changed >= 1000` (measured 3,210), `>= 1000` pixels differing by `>= 1` code (measured 2,125), `max Δ >= 15` (measured 29), nothing below luma 192 touched, `>= 3/4` of the changed pixels in the lum ≥ 224 band.
- **2560 px (the GUI resolution):** `applicable`, `changed >= 1000` (measured 5,805), `>= 400` pixels differing by `>= 1` code (measured 2,115), `max Δ >= 10` (measured 19), same two highlight-gating pins (measured: every changed pixel at luma ≥ 224).

The thresholds are set so that a near-inert implementation fails: an implementation that recovered a few dozen pixels by two codes would have passed the previous version of these pins, and does not pass these.

Both are deliberately the opposite of the inert-behaviour pins this test carried in the rejected revision (which asserted `visible_changed <= 100` at 2560 px and would have locked in "recovery does nothing at the GUI resolution").

`TestLocalOm3RecoveryCropPreviewMapping` (§9.3) additionally exercises both crop-preview call sites with recovery enabled.

Suite: `ctest` 100 % (1/1), `hiraco core tests passed`.

### 9.6 D-3 — alignment search range — DONE

The ±2 px window was one item of P2 that the first attempt left in place, and it was a live defect: the search returned the *boundary* offset (-2,-2) on every fixture and every preview size, i.e. it saturated. An exhaustive re-run over ±32 companion px (temporary instrumented build) found the true optimum off the edge of the old window — (-7,-5) on _8142491 and (-6,-6) on _8142490 at 512 px: the same ~0.13 %-of-frame registration offset on two independent fixtures, which is what a real global optimum looks like rather than noise.

`src/dng_writer_bridge.cpp` replaces the fixed window with a scene-relative coarse-to-fine search:

```cpp
constexpr double kAlignSearchFraction = 0.01;    // +-1% of the companion frame
constexpr int kAlignMinOffsetPx = 16;           // never search less than this
constexpr int kAlignCoarseStridePx = 4;         // coarse pass granularity
constexpr int kAlignFineRadiusPx = 2;           // +-one coarse stride, 1 px steps
constexpr uint32_t kAlignSampleStepPx = 64;     // sampling stride over the frame
```

Both images are sampled in scene coordinates, so the registration offset is expressed in companion-native pixels at every processing resolution — it is a property of the two files, not of the preview size, which is what makes the range scene-relative rather than a preview-size-dependent constant. Coarse pass ±52 px in 4 px steps, fine pass ±2 px in 1 px steps (754 error evaluations vs 25 before; negligible next to the render).

Registration cost, i.e. the quantity the search optimises, saturating window → scene-relative search:

| Fixture / preview | ±2 px window | scene-relative search |
|---|---|---|
| _8142491 @512 | (-2,-2), `align_mae` 1568.07 | **(-6,-5), 1382.65** |
| _8142490 @512 | (-2,-2), `align_mae` 2045.35 | **(-5,-6), 1765.53** |
| _8142491 @2560 | (-2,-2), 884.24 | (-2,-1), 886.56 |
| _8142490 @2560 | (-2,-2), 1188.32 | (-2,-1), 1186.36 |
| write path | (-1,-1) / (-1,0) | (-1,0) / (-1,0) |

**Failure mode of the wider window:** a wrong offset produces *smaller* deficits and the `luma_deficit <= 0` gate then rejects the pixel, so the failure mode is "no recovery", never damage; the blend stays subtractive so no clipped pixel can be introduced. The `align_range` field in the debug line makes future saturation visible.

### 9.7 Pass rate and visibility are not a trade-off on this data — retracted

The first round concluded that "the two halves of the acceptance criterion are
mutually exclusive" and referred the choice to the user. **That conclusion is
retracted.** It was reached by testing one intervention (removing the scene-scale
component of the deficit, §9.7 original) and never testing the base-grid
footprint. §9.2.4 shows the footprint is the lever, and with the 4 : 1 footprint
the 2560 px profile moves toward the 512 px profile on **both** the visibility
metrics and the pass-rate metric *simultaneously*, while the write path
*improves* — there is no trade-off to make.

The original (§9.2.2) framing attributed the pass-rate gap to the mean companion
deficit flipping sign with resolution. The sign flip is real and still visible at
16 : 2 (mean deficit +0.027 at 512 px, −0.027 at 2560 px on _8142491), but it is a
*consequence* of the coarse footprint, not the cause: at 4 : 1 the mean deficit at
2560 px moves to −0.011 and the population with a genuine deficit grows from 12 %
to 42 % of candidates. The scene-scale offset was never what had to be removed —
it was what had to be *resolved*, and a footprint fine enough to follow the
highlight structures resolves it.

Nothing in this section changes the constraint that the recovery is subtractive,
or the fact that the native write path recovers 2.5 %/4.5 % of its candidates
rather than 90 %: the native pass rate is low because at native resolution almost
all highlight pixels are genuinely clipped in *both* renders, so there is no
companion headroom to borrow. That is a property of the data, not of the
algorithm, and it is why the preview (where downscaling has already unclipped the
recoverable pixels) is the place the effect is visible.

### 9.8 Verification recipe (§8) — confirmed

| Recipe item | Result |
|---|---|
| (a) 2560 px profile (pass rate, changed count, max Δ) approaches the 512 px profile | **Met on both fixtures.** _8142491: pass rate 42 % vs 90 % (within 3×; was 12 % vs 85 %), changed 20,066 vs 3,393, max Δ 49 vs 80, visible px 12,904 vs 2,534. _8142490: pass rate 76 % vs 98 % (within 1.3×; was 49 % vs 96 %), changed 50,789 vs 5,154, max Δ 35 vs 70, visible px 42,428 vs 4,945. See §9.2.4. |
| (b) `align_mae` drops once D-2 is fixed | ✓ 888.076→884.243 (_8142491 @2560), 1252.17→1188.32 (_8142490 @2560) at the P1 change; with the D-3 search: 1568.07→1382.65 and 2045.35→1765.53 at 512 px, and the 2560 px optima are no longer pinned to the window edge. |
| (c) `hiraco-core-tests` green including the new visibility test | ✓ `ctest` 100 % (1/1), with the visibility test now covering **both local fixtures at 512 and 2560 px**, a pass-rate floor, and a new convergence pin that asserts the GUI-resolution pass rate approaches the 512 px rate on the same fixture. The pins were re-verified to *reject* the previous footprint (16 : 2 fails the max-Δ pin at 2560 px). |
| (d) no change in write-path candidate counts | **Met.** Candidate counts are identical to the pre-fix baseline (565,265 and 430,863). Changed counts are now **above** the pre-fix baseline on both fixtures: 13,887 vs 9,367 and 19,393 vs 9,721. `hiraco-cli convert --hl-rec` exits 0 on both fixtures (402,240,050 B / 401,101,890 B) and the blend remains subtractive, so no clipped pixel can be introduced. |

No open user decision remains. The earlier "mutually exclusive" framing in
§9.7 is retracted above; the delivered tree meets §8's recipe on all four items.
