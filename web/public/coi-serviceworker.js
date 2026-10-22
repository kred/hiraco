// Cross-origin isolation service worker.
//
// SharedArrayBuffer, which the 64 bit WebAssembly module needs for its shared
// memory, is only available to documents that are cross-origin isolated, i.e.
// documents that are served with
//   Cross-Origin-Opener-Policy: same-origin
//   Cross-Origin-Embedder-Policy: require-corp
// A static host does not send those headers, so this service worker adds
// them to every response it serves.  On the first visit the worker is
// registered and the page is reloaded once; after that the page is isolated
// and the module can load.
//
// Based on the well-known coi-serviceworker pattern by Guido Zuidhof.

const COOP = 'Cross-Origin-Opener-Policy';
const COEP = 'Cross-Origin-Embedder-Policy';

if (typeof window === 'undefined') {
  // Running as a service worker.
  self.addEventListener('install', () => {
    self.skipWaiting();
  });

  self.addEventListener('activate', (event) => {
    event.waitUntil(self.clients.claim());
  });

  self.addEventListener('fetch', (event) => {
    const request = event.request;
    if (request.cache === 'only-if-cached' && request.mode !== 'same-origin') {
      return;
    }
    event.respondWith(
      fetch(request)
        .then((response) => {
          if (response.status === 0) {
            return response;
          }
          const headers = new Headers(response.headers);
          headers.set(COOP, 'same-origin');
          headers.set(COEP, 'require-corp');
          return new Response(response.body, {
            status: response.status,
            statusText: response.statusText,
            headers,
          });
        })
        .catch((error) => {
          console.error('coi-serviceworker: fetch failed', error);
          throw error;
        }),
    );
  });
} else {
  // Running as a script in the page.  Register the worker and reload once.
  (async () => {
    if (window.crossOriginIsolated) {
      // Already isolated: nothing to do.
      return;
    }
    if (!('serviceWorker' in navigator)) {
      // Service workers are unavailable (for example on file://).  The
      // application shows its own unsupported message in that case.
      return;
    }
    try {
      const registration = await navigator.serviceWorker.register(
        './coi-serviceworker.js',
        { scope: './' },
      );
      // Wait until the worker is active before reloading.
      if (registration.installing) {
        await new Promise((resolve) => {
          registration.installing.addEventListener('statechange', (event) => {
            if (event.target.state === 'activated') {
              resolve();
            }
          });
        });
      } else if (registration.waiting) {
        await new Promise((resolve) => {
          registration.waiting.addEventListener('statechange', (event) => {
            if (event.target.state === 'activated') {
              resolve();
            }
          });
        });
      }
      // Reload so the new headers take effect.
      window.location.reload();
    } catch (error) {
      console.error('coi-serviceworker: registration failed', error);
    }
  })();
}
