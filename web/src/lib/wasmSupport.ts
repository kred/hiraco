/**
 * The browser support the WebAssembly module needs.
 *
 * The module is compiled for wasm64 with a shared memory, so the browser has
 * to support 64 bit WebAssembly memory and the page has to be cross-origin
 * isolated, which is what makes the shared memory available.
 */

export interface BrowserSupport {
  supported: boolean;
  /** What is missing, in a sentence the user can act on. */
  reason: string | null;
}

/** Probes the browser for the features the module needs. */
export function checkBrowserSupport(): BrowserSupport {
  if (typeof SharedArrayBuffer === 'undefined') {
    return {
      supported: false,
      reason:
        'This browser has no SharedArrayBuffer. The WebAssembly module needs it to ' +
        'hold the image memory, so a current version of Chrome, Edge, Firefox or ' +
        'Safari is required.',
    };
  }

  const isolated = typeof globalThis.crossOriginIsolated === 'boolean'
    ? globalThis.crossOriginIsolated
    : false;
  if (!isolated) {
    return {
      supported: false,
      reason:
        'This page is not cross-origin isolated, so the browser does not hand out ' +
        'the shared memory the module needs. The server has to answer with ' +
        '"Cross-Origin-Opener-Policy: same-origin" and ' +
        '"Cross-Origin-Embedder-Policy: require-corp".',
    };
  }

  try {
    // The module creates its memory with a 64 bit address space and shares
    // it with its own threads, so the browser has to know that shape.
    const descriptor = {
      initial: 1n,
      maximum: 1n,
      shared: true,
      address: 'i64',
    } as unknown as WebAssembly.MemoryDescriptor;
    const memory = new WebAssembly.Memory(descriptor);
    if (memory.buffer.byteLength === 0) {
      throw new Error('the memory is empty');
    }
  } catch (error) {
    return {
      supported: false,
      reason:
        'This browser cannot create a shared 64 bit WebAssembly memory, which the ' +
        'module needs to hold a full sized image. A current version of Chrome, ' +
        'Edge, Firefox or Safari is required.',
    };
  }

  return { supported: true, reason: null };
}
