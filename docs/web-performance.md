# WebAssembly performance

This page records the browser-path performance work and how to reproduce the
numbers.  The DNG conversion is still the same pipeline as the native build;
the changes below target the interactive path (opening a file, showing a
preview, moving sliders and switching queue entries).

## What was slow

The browser opens a WASM session and then renders previews through the same
core as the native GUI.  A default open used to render **both** the original
and the converted preview, even though the initial comparison mode is
`Converted`:

| 80 MP ORF (`sample/_8142492.ORF`) | Before |
|---|---:|
| `open_session` (LibRaw metadata only) | 16 ms |
| original preview (full-resolution LibRaw AAHD) | 17.7 s |
| converted preview | 9.5 s |
| **default open (both previews)** | **~27 s** |

The converted path also ran the detail enhancement on the full 80 MP raster
and only then resized the result to 2560 px.  Finally, the Emscripten pthread
pool was sized to `hardwareConcurrency + 1`, while OpenMP, FFTW and Halide can
each own a thread set.  Once the preview reached the FFT/Halide stages, new
worker creation was proxied to the module main thread, which was blocked in the
WASM call; in Chromium the 50/80 MP preview sat at "Running Stage 1/2" for
minutes.

## What changed

- **Lazy comparison previews.**  `openSession` now renders only the preview the
  selected comparison mode displays.  `Converted` (the default) never pays for
  the original decode; switching to `Original` or `Side by side` requests the
  missing preview then.  Slider changes while viewing `Original` no longer
  render an invisible converted preview.
- **Half-size original previews below 100%.**  `RenderOriginalPreview` asks
  LibRaw for `half_size = 1` for display-capped requests, which is a 2×2 Bayer
  merge over the full sensor data (not an embedded thumbnail).  100% zoom and
  the native GUI still use the full-resolution decode.
- **Per-row preview cache.**  A completed fit preview is stored on its queue
  item.  Selecting that row again paints the cached preview immediately and
  reopens the session in the background; the expensive preview render is
  skipped while the settings are unchanged.  Settings edits invalidate the
  cached converted preview.
- **Display-sized converted preview.**  Fit/50% requests use
  `RenderConvertedProxyPreview`: the linear full-resolution cache is built
  once, then a display-sized proxy is enhanced and tone-mapped.  100% and DNG
  conversion continue through the exact full-resolution path.  The fit/50%
  preview is therefore approximate; it is deliberately faster than replaying a
  50–80 MP FFT/Halide pipeline for a 2560 px view.
- **Smaller embedded DNG preview.**  The 1024 px thumbnail written into the
  DNG now uses the half-size decode too.  Raw samples and the full-size raw IFD
  are unchanged.
- **LibRaw `-O3`.**  The bundled WebAssembly LibRaw is compiled with `-O3`
  instead of `-O2` (still with `-ffp-contract=off` for cross-target
  reproducibility).
- **Thread-pool sizing and caps.**  FFTW and Halide are capped at 12 threads,
  OpenMP at 6 per team (the conversion's CFA-guide extraction runs on an async
  thread, so two teams can overlap), and the Emscripten pool is
  `min(hardwareConcurrency*3 + 4, 40)`.  This is the fix for the multi-minute
  stalls: with the old `cores + 1` pool, FFT/Halide work (and later the
  conversion) could not get all of its workers.
- **Maintenance cleanup.**  Preview completion no longer overwrites
  "Preparing image" or conversion status, and the worker/Node staging path is
  unchanged.

## Measurements

Local machine: Apple silicon, 12 hardware threads, Node 24.  Browser numbers
are Chromium 148 from Playwright.

### Node, browser bridge API

Command:

```bash
cmake --build build-wasm --target hiraco-web-assets
node tests/wasm_benchmark_harness.js build-wasm/hiraco-wasm.js \
  sample/_8142492.ORF 2560
```

| Phase | Before | After |
|---|---:|---:|
| `open_session` (80 MP) | 16 ms | 18 ms |
| original preview, 2560 | 17.7 s | **3.2 s** |
| converted preview, 2560 | 9.5 s | **6.2 s** |
| `open_session` (20 MP) | 12 ms | 13 ms |
| original preview, 2560 (20 MP) | 1.8 s | **0.75 s** |
| converted preview, 2560 (20 MP) | 1.9 s | **1.8 s** |

