---
name: VisionCam API contract and schemas
description: Conventions for every VisionCam Pro HTTP endpoint and persisted schema — the /api/v1 response envelope and error codes, pagination and filtering parameters, auth/CSRF rules, endpoint inventory by phase, and the versioned configuration, media-index, event-index, model-manifest and MQTT payload schemas. Load when adding or changing an endpoint, a config key, an index record, or when writing the typed frontend client.
license: MIT
metadata:
  version: "1.0"
  author: visioncam-pro
---

# API contract skill

Read `AGENTS.md`, SPEC §6 and §7, and the active phase file first.

## Envelope (every JSON response)

```json
{ "ok": true,  "data": {},    "error": null, "request_id": "req-000001" }
{ "ok": false, "data": null,
  "error": { "code": "SD_NOT_MOUNTED", "message": "Storage is unavailable" },
  "request_id": "req-000002" }
```

- Base prefix `/api/v1`.
- `request_id` monotonic per boot; useful in logs, safe to show a user.
- Appropriate HTTP status codes. **Never** a stack trace or a secret in a
  message.
- Every payload carries `schema_version` where it is persisted or evolves.

## Server limits (SPEC §6.0)

- `esp_http_server`, `lru_purge_enable = true`.
- `max_open_sockets` = allowed API clients + stream slots; document the
  total; start smaller than R1's `HTTP_CLIENTS + 4`.
- 4 KB server stack; measure the high-water mark with the largest handler.
- Each long-running response (live stream, playback, large download) holds
  one stream slot — a hard limit.

## Auth rules on every endpoint

- Auth required for management, media, snapshot, stream and download routes.
- Session = HTTP-only cookie; CSRF token on cookie-authenticated
  state-changing requests; validate Origin/Host.
- No credentials or tokens in query parameters — the MJPEG stream is
  cookie-authenticated, never a long-lived token in the URL.
- Secrets are never returned by GET: return `password_configured: true`
  style booleans; mask integration secrets in the UI.
- Media routes take **opaque IDs** (`med-…`, `rec-…`, `evt-…`), never file
  paths; reject traversal even though FAT has no symlinks.

## Pagination and filtering (list endpoints)

Request: `limit` (strict maximum), `cursor` or `offset`, `from`, `to`,
`type`, `sort`, `order`.
Response: `items`, `next_cursor`, `has_more`, `total_estimate` where
feasible. Do not compute expensive totals on every request.

## Endpoint inventory (by phase)

| Phase | Group | Endpoints |
|---|---|---|
| 1 | Auth | `POST /auth/login`, `/auth/logout`, `GET /auth/session`, `POST /auth/change-password`, `POST /auth/complete-first-login` |
| 1 | Device | `GET /device`, `/status`, `/health`, `/metrics`, `POST /device/restart` |
| 1 | Camera | `GET /camera/status`, `/camera/settings`, `PUT /camera/settings`, `POST /camera/capture`, `/camera/flash`, `GET /camera/stream` |
| 2 | Snapshots | `GET /snapshots`, `/snapshots/{id}`, `/snapshots/{id}/image`, `/snapshots/{id}/download`, `DELETE /snapshots/{id}` |
| 2 | Storage | `GET /storage/status`, `/storage/files`, `/storage/files/{id}`, `/storage/files/{id}/download`, `DELETE /storage/files/{id}`, `POST /storage/rebuild-index`, `GET|PUT /storage/retention`, `POST /storage/format` |
| 3 | Recording | `GET /recording/status`, `/recording/config`, `PUT /recording/config`, `POST /recording/start`, `/recording/stop`, `GET /recordings`, `/recordings/{id}`, `/recordings/{id}/download`, `/recordings/{id}/play`, `DELETE /recordings/{id}` |
| 4 | Motion | `GET|PUT /motion/config`, `GET /motion/status` |
| 5 | Timelapse | `GET|POST /timelapse/projects`, `GET|PUT|DELETE /timelapse/projects/{id}`, `POST …/start`, `…/stop`, `…/generate`, `GET /timelapse/jobs/{id}`, `GET …/preview`, `GET …/download` |
| 6 | AI | `GET /ai/status`, `/ai/providers`, `/ai/models`, `POST /ai/models/upload`, `/ai/models/{id}/validate`, `/ai/models/{id}/activate`, `/ai/models/deactivate`, `/ai/models/rollback`, `GET /ai/events`, `GET|PUT /ai/config` |
| 7–8 | Integrations | `GET|PUT /integrations/mqtt`, `POST /integrations/mqtt/test`, `/reconnect`, `/test-publish`, `POST /integrations/home-assistant/discovery`, `GET /integrations/home-assistant/status`, `GET|PUT /integrations/network-storage`, `POST …/test`, `GET …/queue` |

