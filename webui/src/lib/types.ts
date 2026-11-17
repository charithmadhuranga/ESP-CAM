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
