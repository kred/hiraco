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

if (typeof window === 'undefined') {
  self.addEventListener('install', function () {
    self.skipWaiting();
  });

  self.addEventListener('activate', function (event) {
    event.waitUntil(self.clients.claim());
  });

  self.addEventListener('fetch', function (event) {
    if (event.request.cache === 'only-if-cached' && event.request.mode !== 'same-origin') {
      return;
    }

    event.respondWith(
      fetch(event.request).then(function (response) {
        if (response.status === 0) {
          return response;
        }

        var newHeaders = new Headers(response.headers);
        newHeaders.set('Cross-Origin-Embedder-Policy', 'require-corp');
        newHeaders.set('Cross-Origin-Opener-Policy', 'same-origin');

        // Firefox can fail to install the worker if the body is passed as a
        // ReadableStream, so read it as an ArrayBuffer first.
        return response.arrayBuffer().then(function (buffer) {
          return new Response(buffer, {
            status: response.status,
            statusText: response.statusText,
            headers: newHeaders,
          });
        });
      }),
    );
  });
} else {
  (async function () {
    if (window.crossOriginIsolated) {
      return;
    }

    if (!('serviceWorker' in navigator)) {
      return;
    }

    try {
      var registration = await navigator.serviceWorker.register('coi-serviceworker.js');

      if (registration.installing) {
        await new Promise(function (resolve) {
          registration.installing.addEventListener('statechange', function () {
            if (registration.installing.state === 'activated') {
              resolve();
            }
          });
        });
      }

      window.location.reload();
    } catch (e) {
      console.error('coi-serviceworker: registration failed', e);
    }
  })();
}
