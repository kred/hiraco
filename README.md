# hiraco

`hiraco` is a native C++ toolchain for converting selected Olympus / OM SYSTEM RAW files, with a custom emphasis on high-resolution sensor-shift captures, into rendered Linear DNG files.

The repository builds one shared processing core and exposes it through native and browser-oriented targets:
- `hiraco-cli` for command-line conversion
- `hiraco-gui` for interactive preview, tuning, and batch conversion
- `hiraco-wasm` (experimental) for a non-UI WebAssembly build of the conversion core and DNG SDK integration

The current implementation supports both a custom high-resolution raw-domain reconstruction path and a LibRaw-based render path for other supported inputs. The native desktop stack remains the default, while the WASM target is intended as a browser-facing build path for the conversion layer rather than a direct port of the desktop GUI.
![Comparison](contact_sheet.png)
## GUI

![hiraco GUI screenshot](hiraco-gui.png)

## What The Project Currently Provides

- `hiraco-core`: shared static library containing preparation, metadata, preview, full-preview, enhancement, and DNG writing code.
- `hiraco-cli`: command-line `convert` frontend.
- `hiraco-gui`: wxWidgets desktop frontend with thumbnail queueing, full-frame comparison, output controls, and conversion.
- `hiraco-wasm`: experimental browser target built with Emscripten; the GUI is intentionally excluded and the build focuses on the non-visual conversion core and DNG SDK integration.
- Custom high-resolution raw-domain reconstruction driven by vendor metadata and stack-guidance maps.
- Optional, per-file ORI-assisted highlight recovery for supported high-resolution
  ORFs when a same-directory companion candidate is detected.  The correction is
  disabled by default and is subtractive: it preserves high-resolution detail
  while borrowing only a lower highlight base from the companion.
- Rendered Linear DNG output with `uncompressed`, `deflate`, and `jpeg-xl` compression modes.
- Three user-facing processing control groups in the GUI:
  - `Detail Recovery`
  - `Multi-scale Detail`
  - `Edge Refinement`
- Cached original previews, full-resolution processing data, and display-resolution processing derivatives for responsive converted full-image inspection in the GUI.
- Optional timing output for performance work via `--debug` or `HIRACO_TIMING=1`.

## Supported Workflows

- Convert `.ORF` and `.ORI` sources to rendered Linear DNG.
- Open the GUI, inspect full-frame original and converted previews in original,
  converted, or side-by-side comparison modes.
- Pan and zoom the comparison view while adjusting processing controls.
- Queue multiple files, choose a specific folder, the source folder, or a source
  subfolder, and batch convert them.

The strongest custom path in the repository today is the high-resolution workflow. Standard sources still go through the shared core, but the specialized raw-domain reconstruction is targeted at the  high-resolution capture format.

## Requirements

`hiraco` currently expects:

- CMake 3.28+
- LibRaw
- FFTW3 and `fftw3_threads`
- Halide
- Zlib
- Boost headers (required by the Adobe XMP toolkit bundled with the DNG SDK)
- OpenMP (optional but strongly recommended)
- wxWidgets (for `hiraco-gui`)
- Emscripten toolchain (for `hiraco-wasm`)
- Adobe DNG SDK 1.7.1 bundle unpacked into `dng_sdk_1_7_1/`

The repository also expects the bundled Adobe libjxl and XMP toolkit that ship with the DNG SDK bundle under `dng_sdk_1_7_1/`.

### macOS

```bash
brew install cmake libraw fftw halide libomp pkg-config wxwidgets
```

### Linux (Debian/Ubuntu)

```bash
sudo apt update
sudo apt install build-essential cmake libraw-dev libfftw3-dev libhalide-dev libomp-dev pkg-config zlib1g-dev libwxgtk3.2-dev
```

### Windows (vcpkg)

```powershell
vcpkg install boost libraw fftw3 halide zlib wxwidgets:x64-windows
```

### WebAssembly (Emscripten)

For the browser target, install the Emscripten toolchain first:

```bash
brew install emscripten
# or follow the official emsdk install flow if you prefer managing a specific version
```

After installation, configure the browser build with `emcmake` as shown below.

## Building

### Configure And Build

#### Native build (default)

```bash
cmake --preset release
cmake --build --preset release
```

This produces:

- `build/release/hiraco-cli`
- `build/release/hiraco-gui`
- `build/release/libhiraco-core.a`

#### WebAssembly target (experimental)

