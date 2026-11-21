import { test, expect } from '@playwright/test';

const TEST_USER = 'operator';
const TEST_PASSWORD = 'playwright-only-password';

async function signIn(page: import('@playwright/test').Page) {
  await page.goto('/');
  await page.locator('#login-form [name="username"]').fill(TEST_USER);
  await page.locator('#login-form [name="password"]').fill(TEST_PASSWORD);
  await page.getByRole('button', { name: 'Sign in' }).click();
  await expect(page.locator('#app')).toBeVisible();
}

test.beforeEach(async ({ page, request }) => {
  await request.post('/__test/reset');
  await page.context().clearCookies();
});

test('requires sign-in and clears the submitted password', async ({ page }) => {
  await page.goto('/');
  await expect(page.locator('#login-page')).toBeVisible();
  await expect(page.locator('#app')).toBeHidden();

  await page.locator('#login-form [name="username"]').fill(TEST_USER);
  await page.locator('#login-form [name="password"]').fill('incorrect');
  await page.getByRole('button', { name: 'Sign in' }).click();
  await expect(page.locator('#login-error')).toBeVisible();
  await expect(page.locator('#login-form input[name="password"]')).toHaveValue('');

  await signIn(page);
  expect(await page.evaluate(() => localStorage.length)).toBe(0);
});

test('authenticated navigation renders each dashboard area', async ({ page }) => {
  await signIn(page);
  const pages = [
    ['overview', 'Overview'],
    ['live', 'Live view'],
    ['snapshots', 'Media library'],
    ['recordings', 'Recordings'],
    ['storage', 'Storage'],
    ['settings', 'Settings'],
    ['integrations', 'Integrations']
  ];

  for (const [id, title] of pages) {
    await page.locator(`button[data-page="${id}"]`).first().click();
    await expect(page.locator(`#page-${id}`)).toBeVisible();
    await expect(page.locator('#page-title')).toHaveText(title);
    await expect(page.locator(`.nav-item[data-page="${id}"]`)).toHaveClass(/active/);
  }
});

test('Wi-Fi setup rejects failed joins, then shuts down the AP after a verified join', async ({ page, request }) => {
  await signIn(page);
  await page.locator('button[data-page="settings"]').click();

  const form = page.locator('#wifi-form');
  await expect(form).toBeVisible();
  await form.locator('[name="ssid"]').fill('Test Offline');
  await form.locator('[name="password"]').fill('fake-test-password');
  await form.getByRole('button', { name: 'Test and save Wi-Fi' }).click();
  await expect(page.locator('#toast')).toContainText('Could not join that Wi-Fi network');
  await expect(form).toBeVisible();

  await form.locator('[name="ssid"]').fill('Test Network');
  await form.getByRole('button', { name: 'Test and save Wi-Fi' }).click();
  await expect(form).toBeHidden();
  await expect(page.locator('#wifi-setup-note')).toContainText('hotspot is closing');
  await expect(form.locator('[name="password"]')).toHaveValue('');

  const state = await (await request.get('/__test/state')).json();
  expect(state.wifiAttempts).toEqual([
    { ssid: 'Test Offline', passwordReceived: true, csrfValid: true, staged: false },
    { ssid: 'Test Network', passwordReceived: true, csrfValid: true, staged: true }
  ]);
});

test('settings save through the API client and report success', async ({ page }) => {
  await signIn(page);
  await page.locator('button[data-page="settings"]').click();
  await expect(page.locator('#camera-form')).toBeVisible();
  await page.locator('#camera-form').getByRole('button', { name: 'Save camera settings' }).click();
  await expect(page.locator('#toast')).toContainText('Settings saved');
});

test('mobile menu opens and navigation closes it', async ({ page }) => {
  await page.setViewportSize({ width: 375, height: 812 });
  await signIn(page);
  await page.getByRole('button', { name: 'Open navigation' }).click();
  await expect(page.locator('#sidebar')).toHaveClass(/open/);
  await page.locator('button[data-page="integrations"]').click();
  await expect(page.locator('#page-integrations')).toBeVisible();
  await expect(page.locator('#sidebar')).not.toHaveClass(/open/);
});

test('logout returns to the sign-in screen', async ({ page }) => {
  await signIn(page);
  await page.getByRole('button', { name: 'Sign out' }).click();
  await expect(page.locator('#login-page')).toBeVisible();
  await expect(page.locator('#app')).toBeHidden();
});
