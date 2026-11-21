'use strict';

const assert = require('node:assert/strict');
const fs = require('node:fs');
const path = require('node:path');
const test = require('node:test');
const vm = require('node:vm');

function createClient(responseFactory) {
  const calls = [];
  const document = { cookie: 'visioncam_session=opaque; visioncam_csrf=csrf%2Btoken', addEventListener() {} };
  const context = {
    URLSearchParams,
    document,
    window: {},
    console,
    setTimeout,
    clearTimeout,
    fetch: async (url, options) => {
      calls.push({ url, options });
      return responseFactory
        ? responseFactory(url, options)
        : { ok: true, status: 200, json: async () => ({ ok: true, data: {} }) };
    }
  };
  vm.runInNewContext(
    fs.readFileSync(path.join(__dirname, '../src/app.js'), 'utf8'),
    context,
    { filename: 'app.js' }
  );
  return { api: context.window.VisionCamAPI, calls, context };
}

test('all registered JSON API surfaces have a frontend method', async () => {
  const { api, calls } = createClient();
  await Promise.all([
    api.login('operator', 'secret'),
    api.session(),
    api.logout(),
    api.completeFirstLogin('old', 'new'),
    api.changePassword('old', 'new', 'operator2'),
    api.device(),
    api.status(),
    api.health(),
    api.metrics(),
    api.provisionWifi('Example "WiFi"', 'fake\\test-password'),
    api.restart(),
    api.cameraStatus(),
    api.cameraSettings(),
    api.saveCameraSettings({ framesize: 10, quality: 12 }),
    api.recordingStatus(),
    api.recordingConfig(),
    api.saveRecordingConfig({ enabled: true, capture_fps: 10, segment_duration_s: 60 }),
    api.startRecording(),
    api.stopRecording(),
    api.recordings(0, 12),
    api.recordingMetadata('rec-example'),
    api.deleteRecording('rec-example'),
    api.captureSnapshot(),
    api.storageStatus(),
    api.media(0, 12, 0),
    api.mediaMetadata('med-example'),
    api.deleteMedia('med-example'),
    api.rebuildIndex(),
    api.retention(),
    api.saveRetention({ enabled: true, max_used_percent: 90 }),
    api.retentionPreview(),
    api.runRetention(),
    api.motionConfig(),
    api.saveMotionConfig({ enabled: false }),
    api.motionStatus(),
    api.mqttConfig(),
    api.saveMqttConfig({ enabled: false }),
    api.testMqtt(),
    api.reconnectMqtt(),
    api.testPublish(),
    api.haStatus(),
    api.republishDiscovery(),
    api.networkStorage(),
    api.saveNetworkStorage({ enabled: false }),
    api.testNetworkStorage(),
    api.networkStorageQueue()
  ]);

  const routes = new Set(calls.map(({ url, options }) => `${options.method} ${url.split('?')[0]}`));
  const expected = [
    'POST /auth/login', 'GET /auth/session', 'POST /auth/logout',
    'POST /auth/complete-first-login', 'POST /auth/change-password',
    'GET /device', 'GET /status', 'GET /health', 'GET /metrics', 'POST /wifi/config',
    'POST /device/restart',
    'GET /camera/status', 'GET /camera/settings', 'PUT /camera/settings',
    'GET /recording/status', 'GET /recording/config', 'PUT /recording/config',
    'POST /recording/start', 'POST /recording/stop', 'GET /recordings',
    'DELETE /recordings', 'POST /snapshot', 'GET /storage', 'DELETE /storage',
    'POST /storage', 'PUT /storage', 'GET /motion/config', 'PUT /motion/config',
    'GET /motion/status', 'GET /integrations/mqtt', 'PUT /integrations/mqtt',
    'POST /integrations/mqtt/test', 'POST /integrations/mqtt/reconnect',
    'POST /integrations/mqtt/test-publish', 'GET /integrations/home-assistant/status',
    'POST /integrations/home-assistant/discovery',
    'GET /integrations/network-storage', 'PUT /integrations/network-storage',
    'POST /integrations/network-storage/test', 'GET /integrations/network-storage/queue'
  ];
  expected.forEach((route) => assert.ok(routes.has(route), `missing frontend route method: ${route}`));
  const actionUrls = new Set(calls.map(({ url }) => url));
  [
    '/storage?action=status',
    '/storage?action=files&offset=0&limit=12&category=0',
    '/storage?action=file&id=med-example',
    '/storage?action=rebuild',
    '/storage?action=retention',
    '/storage?action=retention-preview',
    '/storage?action=retention-run'
  ].forEach((url) => assert.ok(actionUrls.has(url), `missing storage action: ${url}`));
  assert.ok(actionUrls.has('/recordings?action=metadata&id=rec-example'));
  const wifiRequest = calls.find(({ url }) => url === '/wifi/config');
  assert.equal(wifiRequest.options.method, 'POST');
  assert.equal(wifiRequest.options.body,
    JSON.stringify({ ssid: 'Example "WiFi"', password: 'fake\\test-password' }));
  assert.equal(wifiRequest.options.headers['X-CSRF-Token'], 'csrf+token');
});

test('requests use cookie sessions and add the double-submit CSRF header on writes', async () => {
  const { api, calls } = createClient();
  await api.request('/camera/settings');
  await api.request('/camera/settings', 'PUT', { quality: 12 });

  assert.equal(calls[0].options.credentials, 'same-origin');
  assert.equal(calls[0].options.headers['X-CSRF-Token'], undefined);
  assert.equal(calls[1].options.credentials, 'same-origin');
  assert.equal(calls[1].options.headers['X-CSRF-Token'], 'csrf+token');
  assert.equal(calls[1].options.body, JSON.stringify({ quality: 12 }));
});

test('firmware error envelopes remain errors instead of success-shaped fallbacks', async () => {
  const { api } = createClient(() => ({
    ok: false,
    status: 403,
    json: async () => ({ ok: false, error: { code: 3, message: 'CSRF token missing or invalid' } })
  }));
  const result = await api.request('/storage?action=rebuild', 'POST', {});
  assert.equal(result.ok, false);
  assert.equal(result.error.message, 'CSRF token missing or invalid');
});

test('binary camera capture and media URLs use the active authenticated endpoints', async () => {
  const { api, calls } = createClient(() => ({
    ok: true, status: 200, blob: async () => ({ type: 'image/jpeg' })
  }));
  const frame = await api.captureFrame();
  assert.equal(frame.type, 'image/jpeg');
  assert.equal(calls[0].url, '/camera/capture');
  assert.equal(calls[0].options.method, 'POST');
  assert.equal(api.recordingDownloadUrl('rec-a'), '/recordings?action=download&id=rec-a');
  assert.equal(api.recordingPlayUrl('rec-a'), '/recordings?action=play&id=rec-a');
  assert.equal(api.snapshotImageUrl('med-a'), '/snapshot?action=image&id=med-a');
});
