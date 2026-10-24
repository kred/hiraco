// hiraco-worker.js  —  Runs the WebAssembly module off the main thread.
//
// The module is built for the browser, so it is loaded here as a classic
// worker script: the glue is pulled in with importScripts, which keeps the
// script name of the worker pointing at this file.  The module starts its
// own worker threads from that name, so this file has to stay next to
// hiraco-wasm.js.
//
// The protocol is one message in, one message out.  The other end of it
// lives in src/lib/wasmClient.ts.
//
// The module is compiled for wasm64, so the exported C functions take their
// pointers and sizes as BigInt while the Emscripten runtime helpers keep
// using plain Numbers.

importScripts('./hiraco-wasm.js');

// The module starts its own threads from the script name of this worker,
// so the module also loads this file inside those threads.  There the glue
// installs its own message handler, so the driver below has to stay out of
// the way.
const IS_PTHREAD_WORKER = typeof self.name === 'string' && self.name === 'em-pthread';

const STAGE_VALUE_COUNT = 8;
const PROGRESS_PREFIX = 'HIRACO_PROGRESS|';

let module = null;
let sessionId = 0;
let currentRequestId = 0;

function reply(message) {
  self.postMessage({ id: currentRequestId, ...message });
}

function fail(error) {
  const message = error && error.message ? error.message : String(error);
  reply({
    type: 'error',
    message: message,
    stack: error && error.stack ? String(error.stack) : '',
  });
}

// The heap grows while a RAW file is processed, which replaces the heap
// views, so every read goes through a view of the current buffer.
function heap() {
  return new DataView(module.HEAPU8.buffer);
}

// In a wasm64 module a pointer is a 64 bit value, so the exported
// functions that return one hand it over as a BigInt.  The runtime
// helpers work with plain numbers, so the pointers are converted here.
function toPointer(value) {
  return typeof value === 'bigint' ? Number(value) : value;
}

function readSize(pointer) {
  return Number(heap().getBigUint64(pointer, true));
}

function writeBytes(bytes) {
  const pointer = module._malloc(bytes.length);
  module.HEAPU8.set(bytes, pointer);
  return pointer;
}

// Releases scratch heap allocations once the native side has consumed them.
// A zero pointer is skipped: that allocation never happened or the value is
// owned elsewhere.
function freeScratch(...pointers) {
  for (const pointer of pointers) {
    if (pointer) {
      module._free(pointer);
    }
  }
}

function copyOut(pointer, size) {
  return module.HEAPU8.slice(pointer, pointer + size).buffer;
}

function stageOverrides(overrides) {
  const values = new Float64Array(STAGE_VALUE_COUNT).fill(Number.NaN);
  if (overrides) {
    for (let index = 0; index < STAGE_VALUE_COUNT && index < overrides.length; ++index) {
      const value = overrides[index];
      values[index] = typeof value === 'number' ? value : Number.NaN;
    }
  }
  const pointer = module._malloc(values.byteLength);
  module.HEAPU8.set(new Uint8Array(values.buffer), pointer);
  return pointer;
}

async function init() {
  module = await self.hiracoWasm({
    print: (line) => {
      if (!line.startsWith(PROGRESS_PREFIX)) {
        return;
      }
      const parts = line.split('|');
      reply({
        type: 'progress',
        phase: parts[1] || '',
        fraction: Number(parts[2]) || 0,
        message: parts.slice(3).join('|'),
      });
    },
    printErr: (line) => {
      if (line.startsWith(PROGRESS_PREFIX)) {
        return;
      }
      reply({ type: 'log', message: line });
    },
    locateFile: (path) => new URL(path, self.location.href).href,
  });

  const statusPointer = toPointer(module._hiraco_wasm_get_dng_status());
  reply({
    type: 'ready',
    dngSdkEnabled: module._hiraco_wasm_is_dng_sdk_enabled() !== 0,
    dngStatus: statusPointer ? module.UTF8ToString(statusPointer) : '',
  });
}

