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
    event.respondWith(
      fetch(event.request)
        .then(function (response) {
          if (response.status === 0) {
            return response;
          }

          var newHeaders = new Headers(response.headers);
          newHeaders.set('Cross-Origin-Embedder-Policy', 'require-corp');
          newHeaders.set('Cross-Origin-Opener-Policy', 'same-origin');

          // For responses without a body (204, 304, opaque) we can just
          // pass the original response with modified headers.
          if (response.status === 204 || response.status === 304 || response.type === 'opaque') {
            return new Response(null, {
              status: response.status,
              statusText: response.statusText,
              headers: newHeaders,
            });
          }

          // For other responses, read the body as ArrayBuffer first.
          // This avoids issues with ReadableStream in Firefox.
          return response.arrayBuffer()
            .then(function (buffer) {
              return new Response(buffer, {
                status: response.status,
                statusText: response.statusText,
                headers: newHeaders,
              });
            })
            .catch(function () {
              // If arrayBuffer() fails, fall back to the original body.
              return new Response(response.body, {
                status: response.status,
                statusText: response.statusText,
                headers: newHeaders,
              });
            });
        })
        .catch(function (e) {
          console.error('coi-serviceworker: fetch failed', e);
          return new Response('', { status: 500 });
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
