import { expect, test } from '@playwright/test';

/**
 * Fixture-free smoke tests.  The RAW conversion tests skip when the
 * gitignored camera files are absent, but these checks still exercise the
 * built application and the queue controls in CI.
 */
test.beforeEach(async ({ page }) => {
  await page.goto('/');
  await expect(page.locator('[data-module-status="ready"]')).toBeVisible({
    timeout: 120_000,
  });
});

test('renders the empty queue and the disabled processing controls', async ({
  page,
}) => {
  await expect(page.getByRole('region', { name: 'Workspace' })).toBeVisible();
  await expect(page.getByRole('heading', { name: 'Queue' })).toBeVisible();
  await expect(page.getByText('No files queued yet.')).toBeVisible();
  await expect(
    page.getByText(/Select a queued file to edit its settings/),
  ).toBeVisible();

  // No file is selected, so the per-file controls must not accept edits.
  for (const name of ['Small', 'Medium', 'Strong', 'Custom']) {
    await expect(page.getByRole('button', { name, exact: true })).toBeDisabled();
  }
  await expect(page.getByLabel('Blur Radius')).toBeDisabled();
  await expect(page.getByLabel('Denoise')).toBeDisabled();
  await expect(
    page.getByRole('button', { name: 'Convert', exact: true }),
  ).toBeDisabled();
  await expect(page.getByRole('button', { name: 'Cancel' })).toBeDisabled();
});

test('labels the queue actions and opens the file picker', async ({ page }) => {
  const add = page.getByRole('button', { name: 'Add files' });
  await expect(add).toHaveAttribute('title', /RAW or DNG files/);
  await expect(
    page.getByRole('button', { name: 'Add companion ORI' }),
  ).toBeDisabled();
  await expect(page.getByRole('button', { name: 'Clear all' })).toBeDisabled();

  const chooserPromise = page.waitForEvent('filechooser');
  await add.click();
  const chooser = await chooserPromise;
  await chooser.setFiles([]);
  await expect(page.getByTestId('queue-item')).toHaveCount(0);
});

test('switches the theme from the header', async ({ page }) => {
  const toggle = page.getByRole('button', {
    name: /Switch to the (light|dark) theme/,
  });
  const before = await page.locator('html').getAttribute('data-theme');
  await toggle.click();
  await expect(page.locator('html')).not.toHaveAttribute('data-theme', before ?? '');
});
