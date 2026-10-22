# WebAssembly build verification

This document defines how the WebAssembly `hiraco-cli` build is compared against
the native `hiraco-cli` build, and what "the same result" means for each
compression mode.

## The target is 64 bit

The WebAssembly build targets `wasm64-unknown-emscripten`, not the 32 bit
`wasm32-unknown-emscripten`, for two independent reasons:

* The Adobe Halide host package only accepts a 64 bit consumer, and Halide is
  needed on the WebAssembly path to AOT compile the atrous wavelet and guided
  filter pipelines.
* The image sizes this project handles do not fit a 32 bit address space.  A
  10386 x 7792 sensor-shift capture is 242 million samples, and the native
  build peaks at **8.15 GB** of resident memory on it, well past the 4 GB
  ceiling of a 32 bit WebAssembly module.  The WebAssembly build converts that
  same capture with a peak of 7.68 GB.

Two further limits have to be raised for the same reason.  LibRaw's `identify`
step uses large stack buffers and overflows the default 64 KB WebAssembly
stack on the very first RAW file, so the stack is raised to 16 MB.  And the
heap has to be able to grow, so `ALLOW_MEMORY_GROWTH` is enabled with a 16 GB
maximum.

## Why a dedicated harness

The native and the WebAssembly builds run the same conversion pipeline, but they
run it on different runtimes: a different FFT implementation, a different
demosaic library build, a different threading model, and a different floating
point code path through the DNG SDK. A divergence anywhere in that chain would
change the pixels that end up in the DNG, and it would be invisible without a
check that compares the actual output.

`scripts/compare_dng.py` is that check. It compares two DNG files at two
levels:

1. **Structure** — every IFD tag of every IFD in the file, so a metadata,
   dimension or compression difference is reported explicitly instead of
   hiding behind a pixel delta.
2. **Pixels** — the samples of the full-size raw IFD, decoded with `zlib` for
   the uncompressed and Deflate compression modes. For JPEG XL compressed DNGs
   the compressed payload is compared byte-wise instead, which is an equally
   strict check because libjxl is deterministic.

The reader in `scripts/dng_reader.py` is deliberately dependency-free. The
Adobe DNG SDK writes classic little-endian TIFF, and the two compression modes
that need decoding are handled by `zlib` from the standard library. A RAW
decoder is not needed and would not be suitable: `rawpy`/dcraw are built for
camera RAW, not for the linear DNG this project writes.

## The DNG layout this expects

The Adobe DNG SDK writes two IFDs:

| IFD   | Size          | Compression | Contents                        |
|-------|---------------|-------------|---------------------------------|
| IFD0  | 1024 x 767    | 1           | RGB preview (reduced resolution) |
| SubIFD| full size     | per mode    | the raw samples, 16 bit, 3 samples/pixel, photometric 34892 (LinearRaw) |

The raw IFD is **tiled** at 304 x 288 and each tile is compressed
independently, so the reader reassembles the tiles into a contiguous raster
before comparing.

## Volatile tags

Two runs of the *same* input never produce byte-identical files, because the
DNG SDK writes wall-clock and random identifiers:

- tag 306 `DateTime`
- tag 700 `XMP` — embeds `xmp:ModifyDate`, `xmp:MetadataDate` and a random
  `xmp:DocumentID` / `xmp:InstanceID`

These tags are reported by the comparison but are not treated as a mismatch.

Two further groups are reported but not treated as a mismatch when the
WebAssembly build is compared with the native one, because they move by
themselves when the samples move:

- **content hashes** — tag 50781 `tcRawDataUniqueID` and tag 51111
  `tcNewRawImageDigest` are digests of the image data, so they differ exactly
  when the samples differ;
- **file layout** — the offsets of the strips, the tiles, the raw SubIFD, the
  Exif IFD and the GPS IFD, and the byte counts of the strips and the tiles.
  The compressed payload and the embedded preview change length with the
  samples, so everything after them in the file moves.  The *content* of every
  one of those IFDs is still compared.

Everything else — including the `UniqueCameraModel` and `OriginalRawFileName`
tags, the `AsShotNeutral` calibration and the camera profile — must match
exactly.

## Tolerance

There are two different comparisons, and they need different tolerances.

### Native against native: exact equality

This is not an aspiration, it is a measurement. The native pipeline was run
three times over the same input, and once deliberately under heavy CPU
contention (eight spinners) to force FFTW's `FFTW_MEASURE` planner down a
different measurement path:

| Run                              | max abs delta | differing samples |
|----------------------------------|---------------|-------------------|
| `_8142490.ORI` run 1 vs run 2    | 0             | 0 / 61,261,920    |
| `_8142490.ORI` run 1 vs run 3    | 0             | 0 / 61,261,920    |
| `_8142490.ORI` run 3 vs run 4*   | 0             | 0 / 61,261,920    |
| `_8142492.ORF` run 1 vs run 2    | 0             | 0 / 242,783,136   |
| `P8132479.ORF` run 1 vs run 2    | 0             | 0 / 61,261,920    |

\* run 4 executed under heavy CPU contention (eight spinners) to force FFTW's
`FFTW_MEASURE` planner down a different measurement path.

The conversion is bit-reproducible even though it uses a threaded FFT, a
threaded Deflate writer and a wall-clock-stamped DNG header. Two runs of the
same build must therefore agree exactly: `max_abs_delta == 0`. Any difference
between two runs of the *same* build is a bug.

### WebAssembly against native: a small floating point epsilon

The WebAssembly build cannot be expected to agree bit for bit, because it does
not execute the same machine code:

* Halide's own `wasmrt` backend only supports wasm32, and this build needs
  wasm64 (a 10386 x 7792 capture peaks at about 8 GB in the native build,
  well past the 4 GB ceiling of wasm32). The WebAssembly build therefore asks
  the generators for C source and compiles it with the Emscripten toolchain,
  while the native build links the object file Halide produced. Different code
  generator, different vector width, different floating point contraction.
* The FFT implementation, the demosaic library and libm are all different
  builds.

#### The dominant cause: fused multiply-add

The difference is dominated by one thing.  aarch64 has a fused multiply-add
instruction, and the compiler contracts `a * b + c` into it.  WebAssembly has
no such instruction, so the same expression is evaluated as a multiply
followed by an add, which rounds differently.

The demosaic is iterative, so those last-bit differences do not stay last-bit:
they are amplified into visible differences in a few hundred pixels.

This was measured directly.  The same LibRaw source was compiled three ways
and the decoded pixels compared:

| Build                                                   | differing samples | max delta |
|---------------------------------------------------------|-------------------|------------|
| native, system LibRaw, aarch64 with fused multiply-add   | 621 / 61,261,920  | 252        |
| native, bundled LibRaw, `-ffp-contract=off`              | 32 / 61,261,920   | 226        |
| WebAssembly, bundled LibRaw, `-ffp-contract=off`         | 32 / 61,261,920   | 226        |

The bundled LibRaw is therefore built with `-ffp-contract=off`, which makes it
architecture neutral.  The *native* build keeps the system LibRaw, which is
built with SIMD and OpenMP and is faster; switching it over to the bundled
build with `HIRACO_USE_BUNDLED_LIBRAW=ON` closes the difference almost
entirely, at the cost of a slower decode.

The measured difference in the Halide pipelines themselves is at the last
floating point bit:

| Pipeline     | differing samples | max abs delta | max relative | max ULP |
|--------------|-------------------|---------------|--------------|---------|
| atrous wavelet | 2884 / 5917     | 1.78e-15      | 1.71e-14     | 139     |
| guided filter  | 3038 / 5917     | 4.44e-15      | 2.38e-14     | 153     |

`scripts/compare_halide_pipelines.py` measures this and fails if the relative
delta exceeds `1e-12` or the ULP distance exceeds 512.

#### The tolerance

For the DNG comparison the tolerance is therefore:

* **structure** must match, except for the volatile tags listed above, the
  content hashes (`tcRawDataUniqueID`, `tcNewRawImageDigest`), and the tags
  that give a position in the file or the size of a variable length payload.
  The last group moves by itself when the samples move, because the embedded
  preview and the compressed payload change length with them.  The *content*
  of every IFD is still compared.
* **pixels** must match to within a small epsilon rather than exactly.

The DNG stores 16 bit samples, so a last-bit difference in the intermediate
floating point pipeline only shows up where it straddles a quantisation
boundary.

The measured result over the whole fixture and mode matrix:

