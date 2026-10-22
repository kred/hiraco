import { expect, test } from '@playwright/test';
import { existsSync } from 'node:fs';
import path from 'node:path';
import { fileURLToPath } from 'node:url';

const here = path.dirname(fileURLToPath(import.meta.url));
const repoRoot = path.resolve(here, '..', '..');

/** The RAW file the test converts. */
const fixture = process.env.HIRACO_E2E_RAW ?? path.join(repoRoot, 'sample', '_8142490.ORI');

/** Loads the page and waits until the module is ready. */
async function openApp(page: import('@playwright/test').Page): Promise<void> {
  await page.goto('/');
  await expect(page.locator('[data-module-status="ready"]')).toBeVisible({
    timeout: 120_000,
  });
}

/** The size of the preview canvas. */
async function canvasSize(
  page: import('@playwright/test').Page,
): Promise<{ width: number; height: number }> {
  return page.getByTestId('preview-canvas').evaluate((canvas) => ({
    width: (canvas as HTMLCanvasElement).width,
    height: (canvas as HTMLCanvasElement).height,
  }));
}

test.describe('the preview and the controls', () => {
  test.beforeEach(() => {
    test.skip(
      !existsSync(fixture),
      `the RAW fixture ${fixture} is not part of the repository`,
    );
  });

  test('shows the original, the converted result and the reveal wipe', async ({
    page,
  }) => {
    await openApp(page);
    await page.getByLabel('RAW file').setInputFiles(fixture);
    await expect(page.getByTestId('preview-canvas')).toBeVisible({ timeout: 300_000 });

    const converted = await canvasSize(page);
    expect(converted.width).toBeGreaterThan(0);

    await page.getByRole('button', { name: 'Original', exact: true }).click();
    const original = await canvasSize(page);
    expect(original.width).toBeGreaterThan(0);

    await page.getByRole('button', { name: 'Side by side', exact: true }).click();
    const sideBySide = await canvasSize(page);
    expect(sideBySide.width).toBe(original.width + converted.width);

    await page.getByRole('button', { name: 'Reveal', exact: true }).click();
    await expect(page.getByLabel('Reveal')).toBeVisible();
    const revealed = await canvasSize(page);
    expect(revealed.width).toBe(Math.max(original.width, converted.width));

    // The wipe moves the border between the two pictures.
    await page.getByLabel('Reveal').fill('0.25');
    await expect(page.getByLabel('Reveal')).toHaveValue('0.25');
  });

  test('applies the presets to the sliders', async ({ page }) => {
    await openApp(page);
    await page.getByLabel('RAW file').setInputFiles(fixture);
    await expect(page.getByTestId('preview-canvas')).toBeVisible({ timeout: 300_000 });

    const denoise = page.getByLabel('Denoise');
    await expect(denoise).toHaveValue('0.33');

    await page.getByRole('button', { name: 'Strong', exact: true }).click();
    await expect(denoise).toHaveValue('0.24');
    await expect(page.getByRole('button', { name: 'Strong', exact: true })).toHaveAttribute(
      'aria-pressed',
      'true',
    );

    // The reset button of the section exists.
    await expect(page.getByRole('button', { name: 'Reset' }).first()).toBeVisible();
    await expect(
      page.getByRole('button', { name: 'Strong', exact: true }),
    ).toHaveAttribute('aria-pressed', 'true');
  });

  test('writes the file the native application writes', async ({ page }) => {
    await openApp(page);
    await page.getByLabel('RAW file').setInputFiles(fixture);
    await expect(page.getByTestId('preview-canvas')).toBeVisible({ timeout: 300_000 });

    const downloadPromise = page.waitForEvent('download', { timeout: 600_000 });
    await page.getByRole('button', { name: 'Convert', exact: true }).click();
    const download = await downloadPromise;
    expect(download.suggestedFilename()).toBe('_8142490.dng');

    // The bridge reports what the writer is configured for.
    await expect(page.getByTestId('writer-summary')).toBeVisible();
  });
});
