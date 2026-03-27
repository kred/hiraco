# hiraco

`hiraco` is a Python CLI pipeline designed to convert OM SYSTEM / Olympus High-Resolution sensor-shift `.ORF` raw files into robust, exceptionally detailed Linear DNG files. 

By deeply analyzing the sensor-shift characteristics of these cameras, `hiraco` operates a specialized custom native reconstruction engine to extract true sub-pixel resolution rather than relying on mathematically standard interpolations. It specifically targets the full optical extraction of High-Res composites.

The implementation is split into two layers:
- **Python CLI**: For task orchestration, CLI interfaces, dependency checks, and accurate metadata reconciliation mapping.
- **Native Helper (`hiraco-native`)**: A C++ engine compiling `LibRaw` decoders, high-performance spatial Deconvolution algorithms (via `FFTW`), and the official `Adobe DNG SDK` to package the processed payload.

## Current status
The project offers fully working conversion paths for standard and high-resolution camera payloads.
Features include:
- Native decoding of specific OM-3 High Resolution payload structures.
- **Advanced Deconvolution Pipeline**: Resolving the hardware Point Spread Function (PSF) blurring innate to multi-shot sensor shifts.
- Corrected radiometrics: Black-level and color neutralizing alignments mapped to proper EXIF bounds, neutralizing historically notorious color-cast display issues in third-party viewers.
- Storage paths supporting `uncompressed`, `deflate`, and modern `jpeg-xl` DNG matrices via DNG versions `1.6.0.0` or `1.7.1.0`.

## Installation & Bootstrap

Create and use a virtual environment:

```bash
/usr/bin/python3 -m venv .venv
source .venv/bin/activate
python -m pip install --upgrade pip
python -m pip install -r requirements.txt
```

Install native prerequisites on macOS:

```bash
# Using Homebrew
brew install cmake exiftool libraw
```

Adobe DNG SDK is a required manual dependency for the native packaging path. It cannot be redistributed in this repository. Before building, copy your local Adobe DNG SDK bundle into `dng_sdk_1_7_1/`, or point CMake to a local copy using `HIRACO_DNG_SDK_ROOT`. 

Build the native helper:

```bash
hiraco build-native
```

To build manually against an external SDK checkout:

```bash
cmake -S native -B native/build -DHIRACO_DNG_SDK_ROOT=/path/to/dng_sdk_1_7_1
cmake --build native/build
```

## Usage

This project explicitly focuses on a direct High-Res to DNG file conversion paradigm. Convert utilizing uncompressed arrays or standard Deflate/JPEG-XL compression:

```bash
# Convert to Uncompressed 16-bit Linear DNG
hiraco convert _3210505.ORF output.dng --compression uncompressed

# Convert using compression paths
hiraco convert _3210505.ORF output.dng --compression deflate
hiraco convert _3210505.ORI output.dng --compression jpeg-xl
```

## Repository notes

- The project relies heavily upon the Adobe DNG SDK for constructing the final negative container. It is not included and must be sourced from Adobe's developer distribution.
- Automatic metadata copy during `hiraco convert` utilizes `ExifTool` to preserve EXIF, IPTC, and XMP metadata while strictly pruning MakerNotes (e.g. `Olympus:all=`). MakerNote processing tags traditionally interfere with third-party software attempting to render already-linearized matrices.
- The converter writes a Rendered Linear RGB DNG, effectively performing the intensive spatial math and providing software with a clean, high-bit-depth image ready for baseline color and tonal manipulation.
