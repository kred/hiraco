import { expect, test } from '@playwright/test';
import { existsSync } from 'node:fs';
import path from 'node:path';
import { fileURLToPath } from 'node:url';

const here = path.dirname(fileURLToPath(import.meta.url));
const repoRoot = path.resolve(here, '..', '..');

const pageErrors: string[] = [];

test.beforeEach(({ page }) => {
  pageErrors.length = 0;
  page.on('pageerror', (error) => pageErrors.push(error.message));
});

test.afterEach(() => {
  expect(pageErrors).toEqual([]);
});

async function openApp(page: import('@playwright/test').Page): Promise<void> {
  await page.goto('/');
  await expect(page.locator('[data-module-status="ready"]')).toBeVisible({
    timeout: 120_000,
  });
}

/** The DNG name the UI derives from a source file. */
function resultName(filePath: string): string {
  const name = path.basename(filePath);
  const dot = name.lastIndexOf('.');
  return `${dot > 0 ? name.slice(0, dot) : name}.dng`;
}

/** Sets a range input through React's value tracker. */
async function setRange(
  page: import('@playwright/test').Page,
  label: string,
  value: string,
): Promise<void> {
  await page.getByLabel(label).evaluate((input, next) => {
    const element = input as HTMLInputElement;
    const setter = Object.getOwnPropertyDescriptor(
      HTMLInputElement.prototype,
      'value',
    )?.set;
    setter?.call(element, next);
    element.dispatchEvent(new Event('input', { bubbles: true }));
    element.dispatchEvent(new Event('change', { bubbles: true }));
  }, value);
}

test('stores one custom preset from the selected file and reuses it', async ({
  page,
}) => {
  const raw =
    process.env.HIRACO_E2E_SMALL_RAW ??
    [path.join(repoRoot, 'reference', '_3060001.ORF'), path.join(repoRoot, 'reference', '_3210504.ORF')]
      .find(existsSync) ??
    path.join(repoRoot, 'reference', '_3060001.ORF');
  test.skip(!existsSync(raw), 'the small RAW fixture is not part of the repository');

  await page.addInitScript(() => {
    if (sessionStorage.getItem('hiraco-preset-test-cleared') === null) {
      localStorage.clear();
      sessionStorage.setItem('hiraco-preset-test-cleared', '1');
    }
  });
  await openApp(page);
  await page.getByLabel('RAW file').setInputFiles(raw);
  await expect(page.getByTestId('preview-canvas')).toBeVisible({ timeout: 300_000 });
  await expect(page.getByRole('button', { name: 'Clear all' })).toBeEnabled({
    timeout: 300_000,
  });

  const blur = page.getByLabel('Blur Radius');
  await setRange(page, 'Blur Radius', '2.5');
  await expect(blur).toHaveValue('2.5');
  await page.getByRole('button', { name: 'Set as custom preset', exact: true }).click();
  await expect(
    page.getByRole('button', { name: 'Custom', exact: true }),
  ).toHaveAttribute('aria-pressed', 'true');

  // Change the slider away from the saved values, then apply Custom.
  await setRange(page, 'Blur Radius', '1');
  await expect(blur).toHaveValue('1');
  await page.getByRole('button', { name: 'Custom', exact: true }).click();
  await expect(blur).toHaveValue('2.5');

  // Saving again overwrites the single slot instead of adding a second preset.
  await setRange(page, 'Blur Radius', '3.5');
  await page.getByRole('button', { name: 'Set as custom preset', exact: true }).click();
  await setRange(page, 'Blur Radius', '1');
  await page.getByRole('button', { name: 'Custom', exact: true }).click();
  await expect(blur).toHaveValue('3.5');

  // One preset slot survives a reload even though the queue does not.
  await page.reload();
  await openApp(page);
  // Presets only edit a selected file, so the queue has to be rebuilt before
  // the saved slot can be applied.
  await page.getByLabel('RAW file').setInputFiles(raw);
  await expect(page.getByTestId('preview-canvas')).toBeVisible({ timeout: 300_000 });
  await expect(
    page.getByRole('button', { name: 'Custom', exact: true }),
  ).toBeEnabled({ timeout: 300_000 });
  await page.getByRole('button', { name: 'Custom', exact: true }).click();
  await expect(page.getByLabel('Blur Radius')).toHaveValue('3.5');
});

