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
  // The first visit registers the worker and reloads the page once: the
  // current document was loaded without the isolation headers, so only a
  // document served through the worker can be cross-origin isolated.
  var reloadKey = 'coi-serviceworker-reloaded';

  (async function () {
    if (window.crossOriginIsolated) {
      // Isolation is in place, so a guard left behind by an earlier attempt
      // in this tab can be dropped again.
      try {
        window.sessionStorage.removeItem(reloadKey);
      } catch (e) {
        // Ignored: the guard is only an optimization.
      }
      return;
    }

    if (!('serviceWorker' in navigator)) {
      return;
    }

    try {
      var guarded = false;
      try {
        guarded = window.sessionStorage.getItem(reloadKey) !== null;
      } catch (e) {
        // Treat a storage failure as "no guard"; the reload is still tried.
      }
      if (guarded) {
        console.error(
          'coi-serviceworker: the page is still not cross-origin isolated after ' +
          'a reload; the service worker cannot add the isolation headers.');
        return;
      }

      var registration = await navigator.serviceWorker.register('./coi-serviceworker.js');

      // registration.installing stops referring to the worker as soon as it
      // leaves the installing state, so the listener has to keep its own
      // reference.  Reading registration.installing inside the listener threw
      // once the worker activated, which left the page unisolated forever.
      var worker = registration.installing || registration.waiting;
      if (worker && worker.state !== 'activated') {
        await new Promise(function (resolve) {
          worker.addEventListener('statechange', function () {
            if (worker.state === 'activated' || worker.state === 'redundant') {
              resolve();
            }
          });
        });
      }

      // The worker claims the page while it activates.  Waiting for that
      // keeps the reload from happening before the worker can serve the
      // document, with at most a short timeout if no claim arrives.
      if (!navigator.serviceWorker.controller) {
        await new Promise(function (resolve) {
          var timer = setTimeout(resolve, 3000);
          navigator.serviceWorker.addEventListener('controllerchange', function () {
            clearTimeout(timer);
            resolve();
          }, { once: true });
          if (navigator.serviceWorker.controller) {
            clearTimeout(timer);
            resolve();
          }
        });
      }

      try {
        window.sessionStorage.setItem(reloadKey, '1');
      } catch (e) {
        // Without storage a reload loop cannot be detected; one reload is
        // still better than leaving the page unusable.
      }
      window.location.reload();
    } catch (e) {
      console.error('coi-serviceworker: registration failed', e);
    }
  })();
}
