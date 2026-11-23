'use strict';

const http = require('node:http');
const fs = require('node:fs');
const path = require('node:path');

const root = path.resolve(__dirname, '../src');
const downloadBody = Buffer.alloc(8 * 1024 * 1024 + 64, 0x41);
downloadBody.write('RIFF', 0);
downloadBody.write('AVI ', 8);
const state = {
  wifiAttempts: [],
  writes: 0,
  recordingState: 'idle',
  playRequests: [],
  downloads: [],
  flashEnabled: false,
  flashUpdates: [],
  streamRequests: 0,
  cameraStatusUnauthorized: false
};
const csrfToken = 'playwright-csrf-token';
const sessionToken = 'playwright-session';

function envelope(data, statusCode = 200) {
  return { statusCode, body: JSON.stringify({ ok: statusCode < 400, data, error: statusCode < 400 ? null : { code: 16, message: 'Could not join that Wi-Fi network; check its password and use a 2.4 GHz network' }, request_id: 'test-1' }) };
}

function jsonResponse(res, result, headers = {}) {
  res.writeHead(result.statusCode, { 'Content-Type': 'application/json', 'Cache-Control': 'no-store', ...headers });
  res.end(result.body);
}

function readBody(req) {
  return new Promise((resolve, reject) => {
    let body = '';
    req.on('data', (chunk) => {
      body += chunk;
      if (body.length > 1024) reject(new Error('request body too large'));
    });
    req.on('end', () => {
      try { resolve(body ? JSON.parse(body) : {}); }
      catch { reject(new Error('invalid JSON')); }
    });
    req.on('error', reject);
  });
}

