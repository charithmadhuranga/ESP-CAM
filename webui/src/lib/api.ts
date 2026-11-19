// VisionCam Pro — typed API client (SPEC §6, API contract skill).
//
// - Every request sends credentials: "include" so the HTTP-only session
//   cookie travels with it. No credentials in query strings or URLs.
// - State-changing requests (POST/PUT/DELETE) add X-CSRF-Token read from
//   the csrf cookie (double-submit pattern; the session cookie itself is
//   HttpOnly, the csrf cookie is readable by JS).
// - Every response is parsed as the {ok,data,error,request_id} envelope.
//   Non-ok envelopes and network failures throw ApiError.
// - A 401 (unauthenticated / session-expired) is surfaced so the shell can
//   redirect to login.
// - AbortController support lets callers cancel stale requests on navigation.
// - No `fetch` outside this module; all page code calls the typed methods.

import {
  ApiError,
} from "./types";
import type {
  CameraSettings,
  ChangePasswordRequest,
  CompleteFirstLoginRequest,
  DeviceInfo,
  Envelope,
  HealthPayload,
  LoginRequest,
  MediaListResponse,
  MediaMetadata,
  MediaCategory,
  MetricsPayload,
  RecordingConfig,
  RecordingInfo,
  RecordingListResponse,
  RecordingStatus,
  RestartResponse,
  RetentionPreviewResponse,
  RetentionPolicy,
  RetentionRunResponse,
  SessionView,
  StatusPayload,
  CameraStatus,
} from "./types";

const BASE = ""; // same-origin; the embedded server is on port 80

let onUnauthenticated: (() => void) | null = null;
let abortControllers = new Set<AbortController>();

export function setOnUnauthenticated(cb: () => void) {
  onUnauthenticated = cb;
}

export function cancelAllRequests() {
  abortControllers.forEach((c) => c.abort());
  abortControllers = new Set();
}

function readCsrfToken(): string | null {
  const match = document.cookie.match(
    /(?:^|; )\s*visioncam_csrf=([^;]+)/
  );
  return match ? decodeURIComponent(match[1]) : null;
}

async function parseEnvelope<T>(
  res: Response
): Promise<Envelope<T>> {
  const requestId = res.headers.get("x-request-id") ?? undefined;
  let body: Envelope<T>;
  const contentType = res.headers.get("content-type") ?? "";
  if (contentType.includes("application/json")) {
    body = (await res.json()) as Envelope<T>;
  } else {
    const text = await res.text();
    body = {
      ok: res.ok,
      data: null,
      error: res.ok
        ? null
        : { code: "Internal", message: text || res.statusText },
      request_id: requestId ?? "",
    };
  }
  return { ...body, request_id: body.request_id ?? requestId ?? "" };
}

function throwIfNotOk<T>(env: Envelope<T>, status: number): T {
  if (!env.ok) {
    throw new ApiError(
      env.error?.code ?? "Internal",
      env.error?.message ?? "Request failed",
      status,
      env.request_id
    );
  }
  if (env.data === null && env.data !== undefined) {
    // data is explicitly null — nothing to return
    return null as unknown as T;
  }
  return env.data;
}

function headersFor(method: string): HeadersInit {
  const h: HeadersInit = { "Content-Type": "application/json" };
  const isStateChange = method === "POST" || method === "PUT" || method === "DELETE";
  if (isStateChange) {
    const token = readCsrfToken();
    if (token) {
      const headers = h as Record<string, string>;
      headers["X-CSRF-Token"] = token;
    }
  }
  return h;
}

export async function api<T>(
  path: string,
  { method = "GET", body, signal }: { method?: string; body?: unknown; signal?: AbortSignal } = {}
): Promise<T> {
  const opts: RequestInit = {
    method,
    credentials: "include",
    headers: headersFor(method),
    signal,
  };
  if (body !== undefined) {
    opts.body = JSON.stringify(body);
  }
  const res = await fetch(`${BASE}${path}`, opts);
  if (res.status === 401) {
    if (onUnauthenticated) onUnauthenticated();
  }
  const env = await parseEnvelope<T>(res);
  return throwIfNotOk(env, res.status);
}

