# Hiraco Improvement Backlog

Prioritized improvement candidates derived from the audit in
[`project-review.md`](project-review.md). Written 2026-10-09 against commit
`d6f5e2f`; this file is a snapshot of the backlog at that point.

Status note (2026-10-25): several candidates are now implemented and should be
read as done rather than open tasks:

- I-01 (deflate compression) — the DNG writer now honours deflate through the
  bundled SDK patch.
- I-13 (HL Rec in the CLI) — `hiraco-cli convert ... --hl-rec`.
- The browser UI and its WebAssembly build/deploy pipeline have shipped
  (live site: https://kred.github.io/hiraco/).

The remaining entries still need individual verification against the current
tree before they are picked up.

Each candidate carries: **evidence** (file:line or measured result, with the
finding ID from the review where applicable), **benefit**, **risk**, **effort**,
**dependencies**, and a **class**.

Effort scale: **S** < 1 hour · **M** 1–4 hours · **L** > 4 hours.

---

## 1. Summary Table

| ID | Title | Class | Priority | Effort |
| --- | --- | --- | --- | --- |
| I-01 | Make `--compression deflate` actually write deflate | Correctness | **P1** | M |
| I-02 | Instrument and report progress for JPEG-XL compression | Performance / UX | **P1** | S |
| I-03 | Make the requested DNG version match the written DNG version | Metadata | P2 | S |
| I-04 | Resolve the 20 MP enhancement gap (implement or document) | Algorithm / coverage | P2 | M |
| I-05 | Complete `algorithm.md` §11.5 item 5 — benchmark + regression tests | Robustness | P2 | M |
| I-06 | Remove dead guidance-map upsample code | Maintainability | P3 | S |
| I-07 | Drop the discarded highlight-recovery parameters | Maintainability | P3 | S |
| I-08 | Reduce `Load mosaic and CFA` cost | Performance | P3 | M |
| I-09 | Avoid materializing the full-resolution guide buffer | Memory / performance | P3 | M |
| I-10 | Validate presets against 20 / 50 / 80 MP sources | Quality | P4 | L |
| I-11 | Add sub-pixel registration to ORI highlight recovery | Algorithm / quality | P4 | L |
| I-12 | Validate companion identity, not just the filename | Robustness | P4 | M |
| I-13 | Expose `HL Rec` in the CLI | Feature parity | P5 | S |
| I-14 | Add a reproducible GUI benchmark mode | Performance | P5 | M |

---

## 2. Recommended Top-3 For The Next Goal

**I-01, I-02, I-05** — in that order.

Rationale:

- **I-01** is a correctness defect with a user-visible symptom (the label lies
  about the file) and a bounded, well-understood fix. It is the highest
  value-per-hour item on this list.
- **I-02** is the cheapest item here and removes the largest blind spot in the
  current observability story. Right now a 2-minute JPEG-XL conversion looks
  identical to a hung process.
- **I-05** is not a code change but a safety net. The preview path
  (`RenderConvertedFullPreview` and friends) is the most intricate orchestration
  in the core and currently has no orientation, cancellation, or cache-reuse
  coverage; landing I-01 and I-02 without it makes regressions likely.

**Alternative third pick.** If the user cares more about output quality than
about regression safety, take **I-04** instead of I-05 — it decides whether
standard 20 MP frames should be enhanced at all, which changes what Hiraco
means for the common case.

---

## 3. Candidates

### I-01 — Make `--compression deflate` actually write deflate

- **Class:** Correctness
- **Priority:** P1
- **Effort:** M
- **Dependencies:** none

**Evidence.** Finding F2 in `project-review.md`.

- `ConfigureHost` (`src/dng_writer_bridge.cpp:1810-1814`) sets only the DNG
  version, the linear-DNG flag, and the JXL flags. It never calls
  `host.SetCompression(...)`.
- The DNG SDK chooses the compression code itself:
  `dng_sdk_1_7_1/dng_sdk/source/dng_image_writer.cpp:7640-7658` selects
  `ccJPEG` (7) for 16-bit `ttShort` images and reserves `ccDeflate` (8) for
  floating-point and 32-bit-integer images.
- SDK constants: `ccJPEG = 7`, `ccDeflate = 8`
  (`dng_sdk_1_7_1/dng_sdk/source/dng_tag_values.h:163-164`).
- **Executed:** `exiftool -Compression -n` on the `deflate` output of
  `sample/_8142490.ORF` reports `7`. The `uncompressed` output reports `1` and
  the `jpeg-xl` output reports `52546`, both correct.

**Expected benefit.** The output metadata matches what the user selected;
downstream software stops misreading the file as lossless JPEG; file sizes
should drop further, since deflate typically beats lossless JPEG on linear
sensor data.

**Risk.** Medium. The SDK's writer may not honour a caller-requested compression
for `ttShort` payloads through the normal `WriteDNG` path. The fix may require
writing compressed strip tiles explicitly rather than flipping a host flag.
Verify against the SDK source before committing to an approach, and confirm the
result still reads correctly in a second tool (not only exiftool).

**Verification.** Convert the same source with all three modes; assert
`exiftool -Compression -n` returns `1`, `8`, and `52546` respectively; confirm
the DNG still opens and the pixel payload round-trips.

---

### I-02 — Instrument and report progress for JPEG-XL compression

- **Class:** Performance / UX
- **Priority:** P1
- **Effort:** S
- **Dependencies:** none

**Evidence.** Finding F1 in `project-review.md`.

- **Executed:** jpeg-xl conversion of `sample/_8142490.ORF` took
  **112.87 s** real / **112,799.7 ms** in `WriteLinearDngFromRaw`, but the timed
  `Write DNG stream` block accounted for only **546.8 ms**. The missing
  **~112.2 s** is `negative->LosslessCompressJXL(host, writer, false)` at
  `src/dng_writer_bridge.cpp:4528`.
- That call sits **outside** the `ScopedTimingLog` block that begins at `:4533`.
- `ReportProgress(progress, "convert", 0.92, "Writing DNG")` fires at `:4531`,
  immediately before the unmeasured call, so progress sits at 0.92 for roughly
  two minutes.

**Expected benefit.** The timing logs stop under-reporting jpeg-xl by ~200×,
and the CLI and GUI stop appearing hung during the longest phase of the longest
mode. This is also the prerequisite for any future jpeg-xl tuning work — right
now there is no measurement to tune against.

**Risk.** Low. Wrapping the call and subdividing progress is additive. The only
caution is that `LosslessCompressJXL` may not report internal progress, so the
progress bar may still move coarsely; a honest "compressing…" message plus a
timing log is the realistic floor.

**Verification.** Re-run the jpeg-xl conversion with `--debug`; the sum of the
timed phases should now equal the total within a small margin, and progress
should advance during compression rather than resting at 0.92.

---

### I-03 — Make the requested DNG version match the written DNG version

- **Class:** Metadata
- **Priority:** P2
- **Effort:** S
- **Dependencies:** none

**Evidence.** Finding F3 in `project-review.md`.

- `DngVersionForCompression` (`src/dng_writer_bridge.cpp:1803-1808`) returns
  `dngVersion_1_6_0_0` for every non-JXL mode and `dngVersion_SaveDefault` for
  jpeg-xl.
- **Executed:** `exiftool -DNGVersion -n` reports `1 4 0 0` for both the
  `uncompressed` and `deflate` outputs, and `1 7 0 0` for `jpeg-xl`. The
  requested `0x01060000` never reaches the file.
- The SDK clamps the saved version to the minimum required by the features
  actually used (`dng_sdk_1_7_1/dng_sdk/source/dng_image_writer.cpp`).

**Expected benefit.** Predictable, self-consistent output. Either the code
gets the 1.6.0.0 it asks for (by using a feature that requires it) or the
constant is corrected to match reality. Today the code asserts an intent the
output contradicts, which will confuse anyone debugging a downstream
compatibility report.

**Risk.** Low for the documentation-style fix; medium if the goal is actually
to emit 1.6.0.0, since that requires understanding which SDK features gate the
version.

**Verification.** Convert in each mode and assert the written `DNGVersion`
matches the value the code requested, or update
`DngVersionForCompression` and the `dng_sdk_bridge.cpp` status strings to
match observed behaviour.

---

### I-04 — Resolve the 20 MP enhancement gap

- **Class:** Algorithm / coverage
- **Priority:** P2
- **Effort:** M
- **Dependencies:** none (but see I-10 for the quality-validation side)

**Evidence.** Finding F4 and drift D2 in `project-review.md`.

- `BuildMetadataFromLibRaw` sets `has_predicted_detail_gain = true` and
  `predicted_detail_gain = 1.8` **unconditionally**
  (`src/hiraco_core.cpp:359-360`).
- `ShouldApplyPredictedDetailGain` (`src/dng_writer_bridge.cpp:924-930`)
  additionally requires `IsOm3HighResMetadata(metadata)`, which is true only for
  the 50 MP and 80 MP high-res crops (`:78-90`).
- **Executed:** converting `reference/_3060001.ORF` (5220×3912, standard 20 MP
  frame) logged `Apply predicted detail gain: 0.0 ms` — the stage was skipped
  entirely.
- `algorithm.md` §7 presents the enhancement pipeline without stating this
  geometry gate, so the behaviour is undocumented as well as surprising.

**Expected benefit.** Either standard frames get detail recovery (a visible
quality improvement on the most common input), or the limitation is documented
and the gating is made explicit in the metadata so the GUI can explain why a
file was not enhanced.

**Risk.** Medium. Enabling enhancement for 20 MP frames changes output for
existing users and increases conversion time on the path that is currently the
fastest. The documented-and-explicit route is the low-risk option.

**Verification.** Convert a 20 MP source before and after; assert the
enhancement stages now run (or that the GUI surfaces a clear "not applicable"
state); measure the time delta.

---

### I-05 — Complete `algorithm.md` §11.5 item 5

- **Class:** Robustness
- **Priority:** P2
- **Effort:** M
- **Dependencies:** none

**Evidence.** Drift D4 in `project-review.md`.

- `algorithm.md` §11.5 item 5 requires a full-preview refresh benchmark on the
  M4 Pro and core regression tests for full-preview **dimensions, orientation,
  cancellation, and cache reuse**.
- **Read:** no benchmark code or results exist anywhere in the repository.
- **Read:** `tests/hiraco_core_tests.cpp` has 12 tests. `TestRealOrfPreviewSmoke`,
  `TestCropPreviewMatchesFullImageCrop`, and
  `TestDisplayProcessingCacheScalesBothInputs` cover dimensions, scaling, and
  crop-vs-full consistency. There is **no orientation test, no cancellation
  test, and no explicit cache-reuse test**.

**Expected benefit.** The preview orchestration in `RenderConvertedFullPreview`
(`src/hiraco_core.cpp:878-1110`) is the most intricate code in the core and the
hardest to reason about. Pinning its orientation, cancellation, and cache-reuse
behaviour makes every later change to it — including I-01 and I-02 — safer.

**Risk.** Low. Tests are additive. The main cost is constructing inputs that
actually exercise the paths (a real ORF with a non-trivial `libraw_flip`, and a
cancel check that fires mid-preview).

**Verification.** `ctest --test-dir build/release --output-on-failure` passes
with the new tests included, and each new test fails if the corresponding
behaviour is deliberately broken.

---

### I-06 — Remove dead guidance-map upsample code

- **Class:** Maintainability
- **Priority:** P3
- **Effort:** S
- **Dependencies:** none

**Evidence.** Finding F6 and drift D5 in `project-review.md`.

- `UpsampleFloatMap` (`src/dng_writer_bridge.cpp:583`) and
  `UpsampleFloatMapRegion` (`:631`) are defined but **never called** — verified
  by grep across `src/`.
- `Is80MpFrame` (`:121`) is unused in the writer; a separate overload exists at
  `src/hiraco_gui.cpp:221` and is used there.
- `algorithm.md` §9 presents the removal of full-frame upsample buffers as
  complete, which is not quite true.

**Expected benefit.** Less code to reason about, and the documentation stops
overstating what the optimization achieved.

**Risk.** Low. Pure deletion after confirming no callers.

**Verification.** Build succeeds; `ctest` passes; grep confirms zero remaining
references.

---

### I-07 — Drop the discarded highlight-recovery parameters

- **Class:** Maintainability
- **Priority:** P3
- **Effort:** S
- **Dependencies:** none

**Evidence.** §9 of `project-review.md`.

- `BuildLinearDngPayload` accepts `highlight_recovery_source_path` and
  `enable_highlight_recovery` and discards both with `(void)` casts at
  `src/dng_writer_bridge.cpp:3381-3382`.
- `BuildProcessingCacheFromRaw` does the same at `:4043-4044`.
- This is deliberate — `algorithm.md` §9 states recovery is applied outside the
  processing cache so the per-file switch takes effect immediately — but the
  unused parameters are a maintainability hazard.

**Expected benefit.** The signatures stop promising something they do not do.
A future reader cannot assume the cache respects the recovery switch.

**Risk.** Low. Signature-only change; the behaviour is already correct.

**Verification.** Build succeeds; `ctest` passes; confirm the `(void)` casts are
gone and recovery still applies (the `TestLocalOm3RecoveryFullPreviewSmoke`
test covers this).

---

### I-08 — Reduce `Load mosaic and CFA` cost

- **Class:** Performance
- **Priority:** P3
- **Effort:** M
- **Dependencies:** I-02 (so the measurement is trustworthy)

**Evidence.** Finding F5 in `project-review.md`.

- **Executed:** on the 80 MP source, `Load mosaic and CFA` took **1395.2 ms** —
  63 % of the raw-domain render (2202.8 ms) and 16 % of total conversion time
  (8935.7 ms).
- The loop at `src/dng_writer_bridge.cpp:2660-2686` is already OpenMP-parallel
  (`:2660`), so the cost is likely LibRaw's `raw_image` access pattern rather
  than raw throughput.

**Expected benefit.** A direct cut to the largest single line item in the
conversion profile. Even a 30 % reduction is ~0.4 s on an 80 MP frame.

**Risk.** Medium. This is the hottest loop in the raw-domain path and it feeds
every downstream stage. Any change must preserve the exact normalization and
CFA indexing, and the `region` handling must stay correct for cropped previews.

**Verification.** Convert the 80 MP source before and after; assert identical
output bytes (or an explicitly approved delta) and a lower
`Load mosaic and CFA` timing.

---

### I-09 — Avoid materializing the full-resolution guide buffer

- **Class:** Memory / performance
- **Priority:** P3
- **Effort:** M
- **Dependencies:** none

**Evidence.** Drift D5 and §4.6 of `project-review.md`.

- `upsampled_stack_guide.resize(pixel_count, 0.0f)` at
  `src/dng_writer_bridge.cpp:2978` allocates a full-resolution `float` buffer —
  ~321 MB for an 80 MP frame — and it is written and read throughout the
  refinement (`:3083-3091`), detail lift (`:3220-3224`), and RGB
  reconstruction (`:3282-3284`) stages.
- Five of the seven guidance maps already use on-demand `RegionMapSampler`s;
  `stack_guide` is the sole exception because the refinement needs guide values
  at every neighbour of every pixel.

**Expected benefit.** Lower peak memory (relevant to the 2 GB GUI LRU budget and
the documented 10 GB working-set allowance) and one less full-frame pass.

**Risk.** Medium. The guide is consumed in three separate stages with
different neighbourhood patterns, so a sampling approach must reproduce all of
them. This is the most invasive item in the P3 group.

**Verification.** Convert 50 MP and 80 MP sources before and after; assert
matching output and a lower peak RSS.

---

### I-10 — Validate presets against 20 / 50 / 80 MP sources

- **Class:** Quality
- **Priority:** P4
- **Effort:** L
- **Dependencies:** I-04 (the two decisions interact)

**Evidence.** Drift D3 in `project-review.md`.

- `algorithm.md` §11.3 requires that "the final numeric values must be
  validated against representative 20 MP, 50 MP, and 80 MP sources before they
  become user-facing defaults."
- `ResolveStageSettingsForImageImpl` (`src/dng_writer_bridge.cpp:143-215`)
  ignores `metadata`, `width`, and `height` — `(void) metadata;` at `:147` — and
  returns static defaults.
- `Is50MpFrame`, `Is80MpFrame`, and `Is20MpFrame`
  (`src/hiraco_gui.cpp:211-239`) are used only for the resolution label
  (`ResolutionLabelForPrepared`, `:241`).

**Expected benefit.** Resolution-appropriate defaults, and an explicit,
validated answer to the question I-04 raises about which frames should be
enhanced.

**Risk.** High. This changes the look of every converted image and needs
subjective quality judgement across three sensor formats. It should be treated
as its own goal with human review, not folded into a code-change task.

**Verification.** A documented comparison across 20 / 50 / 80 MP sources with
the chosen defaults, signed off by the user.

---

### I-11 — Add sub-pixel registration to ORI highlight recovery

- **Class:** Algorithm / quality
- **Priority:** P4
- **Effort:** L
- **Dependencies:** none

**Evidence.** Drift D19 in `project-review.md`.

- `algorithm.md` §10 states the limit explicitly: registration is a global
  integer offset of at most two companion pixels, with no sub-pixel, motion,
  rotation, or scale compensation.
- `const int max_offset = 2;` at `src/dng_writer_bridge.cpp:2291`, with
  integer `dx`/`dy` applied at `:2387-2389`.

**Expected benefit.** Meaningfully better highlight rescue for real-world pairs,
where the camera is never perfectly registered to a whole companion pixel.

**Risk.** High. This changes the recovery output for existing pairs and must
preserve the subtractive-only invariant — the misalignment guard at `:2344-2345`
exists precisely because the current registration is coarse.

**Verification.** Convert a real companion pair before and after; confirm the
recovery still cannot brighten any pixel (the invariant test in
`TestEnhancementPreservesHighlightHeadroom` is the model to follow).

---

### I-12 — Validate companion identity, not just the filename

- **Class:** Robustness
- **Priority:** P4
- **Effort:** M
- **Dependencies:** none

**Evidence.** Drift D8 and §4.7 of `project-review.md`.

- `algorithm.md` §6.1 and §10 state that companion detection is a
  same-directory filename lookup only, with no validation of capture time,
  exposure, ISO, camera identity, or dimensions.
- `DetectHighlightRecoverySourcePath` (`src/hiraco_core.cpp:98-127`) is purely
  filename-based.
- The GUI already warns users to enable `HL Rec` only for known matching pairs
  (`README.md`).

**Expected benefit.** Prevents the failure mode where recovery silently uses
the wrong companion and produces a locally inaccurate highlight base — which
the subtractive design bounds but does not eliminate.

**Risk.** Medium. Over-validating will refuse legitimate pairs. Any check must
be conservative and fail open with a clear message rather than silently
disabling a feature the user turned on.

**Verification.** Construct a deliberately mismatched pair and confirm
detection refuses it; confirm a known-good pair still detects.

---

### I-13 — Expose `HL Rec` in the CLI

- **Class:** Feature parity
- **Priority:** P5
- **Effort:** S
- **Dependencies:** none

**Evidence.** Drift D22 in `project-review.md`.

- `README.md`: "The CLI does not currently expose a switch for this feature."
- `src/main.cpp` accepts only `convert <source> <output>
  [--compression …] [--debug]`.
- The core already supports it: `ConvertToDng` forwards
  `enable_highlight_recovery` (`src/hiraco_core.cpp:1112-1148`), and the GUI
  sets it per file (`src/hiraco_gui.cpp:2546-2547`).

**Expected benefit.** Batch highlight recovery without the GUI, which is the
whole point of having a CLI frontend.

**Risk.** Low. Additive flag on an existing path.

**Verification.** Convert a companion pair with and without the flag; confirm
the outputs differ and that the flag has no effect when no companion is present.

---

### I-14 — Add a reproducible GUI benchmark mode

- **Class:** Performance
- **Priority:** P5
- **Effort:** M
- **Dependencies:** I-05

**Evidence.** Drift D4 in `project-review.md` — `algorithm.md` §11.5 item 5
asks for a full-preview refresh benchmark on the M4 Pro.

**Expected benefit.** A repeatable measurement of the preview path, so future
changes to `RenderConvertedFullPreview` and the dispatcher can be judged
rather than guessed at.

**Risk.** Low. Read-only measurement harness.

**Verification.** The benchmark runs twice on the same file and produces
comparable numbers.

---

## 4. Suggested Sequencing

**Phase 1 — correctness and observability (do first).**
I-01 → I-02 → I-03.
Independent of each other; I-02 is the cheapest and I-01 is the most valuable.

**Phase 2 — safety net.**
I-05.
Land before touching the preview or conversion path further.

**Phase 3 — tidy up.**
I-06, I-07, I-13.
Small, independent, low risk. I-13 is user-facing and can ship any time.

**Phase 4 — deeper work (each is its own decision).**
I-04, I-08, I-09, I-12, I-10, I-11, I-14.
I-04 and I-10 are quality decisions that need human judgement on real images.
I-08, I-09, and I-11 change numerical output and need before/after comparison.
I-12 changes user-visible detection behaviour.

**Dependencies.** I-02 before I-08 (measure with trustworthy timings). I-05
before I-09 and I-11 (regression cover for the path being changed). I-04
informs I-10.
