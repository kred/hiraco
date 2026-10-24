import { expect, test } from '@playwright/test';

// This test runs against a static file server that deliberately sends no
// COOP/COEP headers, the same situation GitHub Pages puts the app in.  The
// service worker in public/coi-serviceworker.js has to install, reload the
// page once and serve the isolation headers from then on.  Without that the
// browser refuses the shared WebAssembly memory and every worker message
// fails with a DataCloneError.
test('the service worker makes a plain static host cross-origin isolated', async ({
  page,
}) => {
  const failures: string[] = [];
  page.on('console', (message) => {
    if (message.type() === 'error') {
      failures.push(message.text());
    }
  });
  page.on('pageerror', (error) => {
    failures.push(error.message);
  });

  await page.goto('/');

  // "ready" is only reported after the module initialized, which needs the
  // shared 64 bit memory, which is only handed out to isolated documents.
  await expect(page.locator('[data-module-status="ready"]')).toBeVisible({
    timeout: 90_000,
  });

  const environment = await page.evaluate(() => ({
    isolated: window.crossOriginIsolated,
    sharedArrayBuffer: typeof SharedArrayBuffer === 'function',
    controlled: navigator.serviceWorker.controller !== null,
  }));
  expect(environment.isolated).toBe(true);
  expect(environment.sharedArrayBuffer).toBe(true);
  expect(environment.controlled).toBe(true);

  // A stuck registration used to throw inside the state listener; the page
  // must reach isolation without any uncaught error on the way.
  expect(failures).toEqual([]);
});