// Convenience wrappers -------------------------------------------------------
export const get = <T>(path: string, signal?: AbortSignal) =>
  api<T>(path, { method: "GET", signal });

export const post = <T>(path: string, body?: unknown, signal?: AbortSignal) =>
  api<T>(path, { method: "POST", body, signal });

export const put = <T>(path: string, body?: unknown, signal?: AbortSignal) =>
  api<T>(path, { method: "PUT", body, signal });

export const del = <T>(path: string, signal?: AbortSignal) =>
  api<T>(path, { method: "DELETE", signal });

// --- Auth -------------------------------------------------------------------
export async function login(body: LoginRequest): Promise<SessionView> {
  return post<SessionView>("/auth/login", body);
}

export async function logout(): Promise<{ ok: boolean }> {
  return post<{ ok: boolean }>("/auth/logout");
}

export async function getSession(): Promise<SessionView> {
  return get<SessionView>("/auth/session");
}

export async function completeFirstLogin(
  body: CompleteFirstLoginRequest
): Promise<{ password_change_required: boolean }> {
  return post("/auth/complete-first-login", body);
}

export async function changePassword(
  body: ChangePasswordRequest
): Promise<Record<string, never>> {
  return post("/auth/change-password", body);
}

// --- Device -----------------------------------------------------------------
export async function getDevice(): Promise<DeviceInfo> {
  return get<DeviceInfo>("/device");
}

export async function getStatus(): Promise<StatusPayload> {
  return get<StatusPayload>("/status");
}

export async function getHealth(): Promise<HealthPayload> {
  return get<HealthPayload>("/health");
}

export async function getMetrics(): Promise<MetricsPayload> {
  return get<MetricsPayload>("/metrics");
}

export async function restartDevice(): Promise<RestartResponse> {
  return post<RestartResponse>("/device/restart");
}

// --- Camera -----------------------------------------------------------------
export async function getCameraStatus(): Promise<CameraStatus> {
  return get<CameraStatus>("/camera/status");
}

export async function getCameraSettings(): Promise<CameraSettings> {
  return get<CameraSettings>("/camera/settings");
}

export async function putCameraSettings(
  body: CameraSettings
): Promise<CameraSettings> {
  return put<CameraSettings>("/camera/settings", body);
}

// --- Snapshots / Storage (SPEC §6.5, §6.7) -----------------------------------

export async function getMediaList({
  offset = 0,
  limit = 12,
  category,
  signal,
}: {
  offset?: number;
  limit?: number;
  category?: MediaCategory;
  signal?: AbortSignal;
}): Promise<MediaListResponse> {
  const qs = new URLSearchParams({ offset: String(offset), limit: String(limit) });
  if (category) qs.set("category", category);
  return get<MediaListResponse>(
    `/storage?action=files&${qs.toString()}`,
    signal
  );
}

export async function getMediaMetadata(
  mediaId: string,
  signal?: AbortSignal
): Promise<MediaMetadata> {
  return get<MediaMetadata>(`/storage?action=file&id=${mediaId}`, signal);
}

export async function downloadMedia(mediaId: string, signal?: AbortSignal): Promise<Response> {
  const res = await fetch(`${BASE}/snapshot?id=${mediaId}`, {
    method: "GET",
    credentials: "include",
    signal,
  });
  if (res.status === 401) {
    if (onUnauthenticated) onUnauthenticated();
  }
  if (!res.ok) {
    const text = await res.text().catch(() => "");
    throw new ApiError("Internal", text || res.statusText, res.status);
  }
  return res;
}

export async function deleteMedia(mediaId: string): Promise<{ deleted: boolean }> {
  return del<{ deleted: boolean }>(`/storage?action=file&id=${mediaId}`);
}

export async function getStorageStatus(): Promise<{
  mounted: boolean;
  index_loaded: boolean;
  total_bytes: number;
  used_bytes: number;
  free_bytes: number;
  media_count: number;
}> {
  return get<{
    mounted: boolean;
    index_loaded: boolean;
    total_bytes: number;
    used_bytes: number;
    free_bytes: number;
    media_count: number;
  }>("/storage?action=status");
}

