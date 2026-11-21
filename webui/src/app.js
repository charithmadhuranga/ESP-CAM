/* VisionCam Pro embedded UI. Keep this dependency-free and same-origin. */
'use strict';

const API = (() => {
  const waiters = [];
  let activeRequests = 0;
  const maxConcurrentRequests = 2;

  async function acquireSlot() {
    if (activeRequests < maxConcurrentRequests) {
      activeRequests++;
      return;
    }
    await new Promise((resolve) => waiters.push(resolve));
  }

  function releaseSlot() {
    const next = waiters.shift();
    if (next) next();
    else activeRequests--;
  }

  const csrfToken = () => {
    const item = document.cookie.split('; ').find((value) =>
      value.startsWith('visioncam_csrf='));
    return item ? decodeURIComponent(item.slice('visioncam_csrf='.length)) : '';
  };

  async function request(path, method = 'GET', body) {
    const headers = { Accept: 'application/json' };
    if (body !== undefined) headers['Content-Type'] = 'application/json';
    if (method !== 'GET' && method !== 'HEAD') {
      const token = csrfToken();
      if (token) headers['X-CSRF-Token'] = token;
    }

    await acquireSlot();
    try {
      const response = await fetch(path, {
        method,
        credentials: 'same-origin',
        cache: 'no-store',
        headers,
        body: body === undefined ? undefined : JSON.stringify(body)
      });
      let envelope;
      try {
        envelope = await response.json();
      } catch {
        return { ok: false, error: { message: `Unexpected response (${response.status})` } };
      }
      if (!response.ok || envelope.ok !== true) {
        if (response.status === 401) UI.showLogin('Your session expired. Sign in again.');
        return {
          ok: false,
          error: envelope.error || { message: `Request failed (${response.status})` },
          request_id: envelope.request_id
        };
      }
      return envelope;
    } catch (error) {
      return { ok: false, error: { message: error.message || 'Device unavailable' } };
    } finally {
      releaseSlot();
    }
  }

  const query = (path, values) => `${path}?${new URLSearchParams(values)}`;
  const idQuery = (path, action, id) => query(path, { action, id });
  return {
    request,
    login: (username, password) => request('/auth/login', 'POST', { username, password }),
    session: () => request('/auth/session'),
    logout: () => request('/auth/logout', 'POST', {}),
    completeFirstLogin: (current_password, new_password) =>
      request('/auth/complete-first-login', 'POST', { current_password, new_password }),
    changePassword: (current_password, new_password, new_username) =>
      request('/auth/change-password', 'POST', {
        current_password, new_password, ...(new_username ? { new_username } : {})
      }),
    device: () => request('/device'),
    status: () => request('/status'),
    health: () => request('/health'),
    metrics: () => request('/metrics'),
    provisionWifi: (ssid, password) =>
      request('/wifi/config', 'POST', { ssid, password }),
    restart: () => request('/device/restart', 'POST', {}),
    cameraStatus: () => request('/camera/status'),
    cameraSettings: () => request('/camera/settings'),
    saveCameraSettings: (settings) => request('/camera/settings', 'PUT', settings),
    captureFrame: async () => {
      const headers = {};
      const token = csrfToken();
      if (token) headers['X-CSRF-Token'] = token;
      const response = await fetch('/camera/capture', {
        method: 'POST', credentials: 'same-origin', cache: 'no-store', headers
      });
      if (!response.ok) {
        if (response.status === 401) UI.showLogin('Your session expired. Sign in again.');
        let message = `Camera capture failed (${response.status})`;
        try {
          const envelope = await response.json();
          message = envelope.error?.message || message;
        } catch { /* The firmware may return a non-JSON transport error. */ }
        throw new Error(message);
      }
      return response.blob();
    },
    recordingStatus: () => request('/recording/status'),
    recordingConfig: () => request('/recording/config'),
    saveRecordingConfig: (config) => request('/recording/config', 'PUT', config),
    startRecording: () => request('/recording/start', 'POST', {}),
    stopRecording: () => request('/recording/stop', 'POST', {}),
    recordings: (offset = 0, limit = 12) =>
      request(query('/recordings', { offset, limit })),
    recordingMetadata: (id) => request(idQuery('/recordings', 'metadata', id)),
    recordingDownloadUrl: (id) => idQuery('/recordings', 'download', id),
    recordingPlayUrl: (id) => idQuery('/recordings', 'play', id),
    deleteRecording: (id) => request(query('/recordings', { id }), 'DELETE'),
    captureSnapshot: () => request('/snapshot', 'POST', {}),
    snapshotImageUrl: (id) => query('/snapshot', { action: 'image', id }),
    storageStatus: () => request('/storage?action=status'),
    media: (offset = 0, limit = 12, category) =>
      request(query('/storage', {
        action: 'files', offset, limit, ...(category == null ? {} : { category })
      })),
    mediaMetadata: (id) => request(idQuery('/storage', 'file', id)),
    deleteMedia: (id) => request(idQuery('/storage', 'file', id), 'DELETE'),
    rebuildIndex: () => request('/storage?action=rebuild', 'POST', {}),
    retention: () => request('/storage?action=retention'),
    saveRetention: (config) => request('/storage?action=retention', 'PUT', config),
    retentionPreview: () => request('/storage?action=retention-preview'),
    runRetention: () => request('/storage?action=retention-run', 'POST', {}),
    motionConfig: () => request('/motion/config'),
    saveMotionConfig: (config) => request('/motion/config', 'PUT', config),
    motionStatus: () => request('/motion/status'),
    mqttConfig: () => request('/integrations/mqtt'),
    saveMqttConfig: (config) => request('/integrations/mqtt', 'PUT', config),
    testMqtt: () => request('/integrations/mqtt/test', 'POST', {}),
    reconnectMqtt: () => request('/integrations/mqtt/reconnect', 'POST', {}),
    testPublish: () => request('/integrations/mqtt/test-publish', 'POST', {}),
    haStatus: () => request('/integrations/home-assistant/status'),
    republishDiscovery: () =>
      request('/integrations/home-assistant/discovery', 'POST', {}),
    networkStorage: () => request('/integrations/network-storage'),
    saveNetworkStorage: (config) =>
      request('/integrations/network-storage', 'PUT', config),
    testNetworkStorage: () =>
      request('/integrations/network-storage/test', 'POST', {}),
    networkStorageQueue: () => request('/integrations/network-storage/queue')
  };
})();

