# VisionCam Pro: Web UI Design

Design reference for the embedded React + TypeScript web application. It is derived from the UI mockup canvas (17 desktop screens) and the product specification (sections 5 and 16). Where the mockup and the spec differ, this file says so.

Mockup: https://claude.ai/artifact/LP5YCXawtpeh76MddDJipe

---

## 1. Design principles

- Professional NVR / security-operations dashboard, not a consumer app.
- Information hierarchy before decoration. No decorative gradients.
- Dark theme is the default. Light theme is optional (tokens must be swappable).
- Strong status visibility: camera, recording, SD, Wi-Fi, MQTT and AI state are always findable.
- Large live video, clear recording state.
- Minimal navigation depth: every page is one click from the sidebar.
- Every destructive action requires confirmation.
- A status is "Connected" only when a real connection succeeded, never just because a setting was saved.
- Fully offline: no CDN, no remote fonts, no remote images at runtime.

---

## 2. Design tokens

Define all colours as CSS variables on `:root`; a light theme overrides the same names.

### 2.1 Colour (dark theme)

| Token | Value | Use |
|---|---|---|
| `--bg` | `#101318` | App background, input fill |
| `--sidebar` | `#171C23` | Sidebar |
| `--surface` | `#1D242D` | Cards |
| `--elevated` | `#252E39` | Active nav item, secondary buttons, pills, progress track |
| `--border` | `#303A46` | All dividers and card/button borders |
| `--text` | `#F1F5F9` | Primary text |
| `--text-2` | `#A3AFBD` | Labels, secondary text, inactive nav |
| `--accent` | `#4F9CF9` | Primary action, progress fill, links, "AI" tags |
| `--success` | `#2FBF71` | Online, connected, on, validated, OK |
| `--warning` | `#E6A23C` | Motion, capturing, not tested, caution |
| `--danger` | `#F26B6B` | Errors, delete, REC, factory reset |
| `--video` | `#0A0D11` | Video and thumbnail placeholder |

Deviation from the spec palette: the spec lists danger as `#E05252`. The mockup uses `#F26B6B` for text so it reaches 4.5:1 contrast on `--surface`. Keep `#E05252` for fills and borders if wanted, `#F26B6B` for danger text.

Status colour must never be the only signal. Pair it with a word ("Connected", "Not tested", "Incompatible operator").

### 2.2 Typography

- Font stack: `system-ui, -apple-system, "Segoe UI", Roboto, sans-serif`.
- The mockup lists Inter first, but Inter is not embedded in firmware. Do not load it from a CDN. Either use the system stack or embed one subsetted woff2 and measure the flash cost.
- Base size 13px / line-height 1.4.

| Role | Size | Weight |
|---|---|---|
| Page title (h2) | 22px | 600 |
| Card title (h3) | 14px | 600 |
| Body, inputs, buttons | 13px | 400 / 500 |
| Labels, footer, pills | 12px | 400 |
| Product wordmark | 13px, letter-spacing 1px | 700, uppercase |

### 2.3 Spacing, radius, size

- Spacing scale: 4, 8, 12, 16, 24 (8px base).
- Card padding 16px; page content padding 24px; grid gap 16px.
- Radius: cards 14px, video areas 10px, buttons/inputs/nav items 8px, pills 99px, progress bars 5px.
- Sidebar 230px (spec range 220-250). Header 60px (spec range 60-68). Footer bar about 36px.
- Minimum touch target 44px on mobile.

---

## 3. Layout

```
+--------------------------------------------------------------+
| Sidebar 230  | Header 60:  [Device] [MQTT] [Admin]           |
|              +-----------------------------------------------+
| VISIONCAM PRO| Main (padding 24, gap 16)                      |
| Overview     |   Page title row           [page actions]      |
| Live View    |   Cards / grids                                |
| Recordings   |                                                |
| ...          +-----------------------------------------------+
|              | Footer: Firmware | SD: Mounted | Wi-Fi         |
+--------------+-----------------------------------------------+
```

