// wasm_benchmark_harness.js  —  Times the browser-facing WebAssembly session
// API from Node, without a browser or a worker.
//
// Usage:
//   node tests/wasm_benchmark_harness.js <hiraco-wasm.js> <input.ORF>
//        [maxDimension] [--convert] [--companion <file.ORI>]
//
// The module is the same one the browser worker loads.  This harness measures
// open_session, the original preview, the converted preview and (optionally)
// a deflate conversion, so a performance change can be compared on one
// machine before it is exercised in Chromium.
const fs = require('fs');
const path = require('path');

const STAGE_VALUE_COUNT = 8;
const now = () => performance.now();
const formatMs = (start, end) => `${(end - start).toFixed(1)} ms`;

function usage() {
  console.error('usage: node wasm_benchmark_harness.js <hiraco-wasm.js> <input.ORF> ' +
                '[maxDimension] [--convert] [--companion <file.ORI>]');
  process.exit(2);
}

async function main() {
  const args = process.argv.slice(2);
  if (args.length < 2) usage();
  const modulePath = args[0];
  const inputPath = args[1];
  let maxDimension = 2560;
  let convertRequested = false;
  let companionPath = '';
  for (let index = 2; index < args.length; ++index) {
    if (args[index] === '--convert') {
      convertRequested = true;
    } else if (args[index] === '--companion') {
      companionPath = args[++index] || '';
    } else {
      maxDimension = Number(args[index]);
    }
  }

  const createModule = require(path.resolve(modulePath));
  const progress = [];
  let start = now();
  const module = await createModule({
    print: (line) => {
      if (line.startsWith('HIRACO_PROGRESS|')) {
        progress.push([now(), line]);
      }
    },
    printErr: (line) => console.error(line),
  });
  console.log(`module init                  ${formatMs(start, now())}`);

  const raw = fs.readFileSync(inputPath);
  start = now();
  const rawPointer = module._malloc(raw.length);
  module.HEAPU8.set(raw, rawPointer);
  const rawNamePointer = module.stringToNewUTF8(path.basename(inputPath));
  let companionPointer = 0;
  let companionSize = 0;
  let companionNamePointer = 0;
  if (companionPath) {
    const companion = fs.readFileSync(companionPath);
    companionSize = companion.length;
    companionPointer = module._malloc(companion.length);
    module.HEAPU8.set(companion, companionPointer);
    companionNamePointer = module.stringToNewUTF8(path.basename(companionPath));
  }
  console.log(`copy raw into heap           ${formatMs(start, now())}`);

  start = now();
  const sessionId = module._hiraco_wasm_open_session(
    BigInt(rawPointer), BigInt(raw.length), BigInt(rawNamePointer),
    BigInt(companionPointer), BigInt(companionSize), BigInt(companionNamePointer));
  console.log(`open_session                 ${formatMs(start, now())}`);
  if (!sessionId) {
    console.error(module.UTF8ToString(module._hiraco_wasm_get_last_error()));
    process.exit(1);
  }
  module._free(rawPointer);
  module._free(rawNamePointer);
  if (companionPointer) {
    module._free(companionPointer);
    module._free(companionNamePointer);
  }

  const heap = () => new DataView(module.HEAPU8.buffer);
  const readU64 = (pointer) => Number(heap().getBigUint64(pointer, true));
  const previewPointer = module._malloc(8);
  const previewSize = module._malloc(8);
  const previewWidth = module._malloc(4);
  const previewHeight = module._malloc(4);
  const maskPointer = module._malloc(8);
  const maskSize = module._malloc(8);

  start = now();
  let status = module._hiraco_wasm_get_original_preview(
    sessionId, maxDimension, BigInt(previewPointer), BigInt(previewSize),
    BigInt(previewWidth), BigInt(previewHeight));
  console.log(`original preview             ${formatMs(start, now())}`);
  if (status !== 0) {
    console.error(module.UTF8ToString(module._hiraco_wasm_get_last_error()));
    process.exit(1);
  }
  module._hiraco_wasm_free_buffer(BigInt(readU64(previewPointer)), BigInt(readU64(previewSize)));

  const overrides = new Float64Array(STAGE_VALUE_COUNT).fill(Number.NaN);
  const overridesPointer = module._malloc(overrides.byteLength);
  module.HEAPU8.set(new Uint8Array(overrides.buffer), overridesPointer);
  start = now();
  status = module._hiraco_wasm_get_converted_preview(
    sessionId, maxDimension, BigInt(overridesPointer), BigInt(STAGE_VALUE_COUNT),
    BigInt(previewPointer), BigInt(previewSize), BigInt(previewWidth),
    BigInt(previewHeight), BigInt(maskPointer), BigInt(maskSize));
  console.log(`converted preview            ${formatMs(start, now())}`);
  if (status !== 0) {
    console.error(module.UTF8ToString(module._hiraco_wasm_get_last_error()));
    process.exit(1);
  }
  module._hiraco_wasm_free_buffer(BigInt(readU64(previewPointer)), BigInt(readU64(previewSize)));
  const convertedMaskSize = readU64(maskSize);
  if (convertedMaskSize) {
    module._hiraco_wasm_free_buffer(BigInt(readU64(maskPointer)), BigInt(convertedMaskSize));
  }

  if (convertRequested) {
    const outPointer = module._malloc(8);
    const outSize = module._malloc(8);
    const compression = module.stringToNewUTF8('deflate');
    start = now();
    status = module._hiraco_wasm_convert_session(
      sessionId, BigInt(compression), BigInt(overridesPointer), BigInt(STAGE_VALUE_COUNT),
      BigInt(outPointer), BigInt(outSize));
    const bytes = status === 0 ? readU64(outSize) : 0;
    console.log(`convert deflate              ${formatMs(start, now())} (${(bytes / 1e6).toFixed(1)} MB)`);
    if (status !== 0) {
      console.error(module.UTF8ToString(module._hiraco_wasm_get_last_error()));
      process.exit(1);
    }
    module._hiraco_wasm_free_buffer(BigInt(readU64(outPointer)), BigInt(bytes));
    module._free(outPointer);
    module._free(outSize);
    module._free(compression);
  }

  start = now();
  module._hiraco_wasm_close_session(sessionId);
  console.log(`close_session                ${formatMs(start, now())}`);
  console.log(`peak RSS                     ${(process.memoryUsage().rss / 1e6).toFixed(0)} MB`);

  if (progress.length > 0) {
    console.log(`progress updates: ${progress.length}`);
    const base = progress[0][0];
    for (const [time, line] of progress) {
      console.log(`  +${(time - base).toFixed(0).padStart(6)} ms  ${line.split('|').slice(1, 3).join(' | ')}`);
    }
  }
}

main().catch((error) => {
  console.error(error);
  process.exit(1);
});
