// VisionCam Pro — API types (SPEC §6, API contract skill).
// Every JSON response uses the {ok, data, error, request_id} envelope.

export interface Envelope<T = unknown> {
  ok: boolean;
  data: T | null;
  error: { code: string; message: string } | null;
  request_id: string;
}

export interface ErrBody {
  code: string;
  message: string;
}

// --- Auth (SPEC §6.1) -------------------------------------------------------
export interface SessionView {
  authenticated: boolean;
  username: string;
  password_change_required: boolean;
}

export interface LoginRequest {
  username: string;
  password: string;
}

export interface CompleteFirstLoginRequest {
  current_password: string;
  new_password: string;
}

export interface ChangePasswordRequest {
  current_password: string;
  new_password?: string;
  new_username?: string;
}

// --- Network (from network_manager status) ----------------------------------
export type WifiState = "Off" | "Connecting" | "Connected" | "Disconnected";

export interface WifiStatus {
  state: WifiState;
  configured: boolean;
  radio_up: boolean;
  rssi: number;
  connects: number;
  disconnects: number;
}

// --- Camera (SPEC §6.3) -----------------------------------------------------
export interface CameraStatus {
  initialised: boolean;
  sensor: string;
  framesize: number;      // framesize_t
  quality: number;        // 0..63
  jpeg_capacity_bytes: number;
}

export interface CameraSettings {
  framesize: number;
  quality: number;
}

// --- Device / status / health / metrics ------------------------------------
export interface DeviceInfo {
  device_id: string;
  device_name: string;
  firmware_version: string;
  schema_version: number;
}

export interface StatusPayload {
  wifi: WifiStatus;
  camera: { initialised: boolean; framesize: number };
  storage: { mounted: boolean };
  time_synced: boolean;
  heap_free_bytes: number;
  largest_block_bytes: number;
  schema_version: number;
}

export interface HealthPayload {
  healthy: boolean;
  camera_initialised: boolean;
  wifi_state: WifiState;
  schema_version: number;
}

export interface MetricsPayload {
  uptime_s: number;
  heap_free_bytes: number;
  heap_min_bytes: number;
  heap_largest_bytes: number;
  psram_free_bytes: number;
  psram_largest_bytes: number;
  abnormal_boots: number;
  boot_count: number;
  frame_captured: number;
  frame_invalid_dropped: number;
  frame_pool_dropped: number;
  frame_queue_dropped: number;
  frame_leases_taken: number;
  frame_leases_released: number;
  cam_null_frames: number;
  cam_recovery_attempts: number;
  cam_recovered: number;
  cam_failed_events: number;
  schema_version: number;
}

export interface RestartResponse {
  restarting: boolean;
}

// --- Storage / snapshots (SPEC §6.5, §6.7) -----------------------------------

export type MediaType = 0 | 1 | 2 | 3; // snapshot | motion | detection | timelapse
export type MediaCategory = "manual" | "motion" | "detection" | "timelapse";

export interface MediaRecord {
  media_id: string;
  type: MediaType;
  category: MediaCategory;
  mime: string;
  size: number;
  created_at: string;
  event_id: string;
  duration_ms: number;
  status: string;
}

export interface MediaListResponse {
  has_more: boolean;
  count: number;
  offset: number;
  limit: number;
  items: MediaRecord[];
}

export interface MediaMetadata {
  media_id: string;
  type: MediaType;
  category: MediaCategory;
  mime: string;
  size: number;
  created_at: string;
  event_id: string;
  duration_ms: number;
  status: string;
  schema_version: number;
}

export interface RetentionPolicy {
  retention_days_manual: number;
  retention_days_motion: number;
  retention_days_detection: number;
  retention_days_timelapse: number;
  max_used_percent: number;
  enabled: boolean;
  schema_version?: number;
}

export interface DeletionCandidate {
  media_id: string;
  category: MediaCategory;
  path: string;
  size: number;
  age_days: number;
}

export interface RetentionPreviewResponse {
  count: number;
  candidates: DeletionCandidate[];
}

export interface RetentionRunResponse {
  deleted: number;
  schema_version: number;
}

// --- Recording (SPEC §6.4) ------------------------------------------------

export type RecordingState = "idle" | "recording";

export interface RecordingConfig {
  enabled: boolean;
  capture_fps: number;
  frame_size: number;
  width: number;
  height: number;
  segment_duration_s: number;
  min_duration_s: number;
  max_storage_percent: number;
  post_event_s: number;
  pre_event_s: number;
}

export interface RecordingInfo {
  recording_id: string;
  category: string;
  start_time: number;
  duration_s: number;
  frame_count: number;
  size_bytes: number;
  width: number;
  height: number;
  fps: number;
  status: string;
  recovered: boolean;
}

export interface RecordingListResponse {
  has_more: boolean;
  count: number;
  offset: number;
  limit: number;
  items: RecordingInfo[];
}

export interface RecordingStatus {
  state: RecordingState;
  schema_version: number;
}

// --- Motion (SPEC §4.3) -----------------------------------------------------

export interface MotionConfig {
  enabled: boolean;
  sample_rate_idle_hz: number;
  sample_rate_rec_hz: number;
  threshold: number;
  min_changed_ratio: number;
  cooldown_ms: number;
  min_frames: number;
  settle_frames: number;
  record_on_motion: boolean;
  snapshot_on_motion: boolean;
  mqtt_notify: boolean;
}

export interface MotionStatus {
  enabled: boolean;
  active: boolean;
  events_total: number;
  events_last_ts_ms: number;
  checks_total: number;
  checks_skipped: number;
  changed_ratio_last: number;
  light_level_last: number;
  decode_time_ms_last: number;
  analyze_time_ms_last: number;
  schema_version: number;
}

// --- Error codes (API contract skill) ---------------------------------------
export type ErrCode =
  | "UNAUTHENTICATED"
  | "SESSION_EXPIRED"
  | "CSRF_INVALID"
  | "VALIDATION_FAILED"
  | "NOT_FOUND"
  | "SD_NOT_MOUNTED"
  | "SD_FULL"
  | "SD_WRITE_FAILED"
  | "STREAM_SLOT_LIMIT"
  | "PLAYBACK_BUSY"
  | "TIME_UNSYNCHRONIZED"
  | "RATE_LIMITED"
  | "CONFIRMATION_REQUIRED"
  | "Internal";

export class ApiError extends Error {
  readonly code: ErrCode | string;
  readonly status: number;
  readonly requestId?: string;
  constructor(code: ErrCode | string, message: string, status: number, requestId?: string) {
    super(message);
    this.name = "ApiError";
    this.code = code;
    this.status = status;
    this.requestId = requestId;
  }
}

// --- Seven page states (DESIGN §6) -----------------------------------------
export type PageState = "loading" | "empty" | "error" | "degraded" | "success" | "auth" | "confirm";