- Header status pills: `Device: Online`, `MQTT: Connected`, `Admin`. Spec also lists time, notifications and a user menu; add them in implementation.
- Footer bar always shows firmware version, SD state and Wi-Fi state.
- Optional right-hand detail panel for a selected media item or event (spec 5.3). Not drawn in the mockup.
- Toast layer and modal layer sit above everything.

### Navigation

```
Overview
Live View
Recordings
Snapshots
Timelapse
AI Detection
Storage
Integrations   (MQTT, Home Assistant, Network Storage)
Settings       (General, Camera, Recording, Motion, AI Models, Network, Security, System)
```

Active item: `--elevated` fill with `--text`. Inactive: `--text-2`, no fill.

### Responsive behaviour

| Breakpoint | Behaviour |
|---|---|
| Desktop | Full sidebar, multi-column cards, side panels, full timeline |
| Tablet | Collapsible sidebar, two-column cards, player controls below video |
| Mobile | Drawer navigation, one-column cards, video 100% width, 2-column media grid where space allows, timeline scrolls horizontally, no hover-only actions, no wide tables |

The mockup draws desktop only (1280x760 artboards). Mobile and tablet layouts still need designing.

---

## 4. Components

| Component | Spec |
|---|---|
| **Card** | `--surface`, 1px `--border`, radius 14, padding 16. Optional h3 title. |
| **Stat card** | Card with a 12px `--text-2` label and a 22px coloured value (e.g. "Online" in success). |
| **Row** | Flex, space-between, 8px vertical padding, 1px bottom `--border`. Used for key/value lists, file lists, event lists. |
| **Primary button** | `--accent` fill, `--bg` text, weight 600, radius 8, padding 9x14. One per view or form. |
| **Secondary button** | `--elevated` fill, 1px `--border`, `--text`. |
| **Danger button** | Secondary style with `--danger` text. Always opens a confirmation. Factory reset and format card use typed confirmation. |
| **Pill** | `--elevated` fill, 12px, fully rounded. Status pills colour the value text; filter pills use accent for the selected one. |
| **Input + label** | 12px `--text-2` label above; input `--bg` fill, 1px `--border`, radius 8, padding 9, full width. Every input has a real `<label>`. |
| **Progress / slider bar** | 10px high, `--elevated` track, `--accent` fill. SD usage, quality, brightness, threshold, timelapse progress. |
| **Video placeholder** | `--video` fill, radius 10, centred `--text-2` caption. Replaced by the MJPEG `<img>` / player. REC pill sits top-left. |
| **Thumbnail** | `--video` box with timestamp caption; hover-free (actions are visible buttons). |
| **Nav item** | 10x12 padding, radius 8, weight 500. |
| **Key/value list** | Rows of label (`--text-2`) and value. Used for device health and system info. |
| **Warning callout** | Card with `--warning` border, bold "Warning" lead. Used on Security. |

Accessibility rules: real `<button>`, `<a>`, `<input>` elements only; `aria-label` on icon-only buttons; visible keyboard focus ring in `--accent`; text contrast 4.5:1 or better.

The mockup uses no icons. Pick one small inline-SVG stroke icon set and embed only the icons used.

---

## 5. Screen inventory

All screens share the shell in section 3.

