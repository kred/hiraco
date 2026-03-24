# hiraco

`hiraco` is a Python CLI for converting OM SYSTEM high-resolution ORF files to linear DNG while preserving as much metadata as possible.

The implementation is split into two layers:

- Python CLI for orchestration, dependency checks, metadata reconciliation, and packaging.
- A native helper that decodes ORF through LibRaw and writes DNG through the Adobe DNG SDK.

## Current status

This repository currently contains:

- project packaging via `pyproject.toml`
- CLI entry point `hiraco`
- configuration and native-helper request models
- native helper with a cross-platform CMake build
- Adobe DNG SDK runtime-backed linear DNG write path for `uncompressed`, `deflate`, and `jpeg-xl`

The current native helper can write linear DNG output through the Adobe DNG SDK
for `uncompressed`, `deflate`, and `jpeg-xl` requests. The writer requests DNG
`1.6.0.0` compatibility for `uncompressed` and `deflate`, and experimental DNG
`1.7.1.0` compatibility for `jpeg-xl`, but the Adobe SDK may stamp a lower
final `DNGVersion` when the file does not require newer features.

## Bootstrap

Create and use a virtual environment:

```bash
/usr/bin/python3 -m venv .venv
source .venv/bin/activate
python -m pip install --upgrade pip
python -m pip install -r requirements.txt
```

The editable install is kept in `requirements.txt`, so `rawpy` and the local
`hiraco` package are installed together.

Install native prerequisites on macOS:

```bash
brew install cmake exiftool libraw
```

For native development, the current helper expects a system LibRaw installation.

Adobe DNG SDK is a required manual dependency and the only supported native
build mode. It cannot be redistributed in this repository. Before building,
copy your local Adobe DNG SDK bundle into `dng_sdk_1_7_1/`, or point CMake at a
local copy with `HIRACO_DNG_SDK_ROOT`.

Build the native helper:

```bash
hiraco build-native
```

Example with an external SDK checkout:

```bash
cmake -S native -B native/build -DHIRACO_DNG_SDK_ROOT=/path/to/dng_sdk_1_7_1
cmake --build native/build
```

## Usage

This project explicitly focuses on a direct file conversion paradigm. The CLI has been stripped of legacy evaluation and maker-note diagnostic logic.

Convert utilizing standard explicit Deflate or JPEG-XL compression:

```bash
hiraco convert _3210505.ORF output.dng --compression deflate
hiraco convert _3210505.ORI output.dng --compression jpeg-xl
```

Convert into Uncompressed 16-bit DNG format:

```bash
hiraco convert _3210505.ORF output.dng --compression uncompressed
```

## Repository notes

- Adobe DNG SDK is required for the full DNG writer, but it is not included in this repository and must be copied in manually from Adobe's distribution.
- The expected default local path is `dng_sdk_1_7_1/`, or you can override it with `HIRACO_DNG_SDK_ROOT` during CMake configure.
- Automatic metadata copy during `hiraco convert` preserves EXIF, IPTC, and XMP, but intentionally skips MakerNotes because raw-camera white-balance and vendor processing tags can corrupt rendered linear DNG color.
- The current converter writes rendered linear RGB DNG, not mosaic/raw DNG. That means color should stay stable across viewers, but some raw editors may present it more like a high-bit-depth rendered image than a fully editable camera-raw file.
- Rendered DNG output is normalized to a generic `hiraco` camera identity after metadata copy so raw editors do not apply OM SYSTEM camera profiles to already rendered pixels.