const server = http.createServer(async (req, res) => {
  const url = new URL(req.url, 'http://127.0.0.1');
  if (url.pathname === '/__test/reset') {
    state.wifiAttempts = [];
    state.writes = 0;
    state.recordingState = 'idle';
    state.playRequests = [];
    state.downloads = [];
    state.flashEnabled = false;
    state.flashUpdates = [];
    state.streamRequests = 0;
    state.cameraStatusUnauthorized = false;
    res.writeHead(204).end();
    return;
  }
  if (url.pathname === '/__test/expire-session') {
    state.cameraStatusUnauthorized = true;
    res.writeHead(204).end();
    return;
  }
  if (url.pathname === '/__test/state') {
    jsonResponse(res, { statusCode: 200, body: JSON.stringify(state) });
    return;
  }

  if (req.method === 'GET' && (url.pathname === '/' ||
      url.pathname.endsWith('.js') || url.pathname.endsWith('.css'))) {
    const relative = url.pathname === '/' ? 'index.html' : url.pathname.slice(1);
    const file = path.resolve(root, relative);
    if (!file.startsWith(`${root}${path.sep}`) && file !== path.join(root, 'index.html')) {
      res.writeHead(404).end();
      return;
    }
    fs.readFile(file, (error, content) => {
      if (error) {
        res.writeHead(404).end();
        return;
      }
      const type = file.endsWith('.html') ? 'text/html'
        : file.endsWith('.css') ? 'text/css'
          : 'text/javascript';
      const headers = { 'Content-Type': type };
      if (file.endsWith('index.html')) {
        headers['Set-Cookie'] = `visioncam_csrf=${csrfToken}; SameSite=Strict; Path=/`;
      }
      res.writeHead(200, headers).end(content);
    });
    return;
  }

  if (req.method === 'GET' && url.pathname === '/auth/session') {
    const authenticated = req.headers.cookie?.includes(`visioncam_session=${sessionToken}`) || false;
    jsonResponse(res, envelope({ authenticated, password_change_required: false, username: authenticated ? 'operator' : '' }));
    return;
  }
  if (req.method === 'POST' && url.pathname === '/auth/login') {
    const body = await readBody(req);
    if (body.username !== 'operator' || body.password !== 'playwright-only-password') {
      jsonResponse(res, envelope(null, 401));
      return;
    }
    jsonResponse(res, envelope({ authenticated: true, password_change_required: false }), {
      'Set-Cookie': `visioncam_session=${sessionToken}; HttpOnly; SameSite=Strict; Path=/`
    });
    return;
  }
  if (req.method === 'POST' && url.pathname === '/auth/logout') {
    jsonResponse(res, envelope({ logged_out: true }), {
      'Set-Cookie': 'visioncam_session=; HttpOnly; SameSite=Strict; Path=/; Max-Age=0'
    });
    return;
  }

  if (req.method !== 'GET' && url.pathname !== '/camera/stream') {
    if (req.headers['x-csrf-token'] !== csrfToken ||
        !req.headers.cookie?.includes(`visioncam_session=${sessionToken}`)) {
      jsonResponse(res, envelope(null, 403));
      return;
    }
  }

  if (req.method === 'POST' && url.pathname === '/wifi/config') {
    const body = await readBody(req);
    const unavailable = body.ssid === 'Test Offline';
    state.wifiAttempts.push({
      ssid: body.ssid,
      passwordReceived: typeof body.password === 'string' && body.password.length > 0,
      csrfValid: req.headers['x-csrf-token'] === csrfToken,
      staged: !unavailable
    });
    jsonResponse(res, unavailable
      ? envelope(null, 503)
      : envelope({ configured: false, provision_pending: true, connected: true, ap_mode: false, restarting: true }));
    return;
  }

  if (req.method === 'POST' && url.pathname === '/recording/start') {
    state.writes++;
    state.recordingState = 'recording';
    jsonResponse(res, envelope({ recording_id: 'rec-test-001', started: true }));
    return;
  }
  if (req.method === 'POST' && url.pathname === '/recording/stop') {
    state.writes++;
    state.recordingState = 'idle';
    jsonResponse(res, envelope({ stopped: true }));
    return;
  }
  if (req.method === 'POST' && url.pathname === '/camera/flash') {
    const body = await readBody(req);
    state.flashEnabled = body.enabled === true;
    state.flashUpdates.push(state.flashEnabled);
    jsonResponse(res, envelope({ enabled: state.flashEnabled }));
    return;
  }

  if (req.method === 'PUT' && url.pathname === '/integrations/rtsp') {
    state.writes++;
    jsonResponse(res, envelope({ enabled: true, port: 554, username: 'homeassistant', password_set: true, path: '/stream', codec: 'MJPEG', transport: 'RTP/AVP/TCP', max_clients: 2, status: { running: false, clients: 0, sessions_total: 3, auth_failures: 0, frames_sent: 120, frames_dropped: 0, last_error: '' }, schema_version: 1 }));
    return;
  }
  if (req.method !== 'GET') {
    state.writes++;
    jsonResponse(res, envelope({ saved: true, deleted: 0, restarting: true }));
    return;
  }

  let data = {};
  if (url.pathname === '/device') data = { device_name: 'VisionCam Pro', firmware_version: 'test-build' };
  else if (url.pathname === '/status') data = { wifi: { state: 'AP', configured: false, ap_mode: true, provision_pending: false }, time_synced: false, heap_free_bytes: 180000 };
  else if (url.pathname === '/health') data = { healthy: true };
  else if (url.pathname === '/camera/status') {
    if (state.cameraStatusUnauthorized) {
      jsonResponse(res, envelope(null, 401));
      return;
    }
    data = { initialised: true, sensor: 'OV2640', framesize: 10, quality: 12 };
  }
  else if (url.pathname === '/camera/stream') {
    state.streamRequests++;
    if (!req.headers.cookie?.includes(`visioncam_session=${sessionToken}`)) {
      res.writeHead(401).end();
      return;
    }
    const jpeg = Buffer.from(
      '/9j/4AAQSkZJRgABAQAAAQABAAD/2wBDAP//////////////////////////////////////////////////////////////////////////////////////2wBDAf//////////////////////////////////////////////////////////////////////////////////////wAARCAABAAEDASIAAhEBAxEB/8QAFQABAQAAAAAAAAAAAAAAAAAAAAb/xAAUEAEAAAAAAAAAAAAAAAAAAAAA/9oADAMBAAIQAxAAAAH/xAAUEAEAAAAAAAAAAAAAAAAAAAAA/9oACAEBAAEFAqf/xAAUEQEAAAAAAAAAAAAAAAAAAAAA/9oACAEDAQE/AV//xAAUEQEAAAAAAAAAAAAAAAAAAAAA/9oACAECAQE/AV//2Q==',
      'base64'
    );
    res.writeHead(200, {
      'Content-Type': 'multipart/x-mixed-replace; boundary=vcframe',
      'Cache-Control': 'no-cache'
    });
    const frame = Buffer.concat([
      Buffer.from(`--vcframe\r\nContent-Type: image/jpeg\r\nContent-Length: ${jpeg.length}\r\n\r\n`),
      jpeg,
      Buffer.from('\r\n')
    ]);
    res.write(Buffer.concat([frame, frame]));
    const streamTimer = setTimeout(() => res.end('\r\n--vcframe--\r\n'), 15000);
    res.on('close', () => clearTimeout(streamTimer));
    return;
  }
  else if (url.pathname === '/camera/settings') data = { framesize: 10, quality: 12 };
  else if (url.pathname === '/recording/status') data = { state: state.recordingState };
  else if (url.pathname === '/recording/config') data = { enabled: true, capture_fps: 10, segment_duration_s: 60 };
  else if (url.pathname === '/motion/config') data = { enabled: true, sample_rate_idle_hz: 5, sample_rate_rec_hz: 1, threshold: 25, min_changed_ratio: 5, cooldown_ms: 1000, min_frames: 2, settle_frames: 2, record_on_motion: false, snapshot_on_motion: true, mqtt_notify: false };
  else if (url.pathname === '/motion/status') data = { active: false, events_total: 0 };
  else if (url.pathname === '/storage' && url.searchParams.get('action') === 'status') data = { used_bytes: 0, free_bytes: 1000000, total_bytes: 1000000, media_count: 0 };
  else if (url.pathname === '/storage' && url.searchParams.get('action') === 'files') data = { items: [], count: 0, has_more: false };
  else if (url.pathname === '/storage' && url.searchParams.get('action') === 'retention') data = { enabled: false, max_used_percent: 90, retention_days_manual: 30, retention_days_motion: 30, retention_days_detection: 30, retention_days_timelapse: 30 };
  else if (url.pathname === '/recordings' && url.searchParams.get('action') === 'play') {
    const id = url.searchParams.get('id');
    state.playRequests.push(id);
    if (!req.headers.cookie?.includes(`visioncam_session=${sessionToken}`)) {
      res.writeHead(401, { 'Content-Type': 'application/json' }).end(
        JSON.stringify({ ok: false, error: { message: 'Sign in required' } })
      );
      return;
    }
    const jpeg = Buffer.from(
      '/9j/4AAQSkZJRgABAQAAAQABAAD/2wBDAP//////////////////////////////////////////////////////////////////////////////////////2wBDAf//////////////////////////////////////////////////////////////////////////////////////wAARCAABAAEDASIAAhEBAxEB/8QAFQABAQAAAAAAAAAAAAAAAAAAAAb/xAAUEAEAAAAAAAAAAAAAAAAAAAAA/9oADAMBAAIQAxAAAAH/xAAUEAEAAAAAAAAAAAAAAAAAAAAA/9oACAEBAAEFAqf/xAAUEQEAAAAAAAAAAAAAAAAAAAAA/9oACAEDAQE/AV//xAAUEQEAAAAAAAAAAAAAAAAAAAAA/9oACAECAQE/AV//2Q==',
      'base64'
    );
    const boundary = Buffer.from(
      `--frameboundary\r\nContent-Type: image/jpeg\r\nContent-Length: ${jpeg.length}\r\n\r\n`
    );
    res.writeHead(200, {
      'Content-Type': 'multipart/x-mixed-replace; boundary=frameboundary',
      'Cache-Control': 'no-cache'
    });
    res.end(Buffer.concat([boundary, jpeg, Buffer.from('\r\n--frameboundary--\r\n')]));
    return;
  } else if (url.pathname === '/recordings' && url.searchParams.get('action') === 'download') {
    const id = url.searchParams.get('id');
    state.downloads.push(id);
    if (!req.headers.cookie?.includes(`visioncam_session=${sessionToken}`)) {
      res.writeHead(401).end();
      return;
    }
    res.writeHead(200, {
      'Content-Type': 'video/x-msvideo',
      'Content-Disposition': `attachment; filename="${id}.avi"`
    });
    res.end(downloadBody);
    return;
  } else if (url.pathname === '/recordings') {
    data = {
      items: [{
        recording_id: 'rec-test-001',
        category: 'manual',
        size_bytes: 4096,
        duration_s: 12,
        start_time: 1730000000
      }],
      count: 1,
      has_more: false
    };
  }
  else if (url.pathname === '/integrations/mqtt') data = { enabled: false, status: { state: 'Disabled', connected: false } };
  else if (url.pathname === '/integrations/rtsp') data = { enabled: true, port: 554, username: 'homeassistant', password_set: true, path: '/stream', codec: 'MJPEG', transport: 'RTP/AVP/TCP', max_clients: 2, status: { running: true, clients: 1, sessions_total: 3, auth_failures: 0, frames_sent: 120, frames_dropped: 0, last_error: '' }, schema_version: 1 };
  else if (url.pathname === '/integrations/home-assistant/status') data = { enabled: false, connected: false, discovery_published: false, state: 'Disabled' };
  else if (url.pathname === '/integrations/network-storage') data = { enabled: false, available: false, state: 'Disabled', auth_token_configured: false };
  else if (url.pathname === '/integrations/network-storage/queue') data = { queue_depth: 0, uploads_ok: 0, uploads_failed: 0, queue_dropped: 0 };
  else if (url.pathname === '/metrics') data = { tasks: [] };
  jsonResponse(res, envelope(data));
});

server.listen(4173, '127.0.0.1');