test('copies settings between queued files and converts them all', async ({
  page,
}) => {
  const first =
    process.env.HIRACO_E2E_RAW_A ?? path.join(repoRoot, 'sample', '_8142489_.ORF');
  const second =
    process.env.HIRACO_E2E_RAW_B ??
    [path.join(repoRoot, 'reference', '_3060001.ORF'), path.join(repoRoot, 'reference', '_3210504.ORF')]
      .find(existsSync) ??
    path.join(repoRoot, 'reference', '_3060001.ORF');
  test.skip(
    !existsSync(first) || !existsSync(second),
    'the queue fixtures are not part of the repository',
  );

  await openApp(page);
  await page.getByLabel('RAW file').setInputFiles([first, second]);
  await expect(page.getByTestId('queue-item')).toHaveCount(2);

  // The first queued file is selected and prepared.
  await expect(page.getByTestId('preview-canvas')).toBeVisible({ timeout: 300_000 });
  await expect(
    page.getByRole('button', {
      name: `Remove ${path.basename(first)}`,
      exact: true,
    }),
  ).toBeVisible();

  // Each queue entry has its own settings, like the native queue.  Copy the
  // Strong preset from the second file and paste it into the first.
  await page.getByTestId('queue-item').nth(1).getByTestId('queue-select').click();
  await expect(
    page.getByRole('button', { name: 'Strong', exact: true }),
  ).toBeEnabled({ timeout: 300_000 });
  await page.getByRole('button', { name: 'Strong', exact: true }).click();
  // The fixture base determines the absolute Strong values; record them so
  // the assertions do not depend on one particular camera file.
  const strongDenoise = await page.getByLabel('Denoise').inputValue();
  // Strong leaves sigma at the source base, which is exactly the value an
  // old sparse copy failed to carry to a different-base target.
  await setRange(page, 'Blur Radius', '2.5');
  await page.getByRole('button', { name: 'Copy settings', exact: true }).click();

  await page.getByTestId('queue-item').nth(0).getByTestId('queue-select').click();
  // The first row was already opened, so its fit preview comes back from the
  // per-row cache while the worker reopens the session.
  await expect(page.getByTestId('preview-canvas')).toBeVisible({ timeout: 1500 });
  await expect(page.getByLabel('Denoise')).not.toHaveValue(strongDenoise);
  await page.getByRole('button', { name: 'Paste settings', exact: true }).click();
  await expect(page.getByLabel('Denoise')).toHaveValue(strongDenoise);
  await expect(page.getByLabel('Blur Radius')).toHaveValue('2.5');

  // Copy a different preset from the selected file and push it to the whole
  // queue with the native-style "paste to all" action.
  await page.getByRole('button', { name: 'Small', exact: true }).click();
  const smallDenoise = await page.getByLabel('Denoise').inputValue();
  await page.getByRole('button', { name: 'Copy settings', exact: true }).click();
  await page.getByRole('button', { name: 'Paste to all files', exact: true }).click();
  await page.getByTestId('queue-item').nth(1).getByTestId('queue-select').click();
  await expect(page.getByLabel('Denoise')).toHaveValue(smallDenoise);

  // Leave the two entries with different settings so the conversion exercises
  // per-file overrides instead of one shared value set.
  await page.getByRole('button', { name: 'Strong', exact: true }).click();
  await expect(page.getByLabel('Denoise')).toHaveValue(strongDenoise);
  await expect(page.getByTestId('queue-item').nth(0)).toContainText('Small');
  await expect(page.getByTestId('queue-item').nth(1)).toContainText('Strong');

  const downloads: string[] = [];
  page.on('download', (download) => downloads.push(download.suggestedFilename()));
  await page.getByRole('button', { name: 'Convert', exact: true }).click();
  await expect.poll(() => downloads.length, { timeout: 600_000 }).toBe(2);
  expect(downloads.sort()).toEqual([resultName(first), resultName(second)].sort());
  await expect(page.locator('[data-testid="queue-item"][data-status="done"]')).toHaveCount(2);
});

test('pairs an ORI companion with its RAW into one queue entry', async ({ page }) => {
  const raw =
    process.env.HIRACO_E2E_PAIR_RAW ??
    [path.join(repoRoot, 'sample', '_8142492.ORF'), path.join(repoRoot, 'sample', '_8142490.ORF')]
      .find(existsSync) ??
    path.join(repoRoot, 'sample', '_8142492.ORF');
  const companion =
    process.env.HIRACO_E2E_PAIR_ORI ??
    [path.join(repoRoot, 'sample', '_8142492.ORI'), path.join(repoRoot, 'sample', '_8142490.ORI')]
      .find(existsSync) ??
    path.join(repoRoot, 'sample', '_8142492.ORI');
  test.skip(
    !existsSync(raw) || !existsSync(companion),
    'the ORI pair fixtures are not part of the repository',
  );

  await openApp(page);
  await page.getByLabel('RAW file').setInputFiles([raw, companion]);
  await expect(page.getByTestId('queue-item')).toHaveCount(1);
  const item = page.getByTestId('queue-item');
  await expect(item).toContainText(path.basename(raw));
  await expect(item).toContainText('ORI');

});


test('removes the selected queue entry', async ({ page }) => {
  const raw =
    process.env.HIRACO_E2E_SMALL_RAW ??
    [path.join(repoRoot, 'reference', '_3060001.ORF'), path.join(repoRoot, 'reference', '_3210504.ORF')]
      .find(existsSync) ??
    path.join(repoRoot, 'reference', '_3060001.ORF');
  test.skip(!existsSync(raw), 'the small RAW fixture is not part of the repository');

  await openApp(page);
  // Exercise the visible add control, not only the hidden file input.
  const chooserPromise = page.waitForEvent('filechooser');
  await page.getByRole('button', { name: 'Add files' }).click();
  const chooser = await chooserPromise;
  await chooser.setFiles(raw);
  await expect(page.getByTestId('queue-item')).toHaveCount(1);
  // Wait for the preview work to finish so the remove button is enabled.
  await expect(page.getByRole('button', { name: 'Clear all' })).toBeEnabled({
    timeout: 300_000,
  });

  await page.getByRole('button', { name: `Remove ${path.basename(raw)}`, exact: true }).click();
  await expect(page.getByTestId('queue-item')).toHaveCount(0);
  await expect(page.getByText('No files queued yet.')).toBeVisible();

  // Clear all and adding the same file again must both work.
  await page.getByLabel('RAW file').setInputFiles(raw);
  await expect(page.getByTestId('queue-item')).toHaveCount(1);
  await expect(page.getByRole('button', { name: 'Clear all' })).toBeEnabled({
    timeout: 300_000,
  });
  await page.getByRole('button', { name: 'Clear all' }).click();
  await expect(page.getByTestId('queue-item')).toHaveCount(0);
  await page.getByLabel('RAW file').setInputFiles(raw);
  await expect(page.getByTestId('queue-item')).toHaveCount(1);
});
