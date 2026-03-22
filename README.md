# hiraco

`hiraco` is a Python CLI for converting OM SYSTEM high-resolution ORF files to linear DNG while preserving as much metadata as possible.

The implementation is split into two layers:

- Python CLI for orchestration, dependency checks, metadata reconciliation, and packaging.
- A native helper that decodes ORF through LibRaw and writes DNG through the Adobe DNG SDK.

## Current status

This repository currently contains:

- project packaging via `pyproject.toml`
- CLI entry point `hiraco`
- environment and upstream freshness checks
- configuration and native-helper request models
- native helper with a cross-platform CMake build
- Adobe DNG SDK runtime-backed linear DNG write path for `uncompressed`, `deflate`, and `jpeg-xl`
- RAW inspection and metadata diff commands backed by rawpy and ExifTool

The current native helper can write linear DNG output through the Adobe DNG SDK
for `uncompressed`, `deflate`, and `jpeg-xl` requests.

## Bootstrap

Create and use a virtual environment:

```bash
/usr/bin/python3 -m venv .venv
source .venv/bin/activate
python -m pip install --upgrade pip
python -m pip install -e .
```

Run the doctor command:

```bash
hiraco doctor
```

Install native prerequisites on macOS:

```bash
brew install cmake exiftool libraw
```

For native development, the current helper expects a system LibRaw installation.

Adobe DNG SDK is a required manual dependency for the DNG writer path, but it
cannot be redistributed in this repository. Before building with
`--enable-dng-sdk`, copy your local Adobe DNG SDK bundle into
`dng_sdk_1_7_1/`, or point CMake at a local copy with `HIRACO_DNG_SDK_ROOT`.

Build the native helper:

```bash
hiraco build-native
```

Build the native helper with Adobe DNG SDK support enabled:

```bash
hiraco build-native --enable-dng-sdk
```

Example with an external SDK checkout:

```bash
cmake -S native -B native/build -DHIRACO_ENABLE_DNG_SDK=ON -DHIRACO_DNG_SDK_ROOT=/path/to/dng_sdk_1_7_1
cmake --build native/build
```

The native helper also exposes a low-level synthetic write validation command:

```bash
native/build/hiraco-native selftest-write --output tmp/test.dng --compression deflate
```

Run upstream freshness checks:

```bash
hiraco doctor --check-upstream
```

Inspect a RAW file:

```bash
hiraco inspect path/to/file.orf
```

Inspect native LibRaw support for a source file:

```bash
hiraco probe-native path/to/file.orf
```

Validate convert preflight without invoking the native helper:

```bash
hiraco convert path/to/file.orf output.dng --compression deflate --preflight-only
```

Compare metadata between two files:

```bash
hiraco metadata-diff source.orf output.dng
```

Preview the metadata copy command that will be used after DNG generation:

```bash
hiraco copy-metadata source.orf output.dng --dry-run
```

## Planned commands

- `hiraco doctor` validates the local environment and optionally checks current upstream releases.
- `hiraco build-native` configures and builds the native helper.
- `hiraco build-native --enable-dng-sdk` enables Adobe DNG SDK-backed writing, including bundled libjxl support for JPEG XL output.
- `hiraco convert ...` writes linear DNG for `uncompressed`, `deflate`, or `jpeg-xl`, then runs ExifTool metadata copy from the source file.
- Automatic metadata copy during `hiraco convert` preserves EXIF, IPTC, and XMP, but intentionally skips MakerNotes because raw-camera white-balance and vendor processing tags can corrupt rendered linear DNG color.
- The current converter writes rendered linear RGB DNG, not mosaic/raw DNG. That means color should stay stable across viewers, but some raw editors may present it more like a high-bit-depth rendered image than a fully editable camera-raw file.
- Rendered DNG output is normalized to a generic `hiraco` camera identity after metadata copy so raw editors do not apply OM SYSTEM camera profiles to already rendered pixels.
- `hiraco inspect` summarizes raw structure and metadata for an input file.
- `hiraco probe-native` shows native LibRaw decode support and camera metadata for an input file.
- `hiraco metadata-diff` compares metadata between source and destination files.
- `hiraco copy-metadata` applies ExifTool-based metadata transfer with a policy focused on EXIF, IPTC, XMP, and optional MakerNotes.

## Repository notes

- Adobe DNG SDK is required for the full DNG writer, but it is not included in this repository and must be copied in manually from Adobe's distribution.
- The expected default local path is `dng_sdk_1_7_1/`, or you can override it with `HIRACO_DNG_SDK_ROOT` during CMake configure.
- The repo is not yet a git repository.