function openSession(request) {
  const raw = new Uint8Array(request.raw);
  const rawPointer = writeBytes(raw);
  const rawNamePointer = module.stringToNewUTF8(request.rawName || 'input.raw');

  let companionPointer = 0;
  let companionSize = 0;
  let companionNamePointer = 0;
  if (request.companion) {
    const companion = new Uint8Array(request.companion);
    companionSize = companion.length;
    companionPointer = writeBytes(companion);
    companionNamePointer = module.stringToNewUTF8(request.companionName || 'companion.ori');
  }

  const widthPointer = module._malloc(4);
  const heightPointer = module._malloc(4);
  const metadataPointer = module._malloc(64 * 1024);
  try {
    // open_session stages the input in the virtual file system and copies
    // the names, so the buffers above are only handed over and are released
    // afterwards, whatever the call does.
    const id = module._hiraco_wasm_open_session(
      BigInt(rawPointer),
      BigInt(raw.length),
      BigInt(rawNamePointer),
      BigInt(companionPointer),
      BigInt(companionSize),
      BigInt(companionNamePointer));

    if (!id) {
      throw new Error(module.UTF8ToString(toPointer(module._hiraco_wasm_get_last_error())));
    }
    if (sessionId) {
      module._hiraco_wasm_close_session(sessionId);
    }
    sessionId = id;

    module._hiraco_wasm_get_image_size(sessionId, BigInt(widthPointer), BigInt(heightPointer));

    const metadataLength = module._hiraco_wasm_get_metadata_json(sessionId, BigInt(metadataPointer), BigInt(64 * 1024));
    const metadata = metadataLength > 0 ? JSON.parse(module.UTF8ToString(metadataPointer, metadataLength)) : {};

    reply({
      type: 'opened',
      sessionId: Number(sessionId),
      width: heap().getUint32(widthPointer, true),
      height: heap().getUint32(heightPointer, true),
      metadata: metadata,
    });
  } finally {
    freeScratch(rawPointer, rawNamePointer, companionPointer, companionNamePointer,
                widthPointer, heightPointer, metadataPointer);
  }
}

function renderPreview(request) {
  const pixelsPointer = module._malloc(8);
  const sizePointer = module._malloc(8);
  const widthPointer = module._malloc(4);
  const heightPointer = module._malloc(4);
  const maskPointer = module._malloc(8);
  const maskSizePointer = module._malloc(8);
  const overridesPointer = stageOverrides(request.overrides);

  try {
    let status = 0;
    if (request.kind === 'original') {
      status = module._hiraco_wasm_get_original_preview(
        sessionId,
        request.maxDimension,
        BigInt(pixelsPointer),
        BigInt(sizePointer),
        BigInt(widthPointer),
        BigInt(heightPointer));
    } else {
      status = module._hiraco_wasm_get_converted_preview(
        sessionId,
        request.maxDimension,
        BigInt(overridesPointer),
        BigInt(STAGE_VALUE_COUNT),
        BigInt(pixelsPointer),
        BigInt(sizePointer),
        BigInt(widthPointer),
        BigInt(heightPointer),
        BigInt(maskPointer),
        BigInt(maskSizePointer));
    }
    if (status !== 0) {
      throw new Error(module.UTF8ToString(toPointer(module._hiraco_wasm_get_last_error())));
    }

    const pixelAddress = readSize(pixelsPointer);
    const pixelBytes = readSize(sizePointer);
    const width = heap().getUint32(widthPointer, true);
    const height = heap().getUint32(heightPointer, true);
    const message = {
      type: 'preview',
      kind: request.kind,
      width: width,
      height: height,
      pixels: copyOut(pixelAddress, pixelBytes),
    };

    if (request.kind === 'converted') {
      const maskAddress = readSize(maskPointer);
      const maskBytes = readSize(maskSizePointer);
      if (maskBytes > 0) {
        message.mask = copyOut(maskAddress, maskBytes);
      }
      // The bridge zeroes the mask pointers when there is no mask, and the
      // freed byte count is zero there, so nothing else is released.
      module._hiraco_wasm_free_buffer(BigInt(maskAddress), BigInt(maskBytes));
    }

    module._hiraco_wasm_free_buffer(BigInt(pixelAddress), BigInt(pixelBytes));

    reply(message);
  } finally {
    freeScratch(pixelsPointer, sizePointer, widthPointer, heightPointer,
                maskPointer, maskSizePointer, overridesPointer);
  }
}