Uploads are **size-limited and streamed to a staging file** — never buffer a
whole model (or any user-supplied blob) in RAM.

## Error codes worth standardising

`UNAUTHENTICATED`, `SESSION_EXPIRED`, `CSRF_INVALID`, `VALIDATION_FAILED`,
`NOT_FOUND`, `SD_NOT_MOUNTED`, `SD_FULL`, `SD_WRITE_FAILED`,
`PLAYBACK_BUSY`, `STREAM_SLOT_LIMIT`, `MODEL_INVALID`,
`MODEL_ARENA_TOO_LARGE`, `AI_PROVIDER_UNAVAILABLE`, `MQTT_UNAVAILABLE`,
`TIME_UNSYNCHRONIZED`, `RATE_LIMITED`, `CONFIRMATION_REQUIRED`.

## Persisted schemas (all versioned)

**Configuration (NVS authoritative, namespaced keys, size-checked):**
`schema_version`, `device{device_id,device_name,timezone,ntp_enabled}`,
`camera{frame_size,jpeg_quality,brightness,contrast}`,
`recording{continuous_enabled,event_enabled,segment_seconds,capture_fps,
post_event_seconds}`, `motion{enabled,sample_interval_ms,threshold,
minimum_changed_ratio,cooldown_seconds}`, `ai{enabled,provider,
inference_interval_ms,confidence_threshold}`, `mqtt{enabled,host,port,
base_topic,ha_discovery_enabled,discovery_prefix}`,
`storage{retention_days,max_used_percent,network_storage_enabled}`.

Update process: parse → validate all fields/ranges → build candidate →
persist safely → apply runtime changes → roll back runtime state if apply
fails → return sanitised effective configuration. Secrets live in a separate
namespace and are never echoed.

**Media index record (JSONL, append-only):**
```json
{"schema_version":1,"media_id":"med-000001","type":"snapshot",
 "category":"motion","path":"/snapshots/2026/11/15/motion/med-000001.jpg",
 "mime":"image/jpeg","size":28431,"created_at":"2026-11-15T10:30:00Z",
 "event_id":"evt-000123","duration_ms":0,"status":"ready"}
```

**Event index record:**
```json
{"schema_version":1,"event_id":"evt-000123","type":"motion",
 "created_at":"2026-11-15T10:30:00Z","ended_at":"2026-11-15T10:30:08Z",
 "snapshot_id":"med-000001","recording_id":"rec-000009","detections":[],
 "notification_status":"queued"}
```

Rules: validate each line independently; ignore a malformed trailing record
during recovery; compact via atomic temp-file replacement; rebuild by
directory scan; never load the whole library into RAM.

**Model manifest (SPEC §4.4.1):** `schema_version`, `model_id`,
`display_name`, `format`, `file`, `sha256`, `input{width,height,channels,
dtype,layout}`, `output{dtype,description}`, `required_tensor_arena_bytes`,
`operators[]`, `classes[]`, `preprocessing`.

**Normalised detection:** `event_id`, `source`, `class_id`, `label`,
`confidence`, `bbox` as **x, y, width, height normalised 0.0–1.0**
(consistent everywhere), `timestamp`, `snapshot_id`.

**External AI:** `POST /api/v1/infer`, multipart JPEG + metadata →
`{request_id, model, inference_ms, detections[{class_id,label,confidence,
bbox{}}]}`. Policies: `disabled | motion_only | periodic | on_device_event |
manual_snapshot`.

**MQTT:** base `visioncam/{device_id}`; availability, state, status,
motion/detection/recording/snapshot/storage/wifi/ai topics; payload
`schema_version`, `device_id`, `event_id`, `event_type`, `source`, `label`,
`confidence`, `timestamp`, `snapshot_path`. **No credentials, no binary
images.**

## Adding an endpoint — the checklist

1. Add it to the inventory table above and to `docs/API.md`.
2. Envelope + `request_id` + correct status code.
3. Auth + CSRF check; opaque IDs only; input validation with ranges.
4. Bounded pagination; no whole-index reads.
5. Error code from the standard set; no secrets in messages.
6. Typed method in `webui/src/lib` + types in `webui/src/types`.
7. Test: authenticated 200, unauthenticated 401, invalid input 400/422.
8. Update `CHANGELOG.md` (rule 28) — never silently change a contract
   (rule 27).
