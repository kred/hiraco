import { expect, test } from '@playwright/test';
import { existsSync } from 'node:fs';
import path from 'node:path';
import { fileURLToPath } from 'node:url';

const here = path.dirname(fileURLToPath(import.meta.url));
const repoRoot = path.resolve(here, '..', '..');

async function openApp(page: import('@playwright/test').Page): Promise<void> {
  await page.goto('/');
  await expect(page.locator('[data-module-status="ready"]')).toBeVisible({
    timeout: 120_000,
  });
}

test('stores one custom preset from the selected file and reuses it', async ({
  page,
}) => {
  const raw =
    process.env.HIRACO_E2E_SMALL_RAW ?? path.join(repoRoot, 'reference', '_3060001.ORF');
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
  const setBlur = (value: string) =>
    blur.evaluate((input, next) => {
      const element = input as HTMLInputElement;
      const setter = Object.getOwnPropertyDescriptor(
        HTMLInputElement.prototype,
        'value',
      )?.set;
      setter?.call(element, next);
      element.dispatchEvent(new Event('input', { bubbles: true }));
      element.dispatchEvent(new Event('change', { bubbles: true }));
    }, value);

  await setBlur('2.5');
  await expect(blur).toHaveValue('2.5');
  await page.getByRole('button', { name: 'Set as custom preset', exact: true }).click();
  await expect(
    page.getByRole('button', { name: 'Custom', exact: true }),
  ).toHaveAttribute('aria-pressed', 'true');

  // Change the slider away from the saved values, then apply Custom.
  await setBlur('1');
  await expect(blur).toHaveValue('1');
  await page.getByRole('button', { name: 'Custom', exact: true }).click();
  await expect(blur).toHaveValue('2.5');

  // One preset slot survives a reload even though the queue does not.
  await page.reload();
  await openApp(page);
  await page.getByRole('button', { name: 'Custom', exact: true }).click();
  await expect(page.getByLabel('Blur Radius')).toHaveValue('2.5');
});

test('copies settings between queued files and converts them all', async ({
  page,
}) => {
  const first =
    process.env.HIRACO_E2E_RAW_A ?? path.join(repoRoot, 'sample', '_8142489_.ORF');
  const second =
    process.env.HIRACO_E2E_RAW_B ?? path.join(repoRoot, 'reference', '_3060001.ORF');
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
  await page.getByTestId('queue-item').nth(1).getByRole('button').first().click();
  await expect(page.getByTestId('preview-canvas')).toBeVisible({ timeout: 300_000 });
  await page.getByRole('button', { name: 'Strong', exact: true }).click();
  await expect(page.getByLabel('Denoise')).toHaveValue('0.24');
  await page.getByRole('button', { name: 'Copy settings', exact: true }).click();

  await page.getByTestId('queue-item').nth(0).getByRole('button').first().click();
  await expect(page.getByLabel('Denoise')).toHaveValue('0.33');
  await page.getByRole('button', { name: 'Paste settings', exact: true }).click();
  await expect(page.getByLabel('Denoise')).toHaveValue('0.24');

  // Copy a different preset from the selected file and push it to the whole
  // queue with the native-style "paste to all" action.
  await page.getByRole('button', { name: 'Small', exact: true }).click();
  await expect(page.getByLabel('Denoise')).toHaveValue('0.41');
  await page.getByRole('button', { name: 'Copy settings', exact: true }).click();
  await page.getByRole('button', { name: 'Paste to all files', exact: true }).click();
  await page.getByTestId('queue-item').nth(1).getByRole('button').first().click();
  await expect(page.getByLabel('Denoise')).toHaveValue('0.41');

  const downloads: string[] = [];
  page.on('download', (download) => downloads.push(download.suggestedFilename()));
  await page.getByRole('button', { name: 'Convert', exact: true }).click();
  await expect.poll(() => downloads.length, { timeout: 600_000 }).toBe(2);
  expect(downloads.sort()).toEqual(['_3060001.dng', '_8142489_.dng']);
  await expect(page.locator('[data-testid="queue-item"][data-status="done"]')).toHaveCount(2);
});

test('pairs an ORI companion with its RAW into one queue entry', async ({ page }) => {
  const raw = process.env.HIRACO_E2E_PAIR_RAW ?? path.join(repoRoot, 'sample', '_8142492.ORF');
  const companion =
    process.env.HIRACO_E2E_PAIR_ORI ?? path.join(repoRoot, 'sample', '_8142492.ORI');
  test.skip(
    !existsSync(raw) || !existsSync(companion),
    'the ORI pair fixtures are not part of the repository',
  );

  await openApp(page);
  await page.getByLabel('RAW file').setInputFiles([raw, companion]);
  await expect(page.getByTestId('queue-item')).toHaveCount(1);
  const item = page.getByTestId('queue-item');
  await expect(item).toContainText('_8142492.ORF');
  await expect(item).toContainText('ORI');

});


test('removes the selected queue entry', async ({ page }) => {
  const raw =
    process.env.HIRACO_E2E_SMALL_RAW ?? path.join(repoRoot, 'reference', '_3060001.ORF');
  test.skip(!existsSync(raw), 'the small RAW fixture is not part of the repository');

  await openApp(page);
  await page.getByLabel('RAW file').setInputFiles(raw);
  await expect(page.getByTestId('queue-item')).toHaveCount(1);
  // Wait for the preview work to finish so the remove button is enabled.
  await expect(page.getByRole('button', { name: 'Clear all' })).toBeEnabled({
    timeout: 300_000,
  });

  await page.getByRole('button', { name: 'Remove _3060001.ORF', exact: true }).click();
  await expect(page.getByTestId('queue-item')).toHaveCount(0);
  await expect(page.getByText('No files queued yet.')).toBeVisible();
});