export async function rebuildIndex(): Promise<{ rebuilt: boolean }> {
  return post<{ rebuilt: boolean }>("/storage?action=rebuild");
}

export async function getRetention(): Promise<RetentionPolicy> {
  return get<RetentionPolicy>("/storage?action=retention");
}

export async function putRetention(
  body: Partial<RetentionPolicy>
): Promise<RetentionPolicy> {
  return put<RetentionPolicy>("/storage?action=retention", body);
}

export async function previewRetention(): Promise<RetentionPreviewResponse> {
  return get<RetentionPreviewResponse>(
    "/storage?action=retention-preview"
  );
}

export async function runRetention(): Promise<RetentionRunResponse> {
  return post<RetentionRunResponse>("/storage?action=retention-run");
}

// --- Snapshot capture --------------------------------------------------------

export async function captureSnapshot(): Promise<{
  media_id: string;
  captured: boolean;
  suppressed?: boolean;
  reason?: string;
}> {
  return post<{ media_id: string; captured: boolean }>("/snapshot");
}

// --- Recording (SPEC §6.4) ------------------------------------------------

export async function getRecordingStatus(): Promise<RecordingStatus> {
  return get<RecordingStatus>("/recording/status");
}

export async function getRecordingConfig(): Promise<RecordingConfig> {
  return get<RecordingConfig>("/recording/config");
}

export async function putRecordingConfig(
  body: Partial<RecordingConfig>
): Promise<RecordingConfig> {
  return put<RecordingConfig>("/recording/config", body);
}

export async function startRecording(): Promise<{
  recording_id: string;
  started: boolean;
}> {
  return post<{ recording_id: string; started: boolean }>("/recording/start");
}

export async function stopRecording(): Promise<{ stopped: boolean }> {
  return post<{ stopped: boolean }>("/recording/stop");
}

export async function getRecordings({
  offset = 0,
  limit = 12,
  signal,
}: {
  offset?: number;
  limit?: number;
  signal?: AbortSignal;
}): Promise<RecordingListResponse> {
  const qs = new URLSearchParams({ offset: String(offset), limit: String(limit) });
  return get<RecordingListResponse>(`/recordings?${qs.toString()}`, signal);
}

export async function getRecordingMetadata(
  recordingId: string,
  signal?: AbortSignal
): Promise<RecordingInfo> {
  return get<RecordingInfo>(
    `/recordings?action=metadata&id=${recordingId}`,
    signal
  );
}

export async function downloadRecording(
  recordingId: string,
  signal?: AbortSignal
): Promise<Response> {
  const res = await fetch(
    `${BASE}/recordings?action=download&id=${recordingId}`,
    {
      method: "GET",
      credentials: "include",
      signal,
    }
  );
  if (res.status === 401) {
    if (onUnauthenticated) onUnauthenticated();
  }
  if (!res.ok) {
    const text = await res.text().catch(() => "");
    throw new Error(text || res.statusText);
  }
  return res;
}

export async function playRecording(
  recordingId: string,
  startFrame?: number,
  startMs?: number,
  signal?: AbortSignal
): Promise<Response> {
  const qs = new URLSearchParams();
  if (startFrame !== undefined) qs.set("start_frame", String(startFrame));
  if (startMs !== undefined) qs.set("start_ms", String(startMs));
  const res = await fetch(
    `${BASE}/recordings?action=play&id=${recordingId}&${qs.toString()}`,
    {
      method: "GET",
      credentials: "include",
      signal,
    }
  );
  if (res.status === 401) {
    if (onUnauthenticated) onUnauthenticated();
  }
  if (!res.ok) {
    const text = await res.text().catch(() => "");
    throw new Error(text || res.statusText);
  }
  return res;
}

export async function deleteRecording(
  recordingId: string
): Promise<{ deleted: boolean }> {
  return del<{ deleted: boolean }>(`/recordings?action=delete&id=${recordingId}`);
}
