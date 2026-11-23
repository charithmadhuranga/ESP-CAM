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

test('integrations page shows RTSP status and Home Assistant URL without the password', async ({ page }) => {
  await signIn(page);
  await page.locator('.nav-item[data-page=integrations]').click();
  await expect(page.locator('#rtsp-live-status')).toHaveText('Running · 1 viewer');
  await expect(page.locator('#rtsp-url')).toHaveText(/^rtsp:\/\/homeassistant:PASSWORD@[^/]+:554\/stream$/);
  await expect(page.locator('#rtsp-ha-url')).toHaveText(/\/stream#video=h264$/);
  await expect(page.locator('#rtsp-form input[name=password]')).toHaveValue('');
  await expect(page.locator('#rtsp-password-hint')).toBeHidden();
  await page.context().grantPermissions(['clipboard-read', 'clipboard-write']);
  // Copy opens the dialog; the generated URL is shown as the password is typed.
  await page.locator('[data-command=copy-rtsp][data-target=rtsp-ha-url]').click();
  await expect(page.locator('#rtsp-copy-dialog')).toBeVisible();
  await expect(page.locator('#rtsp-copy-title')).toHaveText('Home Assistant URL');
  await expect(page.locator('#rtsp-copy-output')).toHaveValue('');
  await page.locator('#rtsp-copy-form input[name=password]').fill('Te@st#1:x/y%z');
  const expected = /^rtsp:\/\/homeassistant:Te@st%231:x%2Fy%25z@[^/]+:554\/stream#video=h264$/;
  await expect(page.locator('#rtsp-copy-output')).toHaveValue(expected);
  // Copy URL: clipboard gets it, dialog stays open with the URL visible + selected.
  await page.locator('#rtsp-copy-form button[type=submit]').click();
  await expect(page.locator('#rtsp-copy-status')).toContainText('Copied');
  await expect(page.locator('#rtsp-copy-dialog')).toBeVisible();
  expect(await page.evaluate(() => navigator.clipboard.readText())).toMatch(expected);
  expect(await page.evaluate(() => {
    const o = document.getElementById('rtsp-copy-output');
    return o.selectionStart === 0 && o.selectionEnd === o.value.length;
  })).toBe(true);
  // A password containing '@' keeps it readable.
  await page.locator('#rtsp-copy-form input[name=password]').fill('Example12@Z');
  await expect(page.locator('#rtsp-copy-output')).toHaveValue(/^rtsp:\/\/homeassistant:Example12@Z@[^/]+:554\/stream#video=h264$/);
  await page.locator('#rtsp-copy-dialog [data-command=close-dialog]').click();
  // Password typed in the RTSP form is reused: URL shown immediately.
  await page.locator('#rtsp-form input[name=password]').fill('Typed#Pass1');
  await page.locator('[data-command=copy-rtsp][data-target=rtsp-url]').click();
  await expect(page.locator('#rtsp-copy-output')).toHaveValue(/:Typed%23Pass1@[^/]+:554\/stream$/);
  await page.locator('#rtsp-copy-dialog [data-command=close-dialog]').click();
  await page.locator('#rtsp-form input[name=password]').fill('');
  await page.locator('.rtsp-steps summary').click();
  await expect(page.locator('.rtsp-steps')).toContainText('Generic Camera');
});


test('confirmations use the in-app dialog, never a native browser prompt', async ({ page }) => {
  page.on('dialog', (dialog) => { throw new Error(`native ${dialog.type()} dialog shown: ${dialog.message()}`); });
  const restarts: string[] = [];
  page.on('request', (request) => { if (request.url().includes('/device/restart')) restarts.push(request.method()); });
  await signIn(page);
  await page.locator('.nav-item[data-page=settings]').click();

  // Cancel: dialog closes, nothing is sent.
  await page.locator('[data-command=restart]').click();
  await expect(page.locator('#confirm-dialog')).toBeVisible();
  await expect(page.locator('#confirm-title')).toHaveText('Restart VisionCam Pro?');
  await expect(page.locator('#confirm-cancel')).toBeFocused();
  await expect(page.locator('#confirm-ok')).toHaveClass(/button-danger/);
  await page.locator('#confirm-cancel').click();
  await expect(page.locator('#confirm-dialog')).toBeHidden();

  // Escape also cancels.
  await page.locator('[data-command=restart]').click();
  await page.keyboard.press('Escape');
  await expect(page.locator('#confirm-dialog')).toBeHidden();
  expect(restarts).toEqual([]);

  // Confirm: exactly one request.
  await page.locator('[data-command=restart]').click();
  await page.locator('#confirm-ok').click();
  await expect(page.locator('#confirm-dialog')).toBeHidden();
  await expect.poll(() => restarts.length).toBe(1);

  // Deleting a recording asks first; Cancel keeps it.
  const deletes: string[] = [];
  page.on('request', (request) => { if (request.method() === 'DELETE') deletes.push(request.url()); });
  await page.locator('.nav-item[data-page=recordings]').click();
  await page.locator('[data-action=delete-recording]').first().click();
  await expect(page.locator('#confirm-title')).toHaveText('Delete this recording?');
  await page.locator('#confirm-cancel').click();
  expect(deletes).toEqual([]);
});

test('RTSP URLs are generated even when another integrations request fails, and after saving', async ({ page }) => {
  await page.route('**/integrations/network-storage/queue', (route) => route.abort('timedout'));
  await signIn(page);
  await page.locator('.nav-item[data-page=integrations]').click();
  await expect(page.locator('#rtsp-url')).toHaveText(/^rtsp:\/\/homeassistant:PASSWORD@[^/]+:554\/stream$/);
  await expect(page.locator('#rtsp-ha-url')).toHaveText(/:554\/stream#video=h264$/);
  await expect(page.locator('#rtsp-form input[name=username]')).toHaveValue('homeassistant');

  // After Save the URLs come from the PUT reply, even if every later GET fails.
  await page.route('**/integrations/rtsp', (route) =>
    route.request().method() === 'GET' ? route.abort('timedout') : route.continue());
  await page.locator('#rtsp-form button[type=submit]').click();
  await expect(page.locator('#toast')).toContainText('Settings saved');
  await expect(page.locator('#rtsp-url')).toHaveText(/^rtsp:\/\/homeassistant:PASSWORD@[^/]+:554\/stream$/);
  await page.waitForTimeout(4000);   // delayed refresh fails twice: URLs must stay
  await expect(page.locator('#rtsp-url')).toHaveText(/^rtsp:\/\/homeassistant:PASSWORD@/);
});
