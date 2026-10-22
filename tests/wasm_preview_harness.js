// wasm_preview_harness.js  —  Drives the browser facing WebAssembly module
// from Node and checks the preview side of the bridge.
//
// The `hiraco-wasm` module is built for the browser, but every call it
// offers takes and returns plain buffers, so the same module can be
// exercised from Node.  This harness is how the browser preview path is
// verified: it opens a session for a RAW file, asks for the image size, the
// metadata and both previews, and fails when any of them comes back empty.
//
// Usage:
//   node tests/wasm_preview_harness.js <hiraco-wasm.js> <input.ORF> [companion.ORI] [maxDimension]
const fs = require('fs');
const path = require('path');

const STAGE_VALUE_COUNT = 8;

async function main() {
  const [, , modulePath, inputPath, companionPath, maxDimensionArg] = process.argv;
  if (!modulePath || !inputPath) {
    console.error('usage: node wasm_preview_harness.js <hiraco-wasm.js> <input.ORF> [companion.ORI] [maxDimension]');
    process.exit(2);
  }
  const maxDimension = Number(maxDimensionArg || 512);

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

  // The heap grows while the RAW file is processed, which replaces the heap
  // views, so every read goes through a view of the current buffer.
  const heap = () => new DataView(module.HEAPU8.buffer);
  const readU64 = (pointer) => Number(heap().getBigUint64(pointer, true));

  const widthPointer = module._malloc(4);
  const heightPointer = module._malloc(4);
  const status = module._hiraco_wasm_get_image_size(
    sessionId, BigInt(widthPointer), BigInt(heightPointer));
  if (status !== 0) {
    console.error('could not read the image size: ' + module.UTF8ToString(module._hiraco_wasm_get_last_error()));
    process.exit(1);
  }
  const width = heap().getUint32(widthPointer, true);
  const height = heap().getUint32(heightPointer, true);
  if (width === 0 || height === 0) {
    console.error(`the image size is empty: ${width}x${height}`);
    process.exit(1);
  }

  const metadataPointer = module._malloc(64 * 1024);
  const metadataLength = module._hiraco_wasm_get_metadata_json(
    sessionId, BigInt(metadataPointer), BigInt(64 * 1024));
  if (metadataLength <= 0) {
    console.error('the metadata is empty: ' + module.UTF8ToString(module._hiraco_wasm_get_last_error()));
    process.exit(1);
  }
  const metadata = module.UTF8ToString(metadataPointer, metadataLength);
  const parsed = JSON.parse(metadata);
  if (!parsed || typeof parsed !== 'object') {
    console.error('the metadata is not a JSON object');
    process.exit(1);
  }

  const previewPointerPointer = module._malloc(8);
  const previewSizePointer = module._malloc(8);
  const previewWidthPointer = module._malloc(4);
  const previewHeightPointer = module._malloc(4);
  const maskPointerPointer = module._malloc(8);
  const maskSizePointer = module._malloc(8);

  const originalStatus = module._hiraco_wasm_get_original_preview(
    sessionId,
    maxDimension,
    BigInt(previewPointerPointer),
    BigInt(previewSizePointer),
    BigInt(previewWidthPointer),
    BigInt(previewHeightPointer));
  if (originalStatus !== 0) {
    console.error('could not render the original preview: ' + module.UTF8ToString(module._hiraco_wasm_get_last_error()));
    process.exit(1);
  }
  const originalPointer = readU64(previewPointerPointer);
  const originalSize = readU64(previewSizePointer);
  const originalWidth = heap().getUint32(previewWidthPointer, true);
  const originalHeight = heap().getUint32(previewHeightPointer, true);
  if (originalSize === 0 || originalWidth === 0 || originalHeight === 0) {
    console.error(`the original preview is empty: ${originalSize} bytes, ${originalWidth}x${originalHeight}`);
    process.exit(1);
  }
  if (originalSize !== originalWidth * originalHeight * 4) {
    console.error(`the original preview is not RGBA: ${originalSize} bytes for ${originalWidth}x${originalHeight}`);
    process.exit(1);
  }
  module._hiraco_wasm_free_buffer(BigInt(originalPointer), BigInt(originalSize));

  // Every stage setting is passed as NaN, which means "not overridden".
  const stageOverrides = new Float64Array(STAGE_VALUE_COUNT).fill(Number.NaN);
  const stagePointer = module._malloc(stageOverrides.byteLength);
  module.HEAPU8.set(new Uint8Array(stageOverrides.buffer), stagePointer);

  const resolvedPointer = module._malloc(STAGE_VALUE_COUNT * 8);
  const resolvedStatus = module._hiraco_wasm_get_resolved_stage_settings(
    sessionId,
    BigInt(stagePointer),
    BigInt(STAGE_VALUE_COUNT),
    BigInt(resolvedPointer),
    BigInt(STAGE_VALUE_COUNT));
  if (resolvedStatus !== 0) {
    console.error('could not resolve the stage settings: ' + module.UTF8ToString(module._hiraco_wasm_get_last_error()));
    process.exit(1);
  }
  const resolved = [];
  for (let index = 0; index < STAGE_VALUE_COUNT; ++index) {
    resolved.push(heap().getFloat64(resolvedPointer + index * 8, true));
  }

  const convertedStatus = module._hiraco_wasm_get_converted_preview(
    sessionId,
    maxDimension,
    BigInt(stagePointer),
    BigInt(STAGE_VALUE_COUNT),
    BigInt(previewPointerPointer),
    BigInt(previewSizePointer),
    BigInt(previewWidthPointer),
    BigInt(previewHeightPointer),
    BigInt(maskPointerPointer),
    BigInt(maskSizePointer));
  if (convertedStatus !== 0) {
    console.error('could not render the converted preview: ' + module.UTF8ToString(module._hiraco_wasm_get_last_error()));
    process.exit(1);
  }
  const convertedPointer = readU64(previewPointerPointer);
  const convertedSize = readU64(previewSizePointer);
  const convertedWidth = heap().getUint32(previewWidthPointer, true);
  const convertedHeight = heap().getUint32(previewHeightPointer, true);
  if (convertedSize === 0 || convertedWidth === 0 || convertedHeight === 0) {
    console.error(`the converted preview is empty: ${convertedSize} bytes, ${convertedWidth}x${convertedHeight}`);
    process.exit(1);
  }
  if (convertedSize !== convertedWidth * convertedHeight * 4) {
    console.error(`the converted preview is not RGBA: ${convertedSize} bytes for ${convertedWidth}x${convertedHeight}`);
    process.exit(1);
  }
  const maskPointer = readU64(maskPointerPointer);
  const maskSize = readU64(maskSizePointer);
  if (maskSize > 0 && maskSize !== convertedWidth * convertedHeight) {
    console.error(`the recovered pixel mask does not match the preview: ${maskSize} bytes for ${convertedWidth}x${convertedHeight}`);
    process.exit(1);
  }
  module._hiraco_wasm_free_buffer(BigInt(convertedPointer), BigInt(convertedSize));
  if (maskSize > 0) {
    module._hiraco_wasm_free_buffer(BigInt(maskPointer), BigInt(maskSize));
  }

  module._hiraco_wasm_close_session(sessionId);

  console.log(`ok: image ${width}x${height}`);
  console.log(`ok: metadata ${JSON.stringify({
    make: parsed.make,
    model: parsed.model,
    width: parsed.width,
    height: parsed.height,
    has_highlight_recovery: parsed.has_highlight_recovery,
  })}`);
  console.log(`ok: original preview ${originalWidth}x${originalHeight} (${originalSize} bytes)`);
  console.log(`ok: converted preview ${convertedWidth}x${convertedHeight} (${convertedSize} bytes, mask ${maskSize} bytes)`);
  console.log(`ok: resolved settings ${resolved.map((value) => value.toFixed(4)).join(' ')}`);
  console.log(`ok: ${progress.length} progress update(s)`);
}

main().catch((error) => {
  console.error(error);
  process.exit(1);
});