| Screen | Layout | Key content |
|---|---|---|
| **Login** | Centred 360px card, no sidebar | Wordmark, username, password, show-password, Sign in, generic error "Invalid username or password" (never reveals which field). Also needed: forced password change on first login, session-expired state. |
| **Overview** | 4 stat cards; 2fr/1fr row; 2-column row | Camera online, recording mode, motion today, AI events today; live preview + device health (Wi-Fi RSSI, SD usage, MQTT, AI provider); recent events and recent snapshots. Actions: Capture, Record. |
| **Live View** | Video + 260px controls panel; toggle strip below | Video with REC pill; resolution, quality, brightness, contrast; Snapshot, Record, Flash; pills for Motion, AI, FPS, stream state; Fullscreen. |
| **Recordings** | 240px sidebar + player column | Calendar, filters (continuous, motion, AI, manual), player, event timeline, Play/Pause/Speed/Download/Delete, integrity badge. |
| **Snapshots** | Filter pills, 5-column grid, pagination | Filters: All, Manual, Motion, Detection, Timelapse; date range. Full-screen viewer, metadata panel and event link are still to design. |
| **Timelapse** | Project list + 340px new-project form | Capturing/Ready status, progress bar, preview, download, manage source frames; interval, schedule, resolution/quality, output FPS. |
| **AI Detection** | 3 cards over a 2fr/1fr row | Provider selector (TFLite Micro / External / Disabled), active model, compatibility result, upload, rollback; threshold, interval, classes; external endpoint with test; recent detections; inference time and tensor arena. |
| **Storage Manager** | Capacity card; file browser + retention | Capacity bar, health, category breakdown, breadcrumb file list with download/delete, retention policy, Rebuild index, Format card (typed confirmation). |
| **Integrations** | 3 equal cards | MQTT (host/port, base topic, client ID, test, save and reconnect), Home Assistant (discovery prefix, birth message, entities, republish), Network Storage (backend, auth, queue, test). |
| **Settings** | 200px section list + form card | General, Camera, Recording, Motion, AI Models, Network, Security, System (below). The active section is highlighted. |

### Settings sections

| Section | Content |
|---|---|
| General | Device name, timezone, NTP server, Wi-Fi network, Save, Restart |
| Camera | Resolution, JPEG quality, frame-rate limit, flash LED mode; brightness, contrast, saturation, sharpness; auto exposure, auto white balance, flip/mirror |
| Recording | Mode, format (MJPEG/AVI), segment length, max FPS, pre- and post-event time; continuous / on-motion / on-AI toggles |
| Motion | Sensitivity, change threshold, detection-zone editor, cooldown, minimum frames; start-recording and MQTT-notify toggles |
| AI Models | Installed models with status and activate/rollback; model + manifest upload and validation; tensor arena; inference timeout |
| Network | SSID, password, IP mode, hostname, nearby networks, Scan, Save and reconnect |
| Security | Change password, session timeout, "do not expose to the internet" warning |
| System | Firmware, web UI, min free heap, PSRAM, uptime; back up / restore config, Restart, Factory reset with typed confirmation |

---

## 6. States every page must have

Required by spec 16.7. Only the happy path is drawn in the mockup, so these still need design:

1. Initial loading (skeleton rows or card placeholders).
2. Empty state (no recordings, no snapshots, no models).
3. Error state with retry.
4. Offline / degraded state (SD missing, MQTT down, external AI unreachable; the core camera keeps working).
5. Success feedback (toast).
6. Authentication failure and session expiry.
7. Confirmation dialog for destructive actions: delete media, delete recording, format card, factory reset, rollback model.

Degraded-mode banner: a `--warning` callout at the top of the main area naming the failed subsystem. It must not block camera, recording or local playback.

---

## 7. Implementation notes

- Stack: React, TypeScript, Vite, CSS variables. No runtime CDN dependencies. Content-hashed, minified, gzip-compressed assets embedded in firmware, plus a fallback page if asset loading fails.
- State: React context or Zustand for session and device state; a lightweight API cache for REST. Keep the video stream connection separate from API state. Cancel stale requests on navigation. Paginate media and events; never preload the whole index.
- Session auth uses HTTP-only cookies. Never store credentials in `localStorage`.
- Live status uses one WebSocket (max 2 clients) with polling fallback.
- Suggested component names: `AppShell`, `Sidebar`, `Header`, `StatusPill`, `Card`, `StatCard`, `Row`, `Button`, `Field`, `Slider`, `ProgressBar`, `VideoPlayer`, `Thumbnail`, `ConfirmDialog`, `Toast`.
- Test on current Chrome, Firefox, Safari (iOS) and mobile Chrome.
- Measure the final bundle size against the flash partition budget before adding fonts or icon sets.

---

## 8. Mockup status and open items

- The mockup values in `[brackets]` are placeholders, not real data.
- The mockup artboards are fixed at 1280x760; the implementation must be fluid.
- Still to design: mobile and tablet layouts, light theme, the states in section 6, snapshot full-screen viewer and metadata panel, recording download and integrity detail, header notifications and user menu, first-login password-change screen, and the sub-pages under Integrations as separate views.
