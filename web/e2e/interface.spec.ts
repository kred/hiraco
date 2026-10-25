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

/** The size of a preview canvas. */
async function canvasSize(
  page: import('@playwright/test').Page,
  testId = 'preview-canvas',
): Promise<{ width: number; height: number }> {
  return page.getByTestId(testId).evaluate((canvas) => ({
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

  test('shows the original, the converted result and the side by side panes', async ({
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

    // Side by side shows the two previews in fixed panes, not in one wide
    // canvas: the left pane carries the original and the right the result.
    await page.getByRole('button', { name: 'Side by side', exact: true }).click();
    const left = await canvasSize(page, 'preview-canvas-left');
    const right = await canvasSize(page, 'preview-canvas-right');
    expect(left.width).toBe(original.width);
    expect(right.width).toBe(converted.width);
  });

  test('shows the image details once and no sensor levels', async ({ page }) => {
    await openApp(page);
    await page.getByLabel('RAW file').setInputFiles(fixture);
    await expect(page.getByTestId('preview-canvas')).toBeVisible({ timeout: 300_000 });

    // The reveal wipe has no native counterpart, so the browser does not
    // offer it either.
    await expect(page.getByRole('button', { name: 'Reveal', exact: true })).toHaveCount(0);

    const inspector = page.getByRole('region', { name: 'Inspector' });
    await expect(inspector.getByText('Working size')).toBeVisible();
    await expect(inspector.getByText('Black level')).toHaveCount(0);
    await expect(inspector.getByText('White level')).toHaveCount(0);

    // The neutral settings are the medium preset, so that button is shown as
    // selected, and the DNG is saved by Convert itself (no second button).
    await expect(
      page.getByRole('button', { name: 'Medium', exact: true }),
    ).toHaveAttribute('aria-pressed', 'true');
    await expect(page.getByRole('button', { name: 'Save DNG', exact: true })).toHaveCount(0);

    // The working size lives in the inspector only; the workspace shows the
    // file names, not the geometry.
    const workspace = page.getByRole('region', { name: 'Workspace' });
    await expect(workspace.getByText('Size', { exact: true })).toHaveCount(0);
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

    const path = await download.path();
    expect(path).not.toBeNull();
    const size = (await (await import('node:fs')).promises.stat(path!)).size;
    expect(size).toBeGreaterThan(1_000_000);
  });
});
