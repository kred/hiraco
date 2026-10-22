# The hiraco web UI

`web/` holds a Vite + React + TypeScript application that runs the full
hiraco conversion in the browser.  The browser loads the `hiraco-wasm`
module, prepares the RAW file, renders the previews, applies the presets and
sliders and writes the Linear DNG, all without uploading the file anywhere.

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
npm run e2e       # Playwright end to end test
```

## Supported browsers

The module is compiled for wasm64 with a 64 bit shared memory, so the browser
has to support 64 bit WebAssembly memory and the page has to be
cross-origin isolated.  Current Chrome, Edge, Firefox and Safari do.  The
application probes for this on startup and shows an actionable message when
it is missing; it does not fall back to a 32 bit build, because a 10386 x
7792 sensor-shift capture peaks at about 8 GB of resident memory and does
not fit in a 32 bit address space.
