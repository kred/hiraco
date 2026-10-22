// wasm_session_harness.js  —  Drives the browser facing WebAssembly module
// from Node through the session API.
//
// A browser session keeps the RAW file (and the optional ORI companion)
// inside the module, so the conversion can be started without handing over
// the bytes again, and the stage settings can be changed between the
// preview and the conversion.  This harness converts a fixture with the
// session API, reports the progress the module emitted and writes the DNG
// out, so the result can be compared with the command line tool.
//
// Usage:
//   node tests/wasm_session_harness.js <hiraco-wasm.js> <input.ORF> <output.dng> [compression] [companion.ORI] [stage1_psf_sigma,stage1_nsr,...]
const fs = require('fs');
const path = require('path');

const STAGE_VALUE_COUNT = 8;

async function main() {
  const [, , modulePath, inputPath, outputPath, compressionArg, companionPath, overridesArg] = process.argv;
  if (!modulePath || !inputPath || !outputPath) {
    console.error('usage: node wasm_session_harness.js <hiraco-wasm.js> <input.ORF> <output.dng> [compression] [companion.ORI] [overrides]');
    process.exit(2);
  }
  const compression = compressionArg || 'deflate';

  const createModule = require(path.resolve(modulePath));

  // The progress of a running operation is reported on the module's print
  // channel, one "HIRACO_PROGRESS|..." line per update.
  const progress = [];
  const module = await createModule({
    print: (line) => {
      if (line.startsWith('HIRACO_PROGRESS|')) {
        progress.push(line);
      }
    },
  });

  // The module is compiled for wasm64, so the exported C functions take
  // their pointers and sizes as BigInt.  The Emscripten runtime helpers
  // (_malloc, the heap views) keep using plain Numbers.
  const raw = fs.readFileSync(inputPath);
  const rawPointer = module._malloc(raw.length);
  module.HEAPU8.set(raw, rawPointer);

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

  const sessionId = module._hiraco_wasm_open_session(
    BigInt(rawPointer),
    BigInt(raw.length),
    BigInt(module.stringToNewUTF8(path.basename(inputPath))),
    BigInt(companionPointer),
    BigInt(companionSize),
    BigInt(companionNamePointer));

  if (!sessionId) {
    console.error('could not open the session: ' + module.UTF8ToString(module._hiraco_wasm_get_last_error()));
    process.exit(1);
  }

  // The stage settings are passed as a flat list of doubles; NaN means
  // "not overridden", which leaves the setting on the value the image
  // asks for.
  const stageOverrides = new Float64Array(STAGE_VALUE_COUNT).fill(Number.NaN);
  if (overridesArg) {
    const values = overridesArg.split(',').map((value) => Number(value));
    if (values.length !== STAGE_VALUE_COUNT) {
      console.error(`expected ${STAGE_VALUE_COUNT} stage values, got ${values.length}`);
      process.exit(2);
    }
    values.forEach((value, index) => {
      stageOverrides[index] = value;
    });
  }
  const stagePointer = module._malloc(stageOverrides.byteLength);
  module.HEAPU8.set(new Uint8Array(stageOverrides.buffer), stagePointer);

  const outPointerPointer = module._malloc(8);
  const outSizePointer = module._malloc(8);
  const status = module._hiraco_wasm_convert_session(
    sessionId,
    BigInt(module.stringToNewUTF8(compression)),
    BigInt(stagePointer),
    BigInt(STAGE_VALUE_COUNT),
    BigInt(outPointerPointer),
    BigInt(outSizePointer));

  if (status !== 0) {
    console.error('conversion failed: ' + module.UTF8ToString(module._hiraco_wasm_get_last_error()));
    process.exit(1);
  }

  // The heap grows while the RAW file is processed, which replaces the heap
  // views, so every read goes through a view of the current buffer.
  const heap = () => new DataView(module.HEAPU8.buffer);
  const outPointer = Number(heap().getBigUint64(outPointerPointer, true));
  const outSize = Number(heap().getBigUint64(outSizePointer, true));
  if (outSize === 0) {
    console.error('the conversion returned no data');
    process.exit(1);
  }
  const dng = Buffer.from(module.HEAPU8.subarray(outPointer, outPointer + outSize));
  fs.writeFileSync(outputPath, dng);

  module._hiraco_wasm_free_buffer(BigInt(outPointer), BigInt(outSize));
  module._hiraco_wasm_close_session(sessionId);

  console.log(`ok: wrote ${dng.length} bytes to ${outputPath}`);
  console.log(`ok: ${progress.length} progress update(s)`);
  if (progress.length > 0) {
    console.log(`ok: last progress ${progress[progress.length - 1]}`);
  }
}

main().catch((error) => {
  console.error(error);
  process.exit(1);
});
