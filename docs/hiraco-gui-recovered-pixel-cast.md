# GUI: show the pixels ORI highlight recovery restored

Status: implemented (2026-10-15). See §5 for measurements.

## 1. What the user asked for

> I would like to see pixels recovered with ORI in the preview as pink or green
> cast. There should be checkbox that enables this feature in the same bar as
> compare and zoom options. this should work in all compare/zoom options (apart
> from original where we show straigh ORF preview without any processing)

## 2. Design decisions taken with the user

| Decision | Answer |
|---|---|
| Cast colour | **Pink / magenta**, applied as a solid replace (255, 0, 255) |
| Blend with the pixel's own value | **No** — solid tint, ignoring the underlying value |
| Toggle latency | **Instant** (no re-render when switching the checkbox) |
| How the mask reaches the GUI | **Extend `PreviewImage` with an optional `recovered_mask`** (one byte per pixel) |

## 3. How it works

| Step | Where | Notes |
|---|---|---|
| Recovery records which pixels it changed | `src/dng_writer_bridge.cpp` → `ApplyOriAssistedHighlightRecovery` | New optional `std::vector<uint8_t>* recovered_mask` out-param. It is resized to exactly `width*height`, zeroed, and marked with **the same test that increments `changed_pixels`**, so the mask and the changed counter cannot drift apart. |
| Mask is attached to the preview | `RenderConvertedCropPreview` → `preview->recovered_mask` | Attached **only when the dimensions still agree** between the crop raster the mask was built in and the returned preview, so a caller can never be handed a mask in a different index space from the pixels it is drawn over. |
| Mask travels with the preview | `PreviewImage::recovered_mask` (`src/hiraco_types.h`) | Default-constructs empty, so every existing caller and every cached preview is byte-identical. The converted-preview cache stores a whole `PreviewImage`, so the cache, the cache-hit path and both GUI panes all see the mask with no extra plumbing. |
| Cast is applied | `ApplyRecoveredPixelCast(PreviewImage*)` in `hiraco-core` | Pure data work: masked pixels become solid pink, unmasked pixels are left **byte-identical**. Refuses a null / colourless / size-mismatched mask rather than bounding-violating. |
| Cast is drawn | `src/hiraco_gui.cpp` → `CompareCanvas::OnPaint` | Draws the cast bitmap for the **converted pane only**. The placement rule is a pure function, `ResolveRecoveredPixelCastPlacement`, so it is unit-testable from `hiraco-core-tests` (the GUI is a separate target and is not linked into the tests). |
| Toggle | `show_recovered_pixels_checkbox_` in the preview toolbar | Sits in the same bar as **Compare** and **Zoom**, to the left of the zoom group. Per-selection state lives on `QueueItem::show_recovered_pixels`, so switching rows restores that image's choice. |
| No re-render on toggle | `CompareCanvas::CastBitmap()` | The overlay bitmap is built lazily from a **copy** of the preview and cached; a new preview invalidates it. Toggling only changes which bitmap `OnPaint` picks and calls `Refresh()`. |

### 3.1 Where the cast appears

`ResolveRecoveredPixelCastPlacement(show, has_mask, converted_visible, _)` returns
`on_converted = show && has_mask && converted_visible` and always
`on_original = false`.

| Compare mode | Converted pane | Original pane |
|---|---|---|
| **Original** | not drawn | **clean** (recovery never ran there) |
| **Converted** | **cast** | not drawn |
| **Side by side** | **cast** (right pane) | **clean** (left pane) |

Zoom cannot change this: the bitmap is chosen *before* the pane scaling in
`DrawBitmap`, so the cast scales with the image at **Fit**, **50%** and **100%**.
(Side-by-side is pinned to 100% by the canvas itself.)

### 3.2 Recovery behaviour is unchanged

The recovery pass itself was not modified in this round: same candidacy, same
blend, same subtractive clamp, same alignment search, same base-grid footprint
as the previous round. The only change is that it *additionally* reports which
pixels it changed, and the write path passes `nullptr` (the written DNG is
byte-for-byte what it was). The cast is a display-layer overlay, applied to a
copy, never to the raster that is written.

