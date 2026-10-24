import { defineConfig, devices } from '@playwright/test';

/**
 * The cross-origin isolation fallback test.
 *
 * Unlike the main configuration, the server started here serves the built
 * files without COOP/COEP headers, exactly like GitHub Pages.  The service
 * worker in public/coi-serviceworker.js has to add those headers and reload
 * the page once; the test then checks that the page really became isolated
 * and that the WebAssembly module loaded.
 */
export default defineConfig({
  testDir: './e2e',
  testMatch: 'coi.spec.ts',
  timeout: 2 * 60 * 1000,
  expect: { timeout: 60_000 },
  fullyParallel: false,
  workers: 1,
  reporter: [['list']],
  use: {
    baseURL: 'http://127.0.0.1:4174',
    trace: 'off',
  },
  projects: [
    {
      name: 'chromium',
      use: { ...devices['Desktop Chrome'] },
    },
  ],
  webServer: {
    command: 'node e2e/coi.server.mjs',
    url: 'http://127.0.0.1:4174',
    reuseExistingServer: !process.env.CI,
    timeout: 30_000,
  },
});
