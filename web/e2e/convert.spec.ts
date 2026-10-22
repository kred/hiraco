import { expect, test } from '@playwright/test';
import { execFileSync } from 'node:child_process';
import { existsSync, mkdirSync, statSync } from 'node:fs';
import path from 'node:path';
import { fileURLToPath } from 'node:url';

const here = path.dirname(fileURLToPath(import.meta.url));
const repoRoot = path.resolve(here, '..', '..');

/** The RAW file the test converts. */
const fixture = process.env.HIRACO_E2E_RAW ?? path.join(repoRoot, 'sample', '_8142490.ORI');

/** The Linear DNG the native build writes for the same file. */
const reference =
  process.env.HIRACO_E2E_REFERENCE ??
  path.join(repoRoot, 'build', 'reference', '_8142490-deflate.dng');

const outputDir = path.join(repoRoot, 'build', 'web-e2e');
const output = path.join(outputDir, 'browser.dng');

test('converts a RAW file in the browser and matches the native output', async ({
  page,
}) => {
  test.skip(
    !existsSync(fixture),
    `the RAW fixture ${fixture} is not part of the repository`,
  );

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

  // The module is a 64 bit WebAssembly module with a shared memory, so the
  // page has to be cross-origin isolated and the browser has to support it.
  await expect(page.locator('[data-module-status="ready"]')).toBeVisible({
    timeout: 120_000,
  });

  await page.getByLabel('RAW file').setInputFiles(fixture);
  await expect(page.getByTestId('preview-canvas')).toBeVisible({ timeout: 300_000 });

  const downloadPromise = page.waitForEvent('download', { timeout: 600_000 });
  await page.getByRole('button', { name: 'Convert', exact: true }).click();
  const download = await downloadPromise;

  mkdirSync(outputDir, { recursive: true });
  await download.saveAs(output);
  expect(statSync(output).size).toBeGreaterThan(1_000_000);

  // The browser has to write the same pixels as the native build.
  test.skip(
    !existsSync(reference),
    `the native reference ${reference} is not available`,
  );
  const python = process.env.HIRACO_PYTHON ?? 'python3';
  const report = execFileSync(
    python,
    [
      path.join(repoRoot, 'scripts', 'compare_dng.py'),
      reference,
      output,
      '--wasm',
    ],
    { encoding: 'utf8', maxBuffer: 32 * 1024 * 1024 },
  );
  expect(report).toContain('RESULT: PASS');
  expect(failures.join('\n')).toBe('');
});
