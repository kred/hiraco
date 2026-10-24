import { defineConfig, devices } from '@playwright/test';

/**
 * The end to end test drives the built application in a real browser.
 *
 * The preview server answers with the cross origin isolation headers the
 * module needs for its shared memory, and the test runs against the built
 * files in dist/, so it checks exactly what a static host would serve.
 */
export default defineConfig({
  testDir: './e2e',
  // coi.spec.ts runs against a server without isolation headers and has its
  // own configuration (playwright.coi.config.ts); the preview server used
  // here already provides the headers.
  testIgnore: 'coi.spec.ts',
  // The conversion of a full sized RAW file takes a while.
  timeout: 15 * 60 * 1000,
  expect: { timeout: 60_000 },
  fullyParallel: false,
  workers: 1,
  reporter: [['list']],
  use: {
    baseURL: 'http://127.0.0.1:4173',
    acceptDownloads: true,
    trace: 'off',
  },
  projects: [
    {
      name: 'chromium',
      use: { ...devices['Desktop Chrome'] },
    },
  ],
  webServer: {
    command: 'npm run preview -- --host 127.0.0.1 --port 4173 --strictPort',
    url: 'http://127.0.0.1:4173',
    reuseExistingServer: !process.env.CI,
    timeout: 120_000,
  },
});
