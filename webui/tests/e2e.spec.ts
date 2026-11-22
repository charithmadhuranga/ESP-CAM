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

test('recording controls show and synchronize the active recording state', async ({ page }) => {
  await signIn(page);
  const controls = page.locator('[data-command="toggle-recording"]');
  await expect(controls).toHaveCount(3);
  await expect(page.locator('#recording-badge')).toHaveText('Not recording');

  await page.locator('#page-overview [data-command="toggle-recording"]').click();
  await expect(controls).toHaveText([
    'Stop recording',
    'Stop recording',
    'Stop recording'
  ]);
  await expect(page.locator('#recording-badge')).toHaveText('Recording');
  await expect(page.locator('#recording-badge')).toHaveAttribute('data-state', 'recording');
  await expect(controls.first()).toHaveAttribute('aria-pressed', 'true');
  await expect(controls.first()).toHaveClass(/button-danger/);

  await page.locator('.nav-item[data-page="live"]').click();
  await page.locator('#page-live [data-command="toggle-recording"]').click();
  await expect(controls).toHaveText([
    'Start recording',
    'Start recording',
    'Start recording'
  ]);
  await expect(page.locator('#recording-badge')).toHaveText('Not recording');
});

test('live view receives MJPEG frames and controls the flash', async ({ page, request }) => {
  await signIn(page);
  const streamResponse = page.waitForResponse((response) =>
    response.url().endsWith('/camera/stream'));
  await page.locator('.nav-item[data-page="live"]').click();
  expect((await streamResponse).status()).toBe(200);
  await expect.poll(() =>
    page.locator('#live-stream').evaluate((image: HTMLImageElement) => image.naturalWidth)
  ).toBeGreaterThan(0);
  await expect(page.locator('#stream-state')).toHaveText('Live');

  const flash = page.locator('#flash-toggle');
  await flash.click();
  await expect(flash).toHaveText('Turn flash off');
  await expect(flash).toHaveAttribute('aria-pressed', 'true');
  await flash.click();
  await expect(flash).toHaveText('Turn flash on');
  await expect(flash).toHaveAttribute('aria-pressed', 'false');
  await flash.click();
  await expect(flash).toHaveAttribute('aria-pressed', 'true');
  await page.locator('.nav-item[data-page="overview"]').click();
  await expect(page.locator('#page-overview')).toBeVisible();

  const deviceState = await (await request.get('/__test/state')).json();
  expect(deviceState.streamRequests).toBe(1);
  expect(deviceState.flashUpdates).toEqual([true, false, true, false]);
});

test('live view returns to sign-in when the reboot invalidates its session', async ({ page, request }) => {
  await signIn(page);
  await request.post('/__test/expire-session');
  await page.locator('.nav-item[data-page="live"]').click();

  await expect(page.locator('#login-page')).toBeVisible();
  const deviceState = await (await request.get('/__test/state')).json();
  expect(deviceState.streamRequests).toBe(0);
});

test('recording playback reports its stream and downloads the complete AVI response', async ({ page }) => {
  await signIn(page);
  await page.locator('button[data-page="recordings"]').click();

  const playResponse = page.waitForResponse((response) =>
    response.url().includes('/recordings?action=play&id=rec-test-001'));
  await page.getByRole('button', { name: 'Play' }).click();
  const playback = await playResponse;
  expect(playback.status()).toBe(200);
  await expect(page.locator('#playback-dialog')).toBeVisible();
  await expect(page.locator('#playback-status')).toHaveText('Recording is playing.');
  await page.locator('#playback-dialog').getByRole('button', { name: 'Close' }).click();
  await expect(page.locator('#playback-dialog')).toBeHidden();

  const downloadPromise = page.waitForEvent('download');
  await page.getByRole('button', { name: 'Download' }).click();
  const download = await downloadPromise;
  expect(download.suggestedFilename()).toBe('rec-test-001.avi');
  const stream = await download.createReadStream();
  const chunks = [];
  for await (const chunk of stream) chunks.push(chunk);
  const downloadedBytes = Buffer.concat(chunks);
  expect(downloadedBytes.byteLength).toBe(8 * 1024 * 1024 + 64);
  expect(downloadedBytes.subarray(0, 4).toString()).toBe('RIFF');
  expect(downloadedBytes.subarray(8, 12).toString()).toBe('AVI ');
  expect(downloadedBytes.at(-1)).toBe(0x41);
});

test('playback failures are shown in the player', async ({ page }) => {
  await signIn(page);
  await page.locator('button[data-page="recordings"]').first().click();
  await page.route('**/recordings?**', async (route) => {
    const url = new URL(route.request().url());
    if (url.searchParams.get('action') === 'play') {
      await route.fulfill({
        status: 503,
        contentType: 'application/json',
        body: JSON.stringify({ ok: false, error: { message: 'Playback unavailable' } })
      });
    } else {
      await route.continue();
    }
  });
  await page.getByRole('button', { name: 'Play' }).click();
  await expect(page.locator('#playback-status')).toContainText('Playback could not be loaded');
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