The default browser open now renders only the converted preview: about
**6–8 s instead of ~27 s** at 80 MP on the bridge, and about **2 s instead of
~3.7 s** at 20 MP.

### Browser end-to-end

A Playwright timing run on `sample/_8142492.ORF`:

- first converted preview after choosing an 80 MP file: **~7.9 s** (the same
  run previously sat in the FFT stage for more than four minutes);
- switching 20 MP A → 80 MP B: A's initial preview **2.3 s**, B **6.5–7.9 s**;
  returning to A **35 ms** from the per-row cache;
- 20 MP original preview after the mode switch: **0.8 s**;
- 50 MP and 80 MP Deflate conversion after the preview: **~25 s** each.  Before
  the thread-pool/OpenMP sizing fix these conversions stalled indefinitely at
  "Running Stage 2 wavelet refinement".

The normal 20 MP conversion test dropped from about 11.5 s to 9.5 s, and the
queue copy/paste test from about 24.4 s to 20.2 s, on the same machine.

## Reproduce and verify

```bash
cd web
npm run typecheck
npm run test            # 44 unit tests
npm run e2e             # 12 Playwright tests when the fixtures are present
npm run test:coi

# Native core tests (fixtures required)
cmake --build build --target hiraco-core-tests
./build/hiraco-core-tests

# DNG parity
python3 scripts/verify_wasm.py --native build-native-verify/hiraco-cli \
  --wasm build-wasm/hiraco-cli.js --reference-dir build/reference \
  --output-dir build/wasm-verify
```

The benchmark harness reports the module's progress timestamps, so a change
in the preparation, cache, enhancement or display phases is visible without
`HIRACO_TIMING` (the wasm module does not import the host environment).

## Known trade-offs

- Fit/50% converted previews are display proxies, not resized full-resolution
  renders.  Their detail-enhancement stages run at the display size, so
  highlight recovery masks and fine-scale sharpening can differ slightly from
  the 100% view.  The DNG output and the 100% preview keep the original exact
  pipeline.
- The queue preview cache keeps fit previews per row (about 20 MB for a
  2560×1920 RGBA preview).  Full-resolution previews are not cached per row.
- Conversion still uses the full-resolution pipeline.  On this machine the
  80 MP Deflate conversion takes about 25 s after the preview; the work above
  removes a thread-pool deadlock, but does not make conversion itself cheaper.

## Further WebAssembly hints worth evaluating

These are documented candidates from the Emscripten/LibRaw ecosystem, not all
implemented here:

- `LibRaw::open_buffer` instead of MEMFS staging, plus sending `File` objects
  to the worker, removes two full-file copies for large RAWs.
- `LibRaw::copy_mem_image` avoids the extra full-size allocation/copy currently
  done by `dcraw_make_mem_image`.
- LibRaw can be compiled with OpenMP (it is currently configured with
  `--disable-openmp`); this mainly helps the 100% original preview and
  non-OM-3 Bayer sources.
- Building the enhancement workspace once and reusing FFT buffers would cut
  several gigabytes of allocation/zeroing per 80 MP slider change.
- `-sMALLOC=mimalloc`, a larger `MEMORY_GROWTH_GEOMETRIC_CAP`, and
  `-sPTHREAD_POOL_SIZE_STRICT=2` are worth a measured A/B.
- The cross-origin-isolation service worker currently rebuilds every response;
  restricting it to navigations would restore streaming WASM compilation and
  allow the browser's WASM code cache to work.

References:

- Emscripten settings reference (pthreads, memory growth, malloc):
  https://emscripten.org/docs/tools_reference/settings_reference.html
- Emscripten pthreads guide:
  https://emscripten.org/docs/porting/pthreads.html
- LibRaw C++ API (`open_buffer`, `copy_mem_image`, `dcraw_process` reuse):
  https://www.libraw.org/docs/API-CXX.html
- LibRaw half-size behaviour:
  https://www.libraw.org/node/1990
- V8 WebAssembly compilation and code caching:
  https://v8.dev/blog/wasm-code-caching
