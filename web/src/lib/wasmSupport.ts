/**
 * The browser support the WebAssembly module needs.
 *
 * The module is compiled for wasm64 with a shared memory, so the browser has
 * to support 64 bit WebAssembly memory and the page has to be cross-origin
 * isolated, which is what makes the shared memory available.  Cross-origin
 * isolation is provided either by the host (COOP/COEP headers) or by the
 * service worker in public/coi-serviceworker.js.
 */

export interface BrowserSupport {
  supported: boolean;
  /** What is missing, in a sentence the user can act on. */
  reason: string | null;
  /** True while a service worker is being registered and the page reloaded. */
  registering: boolean;
}

/** Probes the browser for the features the module needs. */
export function checkBrowserSupport(): BrowserSupport {
  // The file:// protocol cannot provide cross-origin isolation or load
  // ES modules, so the app has to be served from a web server.
  if (window.location.protocol === 'file:') {
    return {
      supported: false,
      reason:
        'This page was opened directly from the file system. The WebAssembly ' +
        'module needs cross-origin isolation, which only a web server can ' +
        'provide, so please serve the dist/ folder (for example with ' +
        '"python3 -m http.server" or "npx serve") and open it through http://.',
      registering: false,
    };
  }

  // The service worker may still be registering on the first visit.  In that
  // case SharedArrayBuffer is not available yet, but the page will reload
  // shortly, so the unsupported banner must not be shown.
  if (!window.crossOriginIsolated && 'serviceWorker' in navigator) {
    // Give the registration a moment to complete before reporting failure.
    // The registration script in index.html handles the reload.
    return { supported: false, reason: null, registering: true };
  }

  if (typeof SharedArrayBuffer === 'undefined') {
    return {
      supported: false,
      reason:
        'This browser has no SharedArrayBuffer. The WebAssembly module needs it to ' +
        'hold the image memory, so a current version of Chrome, Edge, Firefox or ' +
        'Safari is required.',
      registering: false,
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
        'the shared memory the module needs. Either the server has to answer with ' +
        '"Cross-Origin-Opener-Policy: same-origin" and ' +
        '"Cross-Origin-Embedder-Policy: require-corp", or JavaScript has to be ' +
        'enabled so the cross-origin isolation service worker can register.',
      registering: false,
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
      registering: false,
    };
  }

  return { supported: true, reason: null, registering: false };
}