## 4. Regression tests (`tests/hiraco_core_tests.cpp`)

| Test | Pins |
|---|---|
| `TestApplyRecoveredPixelCast` | Solid pink at masked pixels; **byte-identical** unmasked pixels; all 5 mask shapes (all / none / single / corners / empty); empty mask and size-mismatched mask both refused with 0 changes and untouched pixels; null and colourless previews refused; the mask is never resized or mutated. |
| `TestRecoveredPixelCastPlacement` | All 48 combinations of show × mask × converted-visible × original-visible × 3 zoom levels. Cast on the converted pane only; **never** on the original pane. |
| `TestLocalOm3RecoveryPreviewCastCoverage` | End-to-end on both local pairs: the mask is sized to the preview, its popcount equals `changed`, the cast recolors exactly those pixels, unmasked pixels stay byte-identical, and the cast is concentrated in the highlight band (see §5). |
| `TestLocalOm3RecoveryFullPreviewSmoke` (extended) | Mask invariants at both resolutions on both fixtures: size `== w*h`, values only 0/1, popcount `== stats.changed`, and **empty** when recovery is off (so "recovery ran and changed nothing" is distinct from "there is no mask"). |

Both masks were re-verified to be **discriminating**: a build that marks every
pixel fails `the recovered-pixel mask must mark exactly the pixels the blend
changed (mask=195713 changed=3393)`, and the old 16:2 footprint still fails the
max-Δ pin.

The new tests follow the repo convention of **failing loudly on a missing
gitignored fixture** (`missing test fixture (gitignored *.ORF): …`) rather than
passing vacuously.

## 5. Measurements (current tree)

Local fixtures: `sample/_8142491.ORF` + `.ORI`, `sample/_8142490.ORF` + `.ORI`
(80 MP OM-3 high-res pairs; companion renders at 5220x3912).

### 5.1 Cast coverage on the local pairs

| Fixture | resolution | masked pixels (== `changed`) | in lum ≥ 224 | in lum ≥ 192 |
|---|---|---|---|---|
| `_8142491` | 512 px | 3,393 | 3,390 (**99.9 %**) | 3,391 |
| `_8142490` | 512 px | 5,154 | 3,702 (**71.8 %**) | 5,150 |

The rest sit in the 192–223 band. The mask is a *raster-level* mask: a few
pixels whose 16-bit change rounds away in the 8-bit preview are masked but not
visible, which is why the mask population is slightly wider than the visible
population. Visibility of the *displayed* change is pinned separately by
`TestLocalOm3RecoveryFullPreviewSmoke`.

### 5.2 What the user sees

`Ready · highlight recovery: N px restored` in the status line still reports the
recovery outcome (unchanged by this round); the checkbox additionally marks
those same pixels pink on the converted preview.

## 6. Build and verification

| Check | Result |
|---|---|
| `cmake --build build --target hiraco-core-tests hiraco-cli hiraco-gui` | clean (no warnings) |
| `ctest` from `build/` | **100 %** (1/1) |
| `HIRACO_DEBUG_HL_RECOVERY=1 ./build/hiraco-core-tests` | suite passed; 7 recovery runs printed, so both fixtures × both resolutions are exercised, not skipped |
| Cast string in binary | `Show recovered pixels` present in `build/hiraco-gui` |
| Write path unchanged | `hiraco-cli convert --hl-rec` on both fixtures reproduces the previous round's numbers exactly — 565,265 candidates / 13,887 changed and 430,863 / 19,393, exit 0, same DNG sizes (402,240,050 B / 401,101,890 B) |

## 7. Files touched

`src/dng_writer_bridge.{h,cpp}` (mask out-param, cast helper, placement rule),
`src/hiraco_types.h` (`PreviewImage::recovered_mask`),
`src/hiraco_gui.cpp` (toolbar checkbox, overlay bitmap, draw placement,
per-selection state), `tests/hiraco_core_tests.cpp` (4 tests).
No build-system, preset, DNG-SDK or Halide-generator changes.