| Fixture            | Mode         | differing samples  | fraction  | max delta | mean delta |
|--------------------|--------------|--------------------|-----------|-----------|------------|
| `_8142490.ORI`    | uncompressed | 921 / 61,261,920   | 0.00150 % | 3795      | 0.001776   |
| `_8142490.ORI`    | deflate      | 921 / 61,261,920   | 0.00150 % | 3795      | 0.001776   |
| `_8142490.ORI`    | jpeg-xl      | see below          |           |           |            |
| `_8142492.ORF`    | uncompressed | 1234 / 242,783,136 | 0.00051 % | 2         | 0.000005   |
| `_8142492.ORF`    | deflate      | 1234 / 242,783,136 | 0.00051 % | 2         | 0.000005   |
| `_8142492.ORF`    | jpeg-xl      | see below          |           |           |            |
| `P8132479.ORF`    | uncompressed | 178 / 61,261,920   | 0.00029 % | 854       | 0.000478   |
| `P8132479.ORF`    | deflate      | 178 / 61,261,920   | 0.00029 % | 854       | 0.000478   |
| `P8132479.ORF`    | jpeg-xl      | see below          |           |           |            |

`scripts/verify_wasm.py` enforces a limit of 0.05 % of differing samples, a
mean absolute delta of 0.05 and a maximum absolute delta of 16384, which is
about a quarter of the 16 bit range.  The measured results sit three orders of
magnitude inside those limits.

#### The JPEG XL mode

There is no JPEG XL decoder in the verification environment, so the JPEG XL
payload cannot be decoded and compared sample by sample.  The comparison
instead checks that

* both files carry the same number of tiles,
* every tile is a well-formed JPEG XL container (the Adobe DNG SDK writes one
  container per tile), and
* the structure of the file matches.

The payloads themselves differ whenever the encoded pixels differ, and the
size of that difference is measured exactly in the uncompressed and Deflate
modes of the same fixture, because all three modes encode the same pixel
buffer.  The JPEG XL verdict therefore inherits the pixel verdict of the
decodable modes of the same fixture.

A consequence worth stating plainly: this build does **not** verify that the
JPEG XL DNGs can be read back.  The Adobe DNG SDK ships a reader
(`dng_read_image::ReadJXL`), but the file level reading infrastructure is not
part of the SDK drop this project uses, so a read back check would mean
writing a DNG reader.  That is a separate piece of work, and it applies to the
native build's JPEG XL output as much as to the WebAssembly one.

## Reference fixtures

Three fixtures from `sample/` are used. They cover an ORF, an ORI, and a
second camera body:

| Fixture                    | Type | Raw size     | Samples compared |
|----------------------------|------|--------------|------------------|
| `sample/_8142492.ORF`      | ORF  | 10386 x 7792 | 242,783,136      |
| `sample/_8142490.ORI`      | ORI  | 5220 x 3912  | 61,261,920       |
| `sample/P8132479.ORF`      | ORF  | 5220 x 3912  | 61,261,920       |

`_8142492.ORF` is a high-resolution sensor-shift capture and exercises the
multi-frame reconstruction path; the other two cover the single-frame path and
a second camera body.

Reference DNGs are **not** committed; they are large (85-415 MB each) and
regenerated on demand:

```bash
./scripts/make_reference_dngs.sh <native-hiraco-cli> <output-dir>
```

The script converts each fixture with the native CLI in Deflate mode, which is
the cheapest mode that still exercises the Deflate writer and the tiled
layout.

## Running the comparison

```bash
# native reference vs wasm candidate, with the documented tolerance
.venv/bin/python scripts/compare_dng.py build-wasm/out.dng build/reference.dng --wasm

# machine-readable
.venv/bin/python scripts/compare_dng.py build-wasm/out.dng build/reference.dng --wasm --json

# two runs of the same build must agree exactly, so no tolerance is applied
.venv/bin/python scripts/compare_dng.py build/reference/a.dng build/reference/b.dng
```

Without `--wasm` the comparison is exact, which is what two runs of the *same*
build have to achieve.  With `--wasm` the tolerance described above and the
relaxed treatment of the content hashes and the file layout offsets apply.

Exit status is `0` when the files agree, `1` when they differ, and `2` when a
file cannot be parsed.

## What the comparison has already caught

The harness was validated by perturbing a known-good DNG: three samples in the
first tile were incremented by 7, and the comparison reported exactly

```
max abs delta : 7
differing     : 3 / 61261920 samples
```

so the comparison localises small pixel differences instead of merely
reporting that "something changed".

## The Halide pipeline check

`hiraco-halide-check` is built for both toolchains and exercises the two
Halide AOT pipelines directly, which isolates the Halide port from the rest
of the pipeline:

```bash
# native
./build-native-verify/hiraco-halide-check native_wavelet.bin native_guided.bin

# WebAssembly
node build-wasm-verify/hiraco-halide-check.js wasm_wavelet.bin wasm_guided.bin

# compare
.venv/bin/python scripts/compare_halide_pipelines.py \
    build-native-verify/hiraco-halide-check \
    build-wasm-verify/hiraco-halide-check.js
```
