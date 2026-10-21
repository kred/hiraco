// wasm_convert_harness.js  —  Drives the browser facing WebAssembly module
// from Node.
//
// The `hiraco-wasm` module is built for the browser, but its conversion entry
// point takes the RAW data as a buffer and returns the DNG as a buffer, so
// the same module can be exercised from Node.  This harness is how the
// browser path is verified: it must produce exactly the same DNG as the
// command line tool.
//
// Usage:
//   node tests/wasm_convert_harness.js <hiraco-wasm.js> <input.ORF> <output.dng> [compression]
const fs = require('fs');
const path = require('path');

async function main() {
  const [, , modulePath, inputPath, outputPath, compressionArg] = process.argv;
  if (!modulePath || !inputPath || !outputPath) {
    console.error('usage: node wasm_convert_harness.js <hiraco-wasm.js> <input.ORF> <output.dng> [compression]');
    process.exit(2);
  }
  const compression = compressionArg || 'deflate';

  const createModule = require(path.resolve(modulePath));
  const module = await createModule();

  const raw = fs.readFileSync(inputPath);
  const rawName = path.basename(inputPath);

  // The export takes the RAW data as a pointer into the module's heap and
  // writes the DNG back through a pointer to a pointer.
  // The module is built for wasm64, so `size_t` and the pointers are 64 bit
  // integers and the WebAssembly ABI wants a BigInt for them.
  const rawPointer = module._malloc(BigInt(raw.length));
  module.HEAPU8.set(raw, rawPointer);

  const outPointerPointer = module._malloc(8);
  const outSizePointer = module._malloc(8);

  const status = module._hiraco_wasm_convert(
    BigInt(rawPointer),
    BigInt(raw.length),
    BigInt(module.stringToNewUTF8(rawName)),
    BigInt(module.stringToNewUTF8(compression)),
    BigInt(outPointerPointer),
    BigInt(outSizePointer));

  if (status !== 0) {
    const message = module._hiraco_wasm_get_last_error();
    console.error('conversion failed: ' + module.UTF8ToString(message));
    process.exit(1);
  }

  // The heap views Emscripten exposes do not include a 64 bit one, so the
  // pointer and the size are read straight out of the heap.
  const view = new DataView(module.HEAPU8.buffer);
  const outPointer = Number(view.getBigUint64(outPointerPointer, true));
  const outSize = Number(view.getBigUint64(outSizePointer, true));
  const dng = Buffer.from(module.HEAPU8.subarray(outPointer, outPointer + outSize));
  fs.writeFileSync(outputPath, dng);

  module._hiraco_wasm_free_buffer(BigInt(outPointer), BigInt(outSize));
  module._free(rawPointer);
  module._free(outPointerPointer);
  module._free(outSizePointer);

  console.log(`ok: wrote ${dng.length} bytes to ${outputPath}`);
}

main().catch((error) => {
  console.error(error);
  process.exit(1);
});
