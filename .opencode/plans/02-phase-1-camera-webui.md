# Phase 1 — Core camera and web UI

**Goal:** an authenticated device that streams live MJPEG to a browser and
phone, saves a snapshot, and is administered entirely from the embedded
React UI — with no internet access required at runtime.

**Spec:** §4.1, §4.1.1, §4.2, §5, §6 (6.0, 6.1, 6.2, 6.3), §7, §8.2, §10,
§16, §12 Phase 1, §18.
**Skills:** `visioncam-dev`, `visioncam-webui`, `visioncam-api-contract`,
`visioncam-hardware-validation`.

---

## Entry criteria

Phase 0 exit gate green.

## Deliverables

1. Camera manager + frame manager (sole frame owner).
2. Wi-Fi manager (station mode, reconnect with backoff).
3. HTTP server (`esp_http_server`) with the §6.0 limits and the full
   `/api/v1` envelope.
4. Auth: single user, salted password hash, HTTP-only session cookie, CSRF,
   throttling, forced first-login password change.
5. Embedded React + TypeScript + Vite UI with the DESIGN.md shell and tokens.
6. Authenticated MJPEG stream, snapshot capture endpoint, camera settings,
   system status page.
7. The seven UI state classes as reusable components.

## Ordered tasks

### 1.1 Core services

1. `firmware/src/core/`: init order, event bus skeleton (typed, bounded),
   resource monitor (heap, largest block, min heap, PSRAM, uptime, reset
   reason, task stack watermarks), RTC RAM ring log.
2. `firmware/src/network/`: `esp_wifi` + `esp_netif` + `esp_event` station
   mode; NTP via `esp_sntp`; reconnect with exponential backoff + jitter;
   expose RSSI and connection state. Status is `Connected` only after a real
   association + IP (DESIGN principle).
3. `firmware/src/camera/`: implement §4.1.1 exactly — init at max PSRAM frame
   size, PID check, 2 retries with PWDN cycle, ≥3 warm-up frames, `fb_count=2`,
   `CAMERA_GRAB_LATEST`, PSRAM buffers, XCLK 20 MHz, explicit
   `jpeg_buffer_size` for tiny sizes, drop `len==0` / oversized frames and
   count them.
4. NULL-frame recovery per §9.1: ≤1 attempt/2 s, ≤10 attempts, sensor reset
   then PWDN power-cycle + re-init, 10 ms yield, reset counters on success,
   then raise `CameraFailed` and stop consumers cleanly. Never spin.
5. `firmware/src/frame/` frame manager: one capture task, bounded descriptor
   queue, reference-counted leases, per-consumer PSRAM copies for slow
   consumers, drop policy for slow consumers, low-res copy helper for later
   motion/AI use. **No other module may call `esp_camera_fb_get`.**

### 1.2 HTTP server and API