function resolveSettings(request) {
  const overridesPointer = stageOverrides(request.overrides);
  const valuesPointer = module._malloc(STAGE_VALUE_COUNT * 8);
  try {
    const status = module._hiraco_wasm_get_resolved_stage_settings(
      sessionId,
      BigInt(overridesPointer),
      BigInt(STAGE_VALUE_COUNT),
      BigInt(valuesPointer),
      BigInt(STAGE_VALUE_COUNT));
    if (status !== 0) {
      throw new Error(module.UTF8ToString(toPointer(module._hiraco_wasm_get_last_error())));
    }
    const values = [];
    for (let index = 0; index < STAGE_VALUE_COUNT; ++index) {
      values.push(heap().getFloat64(valuesPointer + index * 8, true));
    }
    reply({ type: 'settings', values: values });
  } finally {
    freeScratch(overridesPointer, valuesPointer);
  }
}

function convert(request) {
  const overridesPointer = stageOverrides(request.overrides);
  const compressionPointer = module.stringToNewUTF8(request.compression || 'deflate');
  const outPointerPointer = module._malloc(8);
  const outSizePointer = module._malloc(8);
  try {
    const status = module._hiraco_wasm_convert_session(
      sessionId,
      BigInt(compressionPointer),
      BigInt(overridesPointer),
      BigInt(STAGE_VALUE_COUNT),
      BigInt(outPointerPointer),
      BigInt(outSizePointer));
    if (status !== 0) {
      throw new Error(module.UTF8ToString(toPointer(module._hiraco_wasm_get_last_error())));
    }
    const pointer = readSize(outPointerPointer);
    const size = readSize(outSizePointer);
    reply({ type: 'converted', dng: copyOut(pointer, size), bytes: size });
    module._hiraco_wasm_free_buffer(BigInt(pointer), BigInt(size));
  } finally {
    freeScratch(overridesPointer, compressionPointer, outPointerPointer, outSizePointer);
  }
}

function writerSummary(request) {
  const summaryPointer = module._malloc(4096);
  const compressionPointer = module.stringToNewUTF8(request.compression || 'deflate');
  try {
    const length = module._hiraco_wasm_get_writer_summary(
      BigInt(summaryPointer), BigInt(4096), BigInt(compressionPointer));
    reply({ type: 'summary', text: length > 0 ? module.UTF8ToString(summaryPointer, length) : '' });
  } finally {
    freeScratch(summaryPointer, compressionPointer);
  }
}

function installDriver() {
self.onmessage = async (event) => {
  const request = event.data || {};
  currentRequestId = request.id ?? 0;
  try {
    switch (request.type) {
      case 'init':
        await init();
        break;
      case 'open':
        openSession(request);
        break;
      case 'preview':
        renderPreview(request);
        break;
      case 'settings':
        resolveSettings(request);
        break;
      case 'summary':
        writerSummary(request);
        break;
      case 'convert':
        convert(request);
        break;
      case 'cancel':
        // The conversion runs inside the module, so the request only
        // reaches it between two operations.
        if (sessionId) {
          module._hiraco_wasm_request_cancel(sessionId);
        }
        reply({ type: 'cancelled' });
        break;
      case 'close':
        if (sessionId) {
          module._hiraco_wasm_close_session(sessionId);
          sessionId = 0;
        }
        reply({ type: 'closed' });
        break;
      default:
        fail(new Error(`unknown request: ${request.type}`));
        break;
    }
  } catch (error) {
    fail(error);
  }
};
}

if (!IS_PTHREAD_WORKER) {
  installDriver();
}