```bash
# install Emscripten first, then configure the browser target
emcmake cmake -S . -B build-wasm -DHIRACO_BUILD_WASM=ON -DHIRACO_BUILD_GUI=OFF -DHIRACO_BUILD_TESTS=OFF
cmake --build build-wasm --target hiraco-wasm
```

This produces a browser build under `build-wasm/` with the WebAssembly module and JavaScript wrapper:

- `build-wasm/hiraco-wasm.js`
- `build-wasm/hiraco-wasm.wasm`

The browser target intentionally disables the desktop GUI and is aimed at the non-UI conversion core and Adobe DNG SDK integration. The native desktop build remains the default and continues to work unchanged.

If `wx-config` is not on `PATH` on macOS, point CMake at it explicitly:

```bash
cmake --preset release \
  -DwxWidgets_CONFIG_EXECUTABLE="$(brew --prefix wxwidgets)/bin/wx-config"
cmake --build --preset release
```

### Useful Build Switches

```bash
cmake -S . -B build -DHIRACO_BUILD_GUI=OFF
cmake -S . -B build -DHIRACO_BUILD_CLI=OFF
cmake -S . -B build -DHIRACO_BUILD_TESTS=ON

# WebAssembly / browser build
emcmake cmake -S . -B build-wasm -DHIRACO_BUILD_WASM=ON -DHIRACO_BUILD_GUI=OFF -DHIRACO_BUILD_TESTS=OFF
```

## Running

### CLI

The CLI currently exposes one command:

```bash
./build/release/hiraco-cli convert <source> <output> [--compression uncompressed|deflate|jpeg-xl] [--hl-rec] [--debug]
```

Examples:

```bash
./build/release/hiraco-cli convert _3210505.ORF output.dng --compression uncompressed
./build/release/hiraco-cli convert _3210505.ORF output.dng --compression deflate
./build/release/hiraco-cli convert _3210505.ORI output.dng --compression jpeg-xl --debug
./build/release/hiraco-cli convert _3210505.ORF output.dng --hl-rec
```

`--debug` enables timing logs for the main processing phases.  `--hl-rec`
enables ORI-assisted highlight recovery when a companion file is detected next to
the source; it is off by default and has no effect when no companion is found.

### GUI

```bash
./build/release/hiraco-gui
```

The GUI currently supports:

- Adding files with a button or drag-and-drop
- Persistent thumbnail queue with per-file removal and processed-state feedback
- Full-frame original/converted comparison with `Original`, `Converted`, and
  synchronized `Side by side` modes
- `Fit`, `50%`, and `100%` zoom modes, plus direct panning when zoomed
- `Small`, `Medium`, and `Strong` processing presets alongside direct controls
- Compression plus expandable output options for a specific folder, next to the
  source ORF, or a subfolder under it
- Interactive processing controls for detail recovery, multi-scale detail, and edge refinement
- Per-file `HL Rec` switch when a high-resolution ORF has a detected companion
  candidate; it is off by default
- Overwrite prompts, progress reporting, and cancellation

## Tests

If tests are enabled, the repository builds `hiraco-core-tests`:

```bash
cmake --preset relwithdebinfo -DHIRACO_BUILD_TESTS=ON
cmake --build --preset relwithdebinfo --target hiraco-core-tests
ctest --test-dir build/relwithdebinfo --output-on-failure
```

The test target currently covers core-library behavior rather than full GUI automation.

## Processing Notes

At a high level, the current pipeline is:

1. Inspect the source with LibRaw and build reusable base metadata.
2. Lazily enrich that metadata with vendor MakerNote and stack-guidance data when needed.
3. Render either:
   - a cached original preview,
   - a cached converted full-image preview, or
   - a full rendered Linear DNG payload.
4. Apply enhancement stages when the source metadata requests predicted detail gain.
5. Optionally apply ORI-assisted highlight recovery for an enabled, detected
   companion.
6. Package the final rendered image through the Adobe DNG SDK.

Companion detection is currently filename-based only; it does not verify that the
ORI/ORF pair belongs to the same capture.  Enable `HL Rec` only for known matching
pairs.  See [algorithm.md](algorithm.md) for the implementation details and current
limitations.

## Legal Disclaimer

*OM SYSTEM and Olympus are registered trademarks of OM Digital Solutions Corporation or Olympus Corporation. This project is not affiliated with, endorsed by, or sponsored by these companies. All trademarks are used only for compatibility description and nominative fair use purposes.*