6. `firmware/src/web/`: `esp_http_server` with `lru_purge_enable = true`,
   `max_open_sockets` = API clients + stream slots (document the total;
   start smaller than R1's `HTTP_CLIENTS + 4`), 4 KB stack, LRU purge
   verified. One slot per long-running response; enforce the hard limit.
7. Response envelope helpers for `{ok,data,error,request_id}`; monotonic
   `request_id`; never leak stack traces or secrets.
8. Implement §6.1 auth, §6.2 device/status, §6.3 camera endpoints. Stream
   route is cookie-authenticated — **no credentials in query strings**
   (§6.3). Add `POST /auth/complete-first-login`.
9. `firmware/src/auth/`: salted password KDF (benchmark on ESP32 and document
   parameters), constant-time comparison, session tokens from the platform
   CSPRNG, sessions in RAM only (reboot invalidates), HTTP-only + SameSite
   cookie, login throttling, session revocation on password change, CSRF
   token for cookie-authenticated state changes, Origin/Host validation.
   Never log passwords, hashes or tokens.
10. `firmware/src/system/`: `/metrics`, `/health`, `/status`, restart.

### 1.3 Embedded web UI

11. `webui/`: Vite + React + TypeScript. No runtime CDN, no remote fonts,
    no remote images. System font stack per DESIGN §2.2.
12. Design tokens as CSS variables on `:root` exactly per DESIGN §2.1–2.3
    (danger text `#F26B6B`, danger fills `#E05252`).
13. Shell components: `AppShell`, `Sidebar`, `Header`, `Footer`,
    `StatusPill`, `Card`, `StatCard`, `Row`, `Button`, `Field`, `Slider`,
    `ProgressBar`, `VideoPlayer`, `Thumbnail`, `ConfirmDialog`, `Toast`,
    `Nav`. One embedded inline-SVG stroke icon set, only the icons used.
14. Pages for this phase: **Login** (incl. forced password change and
    session-expired states), **Overview**, **Live View**, **Settings ›
    General/Camera/Network/Security/System** (the rest can be stubs that
    show "coming in Phase N"), **System status**.
15. Build the seven required states once and reuse: skeleton loading, empty,
    error + retry, degraded banner, success toast, auth failure/expiry,
    destructive confirm (typed confirm for format/factory reset).
16. Responsive: desktop full sidebar → tablet collapsible → mobile drawer,
    one-column cards, 44px targets, no hover-only actions (DESIGN §3).
17. Typed API client; HTTP-only cookie session; **no credentials in
    `localStorage` or URLs**; cancel stale requests on navigation; separate
    the stream connection from API state.
18. Single WebSocket for status push with `max 2` clients and polling
    fallback (DESIGN §7). Verify `httpd_ws` under load before relying on it.

### 1.4 Asset embedding

19. `tools/embed_web_assets.py`: content-hashed, minified, gzip-compressed
    assets embedded as firmware byte arrays + asset manifest.
20. Embedded fallback page shown if asset loading fails.
21. Measure the final bundle against the flash partition headroom and record
    it. Lazy-load large pages.

## Memory / timing / hardware notes

- Camera task stays off the application hot path; R2's camera task is core 0 —
  pin deliberately and measure.
- Stream copies live in PSRAM; never hold a camera buffer across network I/O.
- Keep free-heap warning thresholds (≈32 KB free, ≈16 KB largest block) wired
  into the resource monitor now — later phases depend on them.

## Tests and acceptance

- [ ] Authenticated browser access works; unauthenticated `/api/v1/*` and
      `/camera/stream` are rejected (401).
- [ ] First login forces password change; default credentials stop working.
- [ ] Password survives reboot; logout invalidates; session expires.
- [ ] Login throttling works; no secret in logs or API responses.
- [ ] Live view loads on desktop **and** on a phone.
- [ ] Stream disconnect releases the slot and frees buffers.
- [ ] Snapshot endpoint returns a valid JPEG; camera settings persist and
      apply live (no reboot for camera/motion settings).
- [ ] Maximum stream client limit enforced; streaming does not starve
      resource monitoring.
- [ ] UI assets load with the device having **no** internet access.
- [ ] Loading / empty / error / degraded / toast / auth / confirm states all
      demonstrable.
- [ ] `pio run` size + web asset size recorded against partition headroom.

## Exit gate

Authenticated live MJPEG on phone and laptop, snapshot saved, no
unauthenticated stream access, UI fully offline.

## Docs updated

`docs/ARCHITECTURE.md` (task table, measured priorities/stacks),
`docs/API.md` (endpoints actually implemented), `docs/SECURITY.md` (threat
model + limitations), `docs/UI_UX_SPEC.md`, `CHANGELOG.md`.

## Risks / known gaps

- Flash budget for the React bundle is the #1 risk in this phase — measure
  before adding icons/fonts.
- Session + CSRF design must be settled here; retrofitting is expensive.
- Mobile/tablet layouts are not in the mockups and must be designed from
  DESIGN §3.
