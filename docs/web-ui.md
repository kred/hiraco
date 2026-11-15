# The hiraco web UI

A live build is published at https://kred.github.io/hiraco/.

`web/` holds a Vite + React + TypeScript application that runs the full
hiraco conversion in the browser.  The browser loads the `hiraco-wasm`
module, prepares the RAW file, renders the previews, applies the presets and
sliders and writes the Linear DNG, all without uploading the file anywhere.

## Queue and presets

Several RAW files can be queued at once.  ORI files added in the same batch are
paired automatically with the RAW file of the same name (an ORI added on its
own is treated as a source).  The selected queue entry is the one shown in the
preview and edited by the presets and sliders, like the native queue.  The
presets and sliders are disabled until a file is selected; the Processing panel
names the file they edit.  Use the copy, paste and layers icons in the queue
footer to move a settings set between entries (`layers` pastes to every queued
file); `Convert` writes one DNG for every queued file using each entry's
settings.  Every icon has a tooltip and an accessible label.

The preset row has the native Small, Medium and Strong settings and one Custom
button.  Use the bookmark icon in the queue footer to store the selected file's
current absolute slider values as the single custom preset; the Custom button
applies it to the selected file.  Setting it again overwrites the previous
custom preset.  It is stored in `localStorage` and validated/clamped when read
back, so it normally survives a page reload.  A browser that blocks storage
(private mode, a full quota) keeps the preset for the session but cannot
persist it, and the old named-preset list `hiraco.custom-presets.v1` is
migrated to the new single slot.


## Requirements

- Node.js 24 or newer
- npm
- the WebAssembly module built by `scripts/build_wasm.sh` (or `./scripts/build_web.sh`)

## Building

```bash
# from the repository root
./scripts/build_web.sh
```

This builds the WebAssembly module, copies `hiraco-wasm.js` and
`hiraco-wasm.wasm` into `web/public/wasm/`, installs the node dependencies
and produces `web/dist/`.

## Running locally

```bash
cd web
npm run dev      # http://localhost:5173
npm run preview  # http://localhost:4173
```

The dev and preview servers send the `Cross-Origin-Opener-Policy: same-origin`
and `Cross-Origin-Embedder-Policy: require-corp` headers, which the module
needs for its shared memory.  A static host that does not send those headers
is still supported, because the application registers a small service worker
(`coi-serviceworker.js`) that adds them to every response.  The first visit
reloads the page once so the headers take effect.

The application cannot be opened directly from the file system (`file://`):
browsers block both ES modules and service workers on that protocol, so the
page has to be served over `http://` or `https://`, even locally.  Any
static server works, for example:

```bash
cd web/dist
python3 -m http.server 8000
```

## Testing

```bash
cd web
npm run test      # vitest unit tests
npm run e2e       # builds dist/ and runs the Playwright tests
```

The end-to-end server serves `dist/`; `npm run e2e` runs the build first so the
tests never exercise a stale bundle.  The end-to-end tests use these generated
or sample files:

- `sample/_8142490.ORI` and `build/reference/_8142490-deflate.dng` for the
  pixel comparison
- `sample/_8142489_.ORF`, `reference/_3060001.ORF` for the queue and copy/paste
  tests
- `sample/_8142492.ORF` and `sample/_8142492.ORI` for ORI pairing

They are not committed.  Regenerate the reference with
`scripts/make_reference_dngs.sh` after changing the writer, otherwise the pixel
comparison is against a stale baseline.  `HIRACO_E2E_RAW`,
`HIRACO_E2E_REFERENCE`, `HIRACO_E2E_SMALL_RAW`, `HIRACO_E2E_RAW_A`,
`HIRACO_E2E_RAW_B`, `HIRACO_E2E_PAIR_RAW` and `HIRACO_E2E_PAIR_ORI` override
the fixture paths, and `HIRACO_PYTHON` selects the interpreter used for the DNG
comparison (it must have NumPy).

## Supported browsers

The module is compiled for wasm64 with a 64 bit shared memory, so the browser
has to support 64 bit WebAssembly memory and the page has to be
cross-origin isolated.  Current Chrome and Edge support both; Firefox support
depends on its wasm64 memory implementation, and Safari does not support
wasm64 yet.  The application probes for this on startup and shows an
actionable message when it is missing; it does not fall back to a 32 bit
build, because a 10386 x 7792 sensor-shift capture peaks at about 8 GB of
resident memory and does not fit in a 32 bit address space.