const UI = {
  page: 'overview',
  pollTimer: null,
  frameUrl: null,
  storageOffset: 0,
  recordingOffset: 0,
  mediaHasMore: false,
  recordingsHasMore: false,
  mediaItems: [],
  recordingItems: [],

  el(id) { return document.getElementById(id); },

  messageOf(result, fallback = 'Request failed') {
    return result?.error?.message || fallback;
  },

  toast(message, type = 'info') {
    const toast = this.el('toast');
    toast.textContent = message;
    toast.dataset.type = type;
    toast.hidden = false;
    clearTimeout(this.toastTimer);
    this.toastTimer = setTimeout(() => { toast.hidden = true; }, 3200);
  },

  showLogin(message = '') {
    clearInterval(this.pollTimer);
    this.pollTimer = null;
    this.stopStream();
    this.el('app').hidden = true;
    this.el('login-page').hidden = false;
    this.el('first-login-page').hidden = true;
    this.el('login-error').textContent = message;
    this.el('login-error').hidden = !message;
  },

  showFirstLogin() {
    this.el('login-page').hidden = true;
    this.el('app').hidden = true;
    this.el('first-login-page').hidden = false;
  },

  async enterApp() {
    this.el('login-page').hidden = true;
    this.el('first-login-page').hidden = true;
    this.el('app').hidden = false;
    await this.navigate('overview');
    this.startPolling();
  },

  async navigate(page) {
    const pages = ['overview', 'live', 'snapshots', 'recordings', 'storage', 'settings', 'integrations'];
    if (!pages.includes(page)) return;
    this.page = page;
    document.querySelectorAll('[data-page]').forEach((item) => {
      const active = item.dataset.page === page;
      item.classList.toggle('active', active);
      if (item.matches('button')) item.setAttribute('aria-current', active ? 'page' : 'false');
    });
    pages.forEach((name) => {
      this.el(`page-${name}`).hidden = name !== page;
    });
    this.el('page-title').textContent = ({
      overview: 'Overview', live: 'Live view', snapshots: 'Media library',
      recordings: 'Recordings', storage: 'Storage', settings: 'Settings',
      integrations: 'Integrations'
    })[page];
    this.el('sidebar').classList.remove('open');
    if (page !== 'live') this.stopStream();
    if (page === 'overview') await this.loadOverview();
    if (page === 'live') await this.startStream();
    if (page === 'snapshots') {
      this.storageOffset = 0;
      await this.loadMedia();
    }
    if (page === 'recordings') {
      this.recordingOffset = 0;
      await this.loadRecordings();
    }
    if (page === 'storage') await this.loadStoragePage();
    if (page === 'settings') await this.loadSettings();
    if (page === 'integrations') await this.loadIntegrations();
  },

  async data(pathRequest) {
    const result = await pathRequest;
    if (!result.ok) throw new Error(this.messageOf(result));
    return result.data;
  },

  async loadOverview() {
    try {
      const [device, status, health, camera, recording, motion, storage, ha] = await Promise.all([
        this.data(API.device()), this.data(API.status()), this.data(API.health()),
        this.data(API.cameraStatus()), this.data(API.recordingStatus()),
        this.data(API.motionStatus()), this.data(API.storageStatus()),
        this.data(API.haStatus())
      ]);
      this.setText('device-name', device.device_name || 'VisionCam Pro');
      this.setText('firmware-version', device.firmware_version || 'Firmware');
      this.setText('footer-firmware', `VisionCam Pro · ${device.firmware_version || 'firmware'}`);
      this.setText('stat-camera', camera.initialised ? 'Online' : 'Unavailable');
      this.setText('stat-recording', recording.state || 'Unknown');
      this.setText('stat-motion', motion.events_total ?? 0);
      this.setText('stat-sd', this.formatBytes(storage.used_bytes));
      this.setText('stat-wifi', `${status.wifi?.state || 'Unknown'}${Number.isFinite(status.wifi?.rssi) && status.wifi.rssi < 0 ? ` · ${status.wifi.rssi} dBm` : ''}`);
      this.setText('stat-uptime', `${status.time_synced ? 'Clock synced' : 'Clock not synced'}`);
      this.setText('stat-health', health.healthy ? 'Healthy' : 'Degraded');
      this.setText('stat-mqtt', ha.connected ? 'Connected' : 'Offline');
      this.setText('stat-heap', this.formatBytes(status.heap_free_bytes));
      this.setText('stat-sd-detail', `${this.formatBytes(storage.free_bytes)} free · ${storage.media_count} items`);
      this.setStatus('device-state', health.healthy ? 'Online' : 'Degraded', health.healthy ? 'good' : 'warn');
      this.setStatus('mqtt-badge', ha.connected ? 'MQTT connected' : 'MQTT offline', ha.connected ? 'good' : 'muted');
      this.setStatus('recording-badge', recording.state === 'recording' ? 'Recording' : 'Not recording', recording.state === 'recording' ? 'warn' : 'muted');
      this.setText('overview-detail', `${camera.sensor || 'Camera'} · ${camera.framesize} · JPEG quality ${camera.quality}`);
    } catch (error) {
      this.setStatus('device-state', 'Status unavailable', 'bad');
      this.toast(error.message, 'error');
    }
  },

  async loadMetrics() {
    try {
      const metrics = await this.data(API.metrics());
      const rows = [
        ['Uptime', `${metrics.uptime_s ?? 0} s`],
        ['Minimum heap', this.formatBytes(metrics.heap_min_bytes)],
        ['Largest block', this.formatBytes(metrics.heap_largest_bytes)],
        ['Free PSRAM', this.formatBytes(metrics.psram_free_bytes)],
        ['Frames captured', metrics.frame_captured ?? 0],
        ['Dropped frames', (metrics.frame_invalid_dropped || 0) + (metrics.frame_pool_dropped || 0) + (metrics.frame_queue_dropped || 0)],
        ['Camera recoveries', metrics.cam_recovered ?? 0],
        ['Boot count', metrics.boot_count ?? 0]
      ];
      const body = this.el('metrics-body');
      body.replaceChildren(...rows.map(([name, value]) => this.row(name, value)));
      const tasks = metrics.tasks || [];
      const taskBody = this.el('tasks-body');
      taskBody.replaceChildren(...tasks.map((task) => {
        const row = document.createElement('tr');
        [task.n, `${task.hwm} B`, task.prio, task.core, task.state]
          .forEach((value) => row.append(this.cell(value)));
        return row;
      }));
      this.toast('Diagnostics refreshed', 'success');
    } catch (error) { this.toast(error.message, 'error'); }
  },

  async startStream() {
    this.stopStream();
    const image = this.el('live-stream');
    this.el('stream-state').textContent = 'Connecting…';
    image.onload = () => { this.el('stream-state').textContent = 'Live'; };
    image.onerror = () => { this.el('stream-state').textContent = 'Unavailable'; };
    image.src = '/camera/stream';
  },

  stopStream() {
    const image = this.el('live-stream');
    if (image) {
      image.onload = null;
      image.onerror = null;
      image.removeAttribute('src');
    }
    if (this.frameUrl) {
      URL.revokeObjectURL(this.frameUrl);
      this.frameUrl = null;
    }
  },

  async captureFrame() {
    try {
      const blob = await API.captureFrame();
      if (this.frameUrl) URL.revokeObjectURL(this.frameUrl);
      this.frameUrl = URL.createObjectURL(blob);
      this.el('capture-preview').src = this.frameUrl;
      this.el('capture-preview').hidden = false;
      this.toast('Frame captured', 'success');
    } catch (error) { this.toast(error.message, 'error'); }
  },

  async captureSnapshot() {
    try {
      const data = await this.data(API.captureSnapshot());
      this.toast(data.suppressed ? 'Snapshot cooldown is active' : `Snapshot saved${data.media_id ? ` · ${data.media_id}` : ''}`, data.suppressed ? 'info' : 'success');
      if (this.page === 'snapshots' || this.page === 'storage') await this.loadMedia();
    } catch (error) { this.toast(error.message, 'error'); }
  },

  async toggleRecording() {
    try {
      const { state } = await this.data(API.recordingStatus());
      const result = state === 'recording'
        ? await this.data(API.stopRecording())
        : await this.data(API.startRecording());
      this.toast(state === 'recording' ? 'Recording stopped' : `Recording started${result.recording_id ? ` · ${result.recording_id}` : ''}`, 'success');
      await this.loadOverview();
      if (this.page === 'recordings') await this.loadRecordings();
    } catch (error) { this.toast(error.message, 'error'); }
  },

  async loadMedia() {
    try {
      const page = await this.data(API.media(this.storageOffset, 12));
      this.mediaItems = page.items || [];
      this.mediaHasMore = Boolean(page.has_more);
      this.renderMedia();
      this.setText('media-page-label', `Page ${Math.floor(this.storageOffset / 12) + 1}`);
      this.el('media-prev').disabled = this.storageOffset === 0;
      this.el('media-next').disabled = !this.mediaHasMore;
      this.setText('storage-page-label', `Page ${Math.floor(this.storageOffset / 12) + 1}`);
      this.el('storage-media-prev').disabled = this.storageOffset === 0;
      this.el('storage-media-next').disabled = !this.mediaHasMore;
      this.setStatus('media-count', `${page.count || 0} shown`, 'muted');
    } catch (error) {
      this.setStatus('media-count', error.message, 'bad');
      this.toast(error.message, 'error');
    }
  },

  renderMedia() {
    ['media-body', 'storage-media-body'].forEach((id) => {
      const body = this.el(id);
      if (!body) return;
      if (!this.mediaItems.length) {
        body.replaceChildren(this.emptyRow(6, 'No media found on the SD card'));
        return;
      }
      body.replaceChildren(...this.mediaItems.map((item) => {
        const row = document.createElement('tr');
        row.append(this.cell(item.media_id), this.cell(this.mediaType(item.type)), this.cell(this.category(item.category)),
          this.cell(this.formatBytes(item.size)), this.cell(this.formatDate(item.created_at)));
        const actions = document.createElement('td');
        if (Number(item.type) === 0) {
          actions.append(this.actionButton('View', 'view-media', item.media_id));
        }
        actions.append(this.actionButton('Details', 'media-meta', item.media_id),
          this.actionButton('Delete', 'delete-media', item.media_id, 'danger'));
        row.append(actions);
        return row;
      }));
    });
  },

  async loadRecordings() {
    try {
      const page = await this.data(API.recordings(this.recordingOffset, 12));
      this.recordingItems = page.items || [];
      this.recordingsHasMore = Boolean(page.has_more);
      const body = this.el('recordings-body');
      body.replaceChildren(...(this.recordingItems.length ? this.recordingItems.map((item) => {
        const row = document.createElement('tr');
        row.append(this.cell(item.recording_id), this.cell(item.category), this.cell(this.formatBytes(item.size_bytes)),
          this.cell(`${item.duration_s}s`), this.cell(this.formatDate(item.start_time * 1000)));
        const actions = document.createElement('td');
        actions.append(this.actionButton('Play', 'play-recording', item.recording_id),
          this.actionButton('Download', 'download-recording', item.recording_id),
          this.actionButton('Details', 'recording-meta', item.recording_id),
          this.actionButton('Delete', 'delete-recording', item.recording_id, 'danger'));
        row.append(actions);
        return row;
      }) : [this.emptyRow(6, 'No recordings found')]));
      this.setText('recording-page-label', `Page ${Math.floor(this.recordingOffset / 12) + 1}`);
      this.el('recording-prev').disabled = this.recordingOffset === 0;
      this.el('recording-next').disabled = !this.recordingsHasMore;
    } catch (error) { this.toast(error.message, 'error'); }
  },

  async loadStoragePage() {
    try {
      const [storage, policy, queue] = await Promise.all([
        this.data(API.storageStatus()), this.data(API.retention()), this.data(API.networkStorageQueue())
      ]);
      this.setText('storage-summary', `${this.formatBytes(storage.used_bytes)} used of ${this.formatBytes(storage.total_bytes)}`);
      this.setText('storage-free', `${this.formatBytes(storage.free_bytes)} free · ${storage.media_count} indexed files`);
      this.fillForm('retention-form', policy);
      this.setText('retention-status', `Automatic retention ${policy.enabled ? 'enabled' : 'disabled'}`);
      this.setText('upload-queue', `${queue.queue_depth} pending · ${queue.uploads_ok} uploaded · ${queue.uploads_failed} failed · ${queue.queue_dropped} dropped`);
      this.storageOffset = 0;
      await this.loadMedia();
    } catch (error) { this.toast(error.message, 'error'); }
  },

  async previewRetention() {
    try {
      const preview = await this.data(API.retentionPreview());
      const items = preview.candidates || [];
      const text = items.length
        ? items.map((item) => `${item.media_id} · ${this.formatBytes(item.size)} · ${item.age_days} days`).join('\n')
        : 'No eligible media files would be removed.';
      this.el('retention-preview').textContent = `${preview.count || 0} candidates\n${text}`;
      this.toast('Retention preview ready', 'success');
    } catch (error) { this.toast(error.message, 'error'); }
  },

  async loadSettings() {
    try {
      const [camera, recording, motion, deviceStatus] = await Promise.all([
        this.data(API.cameraSettings()), this.data(API.recordingConfig()),
        this.data(API.motionConfig()), this.data(API.status())
      ]);
      this.fillForm('camera-form', camera);
      this.fillForm('recording-form', recording);
      this.fillForm('motion-form', motion);
      const wifi = deviceStatus.wifi || {};
      this.setText('wifi-setup-status',
        `${wifi.state || 'Unknown'}${wifi.ap_mode ? ' · Setup access point active' : ''}`);
      this.el('wifi-form').hidden = Boolean(wifi.configured || wifi.provision_pending);
      this.setText('wifi-setup-note', wifi.provision_pending
        ? 'Network verified. Restart the camera from this page to apply the credentials safely before camera startup.'
        : wifi.configured
          ? 'Wi-Fi credentials are configured. Restart the camera to leave setup AP mode.'
          : 'Connect to a 2.4 GHz Wi-Fi network. Credentials are tested before they are stored.');
      const status = await this.data(API.motionStatus());
      this.setText('motion-live-status', `${status.active ? 'Motion currently detected' : 'No active motion'} · ${status.events_total} events`);
    } catch (error) { this.toast(error.message, 'error'); }
  },

  async loadIntegrations() {
    try {
      const [mqtt, ha, netstore, queue] = await Promise.all([
        this.data(API.mqttConfig()), this.data(API.haStatus()),
        this.data(API.networkStorage()), this.data(API.networkStorageQueue())
      ]);
      this.fillForm('mqtt-form', mqtt);
      this.el('mqtt-form').elements.password.value = '';
      this.el('mqtt-form').elements.password.placeholder = mqtt.password_set ? 'Configured — leave blank to keep' : 'Not configured';
      this.el('mqtt-client-id').value = mqtt.client_id || 'Assigned by firmware';
      this.setText('mqtt-live-status', `${mqtt.status?.state || 'Unknown'} · ${mqtt.status?.connected ? 'Connected' : 'Disconnected'}`);
      this.setText('mqtt-connection-detail',
        `${mqtt.status?.connects || 0} connects · ${mqtt.status?.disconnects || 0} disconnects · ${mqtt.status?.published || 0} published · ${mqtt.status?.dropped || 0} dropped${mqtt.status?.last_error ? ` · ${mqtt.status.last_error}` : ''}`);
      this.setText('ha-live-status', `${ha.enabled ? 'Discovery enabled' : 'Discovery disabled'} · ${ha.discovery_published ? 'Published' : 'Not published'} · ${ha.state || 'Unknown'}`);
      this.setText('network-live-status', `${netstore.state || 'Disabled'} · ${netstore.available ? 'Available' : 'Unavailable'}`);
      this.setText('network-queue-status', `${queue.queue_depth} queued · ${queue.uploads_ok} successful · ${queue.uploads_failed} failed`);
      this.setText('network-stat-detail',
        `${netstore.uploads_total} total uploads · ${netstore.queue_dropped} dropped from queue${netstore.last_error ? ` · ${netstore.last_error}` : ''}`);
      this.fillForm('network-form', netstore);
      this.el('network-form').elements.auth_token.value = '';
      this.el('network-form').elements.auth_token.placeholder =
        netstore.auth_token_configured ? 'Configured — leave blank to keep' : 'Optional token';
    } catch (error) { this.toast(error.message, 'error'); }
  },

  fillForm(formId, values) {
    const form = this.el(formId);
    Object.entries(values || {}).forEach(([name, value]) => {
      const input = form.elements.namedItem(name);
      if (!input || value === null || value === undefined) return;
      if (input.type === 'checkbox') input.checked = Boolean(value);
      else if (input.type !== 'password') input.value = value;
      if (name === 'quality') this.setText('quality-value', value);
    });
  },

  formValues(form, numericFields = []) {
    const values = {};
    for (const [name, value] of new FormData(form).entries()) {
      const input = form.elements.namedItem(name);
      if (input?.type === 'checkbox') continue;
      values[name] = numericFields.includes(name) ? Number(value) : value;
    }
    form.querySelectorAll('input[type="checkbox"][name]').forEach((input) => {
      values[input.name] = input.checked;
    });
    return values;
  },

  async saveForm(form, request, numericFields = []) {
    const button = form.querySelector('[type="submit"]');
    if (button) button.disabled = true;
    try {
      const result = await request(this.formValues(form, numericFields));
      if (!result.ok) throw new Error(this.messageOf(result));
      this.toast('Settings saved', 'success');
      return result.data;
    } catch (error) {
      this.toast(error.message, 'error');
      return null;
    } finally { if (button) button.disabled = false; }
  },

  async savePassword(form, firstLogin = false) {
    const values = this.formValues(form);
    if (values.new_password !== values.confirm_password) {
      this.toast('New passwords do not match', 'error');
      return;
    }
    const result = await (firstLogin
      ? API.completeFirstLogin(values.current_password, values.new_password)
      : API.changePassword(values.current_password, values.new_password, values.new_username));
    if (!result.ok) {
      this.toast(this.messageOf(result), 'error');
      return;
    }
    form.reset();
    if (firstLogin) await this.enterApp();
    else {
      this.toast('Password updated. Sign in again.', 'success');
      await API.logout();
      this.showLogin('Password updated. Sign in with your new credentials.');
    }
  },

  async runAction(action, id) {
    try {
      if (action === 'view-media') {
        const image = this.el('media-preview');
        image.src = API.snapshotImageUrl(id);
        image.hidden = false;
        this.el('media-preview-id').textContent = id;
        this.el('media-dialog').showModal();
      } else if (action === 'media-meta') {
        const data = await this.data(API.mediaMetadata(id));
        this.showDetails(data);
      } else if (action === 'recording-meta') {
        this.showDetails(await this.data(API.recordingMetadata(id)));
      } else if (action === 'download-recording') {
        window.location.assign(API.recordingDownloadUrl(id));
      } else if (action === 'play-recording') {
        this.el('playback-title').textContent = `Playback · ${id}`;
        this.el('playback-image').src = `${API.recordingPlayUrl(id)}&t=${Date.now()}`;
        this.el('playback-dialog').showModal();
      } else if (action === 'delete-media') {
        if (!window.confirm(`Delete media ${id}? This cannot be undone.`)) return;
        await this.data(API.deleteMedia(id));
        this.toast('Media deleted', 'success');
        await this.loadMedia();
      } else if (action === 'delete-recording') {
        if (!window.confirm(`Delete recording ${id}? This cannot be undone.`)) return;
        await this.data(API.deleteRecording(id));
        this.toast('Recording deleted', 'success');
        await this.loadRecordings();
      }
    } catch (error) { this.toast(error.message, 'error'); }
  },

  showDetails(data) {
    const output = this.el('details-output');
    output.textContent = JSON.stringify(data, null, 2);
    this.el('details-dialog').showModal();
  },

  async testAction(request, successMessage) {
    try {
      const result = await this.data(request());
      this.toast(successMessage(result), 'success');
      return result;
    } catch (error) {
      this.toast(error.message, 'error');
      return null;
    }
  },

  setText(id, value) {
    const element = this.el(id);
    if (element) element.textContent = value == null ? '—' : String(value);
  },

  setStatus(id, value, kind) {
    const element = this.el(id);
    if (!element) return;
    element.textContent = value;
    element.dataset.state = kind;
  },

  formatBytes(value) {
    const n = Number(value);
    if (!Number.isFinite(n)) return '—';
    if (n < 1024) return `${n} B`;
    const units = ['KB', 'MB', 'GB', 'TB'];
    let amount = n / 1024;
    let index = 0;
    while (amount >= 1024 && index < units.length - 1) {
      amount /= 1024;
      index++;
    }
    return `${amount.toFixed(amount < 10 ? 1 : 0)} ${units[index]}`;
  },

  formatDate(value) {
    if (!value) return '—';
    const date = typeof value === 'number' ? new Date(value) : new Date(value);
    return Number.isNaN(date.getTime()) ? String(value) : date.toLocaleString();
  },

  mediaType(value) {
    return ({ 0: 'Snapshot', 1: 'Recording', 2: 'Thumbnail', 3: 'Timelapse frame', 4: 'Timelapse output', 5: 'Model', 6: 'Log' })[value] || `Type ${value}`;
  },

  category(value) {
    return ({ 0: 'Manual', 1: 'Motion', 2: 'Detection', 3: 'Timelapse' })[value] || `Category ${value}`;
  },

  cell(value) {
    const td = document.createElement('td');
    td.textContent = value == null ? '—' : String(value);
    return td;
  },

  emptyRow(span, message) {
    const row = document.createElement('tr');
    const cell = this.cell(message);
    cell.colSpan = span;
    cell.className = 'empty-cell';
    row.append(cell);
    return row;
  },

  row(label, value) {
    const row = document.createElement('tr');
    row.append(this.cell(label), this.cell(value));
    return row;
  },

  actionButton(label, action, id, style = '') {
    const button = document.createElement('button');
    button.type = 'button';
    button.className = `button button-small ${style ? `button-${style}` : 'button-secondary'}`;
    button.textContent = label;
    button.dataset.action = action;
    button.dataset.id = id;
    return button;
  },

  startPolling() {
    clearInterval(this.pollTimer);
    this.pollTimer = setInterval(() => {
      if (document.hidden) return;
      if (this.page === 'overview') this.loadOverview();
    }, 15000);
  },

  async onFormSubmit(event) {
    const form = event.target;
    event.preventDefault();
    if (form.id === 'login-form') {
      const result = await API.login(form.elements.username.value, form.elements.password.value);
      if (!result.ok) {
        this.el('login-error').textContent = this.messageOf(result, 'Invalid username or password');
        this.el('login-error').hidden = false;
      } else if (result.data?.password_change_required) {
        this.showFirstLogin();
      } else {
        await this.enterApp();
      }
      form.elements.password.value = '';
      return;
    }
    if (form.id === 'first-login-form') return this.savePassword(form, true);
    if (form.id === 'password-form') return this.savePassword(form, false);
    if (form.id === 'wifi-form') {
      const submit = form.querySelector('[type="submit"]');
      submit.disabled = true;
      try {
        const result = await API.provisionWifi(
          form.elements.ssid.value,
          form.elements.password.value
        );
        if (!result.ok) throw new Error(this.messageOf(result));
        form.elements.password.value = '';
        form.hidden = true;
        this.setText('wifi-setup-status', 'Connected · restarting');
        this.setText('wifi-setup-note',
          'Network verified. The setup hotspot is closing and the camera is restarting to save credentials. Reconnect to your Wi-Fi network to access the camera.');
        this.toast('Wi-Fi connected; camera restarting', 'success');
      } catch (error) {
        this.toast(error.message, 'error');
      } finally {
        submit.disabled = false;
      }
      return;
    }
    const actions = {
      'camera-form': [API.saveCameraSettings, ['framesize', 'quality']],
      'recording-form': [API.saveRecordingConfig, ['capture_fps', 'segment_duration_s']],
      'motion-form': [API.saveMotionConfig, ['sample_rate_idle_hz', 'sample_rate_rec_hz', 'threshold', 'min_changed_ratio', 'cooldown_ms', 'min_frames', 'settle_frames']],
      'retention-form': [API.saveRetention, ['max_used_percent', 'retention_days_manual', 'retention_days_motion', 'retention_days_detection', 'retention_days_timelapse']],
      'mqtt-form': [API.saveMqttConfig, ['keepalive_s', 'timeout_ms']],
      'network-form': [API.saveNetworkStorage, ['retry_count', 'retry_interval_ms', 'upload_queue_limit']]
    };
    const config = actions[form.id];
    if (!config) return;
    await this.saveForm(form, config[0], config[1]);
    if (form.id === 'mqtt-form' || form.id === 'network-form') await this.loadIntegrations();
  },

  bind() {
    document.addEventListener('submit', (event) => this.onFormSubmit(event));
    document.addEventListener('click', (event) => {
      const nav = event.target.closest('[data-page]');
      if (nav) {
        event.preventDefault();
        return this.navigate(nav.dataset.page);
      }
      const action = event.target.closest('[data-action]');
      if (action) return this.runAction(action.dataset.action, action.dataset.id);
      const button = event.target.closest('[data-command]');
      if (!button) return;
      const command = button.dataset.command;
      if (command === 'logout') {
        API.logout().finally(() => this.showLogin());
      } else if (command === 'capture-frame') this.captureFrame();
      else if (command === 'capture-snapshot') this.captureSnapshot();
      else if (command === 'toggle-recording') this.toggleRecording();
      else if (command === 'refresh-media') this.loadMedia();
      else if (command === 'refresh-recordings') this.loadRecordings();
      else if (command === 'media-prev') {
        this.storageOffset = Math.max(0, this.storageOffset - 12);
        this.loadMedia();
      } else if (command === 'media-next') {
        if (this.mediaHasMore) { this.storageOffset += 12; this.loadMedia(); }
      } else if (command === 'recording-prev') {
        this.recordingOffset = Math.max(0, this.recordingOffset - 12);
        this.loadRecordings();
      } else if (command === 'recording-next') {
        if (this.recordingsHasMore) { this.recordingOffset += 12; this.loadRecordings(); }
      } else if (command === 'storage-media-prev') {
        this.storageOffset = Math.max(0, this.storageOffset - 12);
        this.loadMedia();
      } else if (command === 'storage-media-next') {
        if (this.mediaHasMore) { this.storageOffset += 12; this.loadMedia(); }
      } else if (command === 'metrics') this.loadMetrics();
      else if (command === 'retention-preview') this.previewRetention();
      else if (command === 'retention-run') {
        if (window.confirm('Run retention now? Eligible media may be deleted.')) {
          this.testAction(API.runRetention, (result) => `${result.deleted} files deleted`)
            .then(() => this.loadStoragePage());
        }
      } else if (command === 'rebuild-index') {
        if (window.confirm('Rebuild media indexes from the SD card?')) {
          this.testAction(API.rebuildIndex, () => 'Media indexes rebuilt')
            .then(() => this.loadStoragePage());
        }
      } else if (command === 'mqtt-test') this.testAction(API.testMqtt, () => 'MQTT broker test succeeded');
      else if (command === 'mqtt-reconnect') this.testAction(API.reconnectMqtt, (r) => r.reconnected ? 'MQTT reconnected' : 'Reconnect requested; broker unavailable');
      else if (command === 'mqtt-publish') this.testAction(API.testPublish, () => 'Test message published');
      else if (command === 'ha-republish') this.testAction(API.republishDiscovery, () => 'Home Assistant discovery republished');
      else if (command === 'network-test') this.testAction(API.testNetworkStorage, () => 'Network storage server reachable');
      else if (command === 'restart') {
        if (window.confirm('Restart VisionCam Pro now? Active streams will disconnect.')) {
          this.testAction(API.restart, () => 'Device restarting');
        }
      } else if (command === 'close-dialog') event.target.closest('dialog')?.close();
      else if (command === 'menu') this.el('sidebar').classList.toggle('open');
      else if (command === 'refresh-settings') this.loadSettings();
      else if (command === 'refresh-integrations') this.loadIntegrations();
    });

    document.addEventListener('input', (event) => {
      if (event.target.name === 'framesize' || event.target.name === 'quality') {
        const label = this.el('quality-value');
        if (label && event.target.name === 'quality') label.textContent = event.target.value;
      }
    });

    window.addEventListener('beforeunload', () => this.stopStream());
  },

  async init() {
    this.bind();
    const result = await API.session();
    if (!result.ok || !result.data?.authenticated) {
      this.showLogin();
      return;
    }
    if (result.data.password_change_required) {
      this.showFirstLogin();
      return;
    }
    await this.enterApp();
  }
};

window.VisionCamAPI = API;
window.VisionCamUI = UI;
document.addEventListener('DOMContentLoaded', () => UI.init());
