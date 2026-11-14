---
name: VisionCam embedded web UI
description: Build the VisionCam Pro React + TypeScript + Vite interface — design tokens from DESIGN.md, shell and component inventory, the seven required page states, responsive rules, typed API client, HTTP-only cookie sessions, asset embedding and flash bundle budget, and conformance checks against the 17 mockup-ui artboards. Load when creating or changing any page, component, style, or when embedding the built UI into firmware.
license: MIT
metadata:
  version: "1.0"
  author: visioncam-pro
---

# VisionCam web UI skill

Read `AGENTS.md`, `webui/AGENTS.md`, `DESIGN.md` and the matching
`mockup-ui/*.png` before touching `webui/`.

## Authority

1. **`DESIGN.md`** — tokens, type, spacing, components, states, responsive
   behaviour. It wins over the mockup where they differ (e.g. danger text
   `#F26B6B` vs fills `#E05252`).
2. **`mockup-ui/*.png`** — layout and content reference. `[bracketed]` values
   are placeholders, never real data. Artboards are fixed 1280x760; the
   implementation must be fluid.
3. **SPEC §5 and §16** — page requirements and required states.

## Stack and constraints

- React + TypeScript + Vite, CSS variables. No server runtime.
- **No CDN, no remote fonts, no remote images at runtime** — fully offline.
- System font stack: `system-ui, -apple-system, "Segoe UI", Roboto,
  sans-serif`. Never load Inter from a CDN; if a subsetted woff2 is embedded,
  measure the flash cost first.
- Static output: minified, gzip-compressed, content-hashed, embedded in
  firmware via `tools/embed_web_assets.py`, plus an asset manifest and an
  embedded fallback page if asset loading fails.
- **Bundle budget:** measure `dist/` against partition headroom before adding
  icons or fonts. Lazy-load large pages.

## Design tokens (dark theme default)

`--bg #101318` · `--sidebar #171C23` · `--surface #1D242D` ·
`--elevated #252E39` · `--border #303A46` · `--text #F1F5F9` ·
`--text-2 #A3AFBD` · `--accent #4F9CF9` · `--success #2FBF71` ·
`--warning #E6A23C` · `--danger #F26B6B` (text) / `#E05252` (fills) ·
`--video #0A0D11`

Type: base 13px/1.4 · h2 22/600 · h3 14/600 · body 13 · labels/pills 12 ·
wordmark 13/700 uppercase, letter-spacing 1px.
Spacing 4/8/12/16/24 · card padding 16 · page padding 24 · grid gap 16 ·
radius: cards 14, video 10, controls 8, pills 99, progress 5 ·
sidebar 230 · header 60 · footer ~36 · touch target ≥44px.

## Shell and components

`AppShell`, `Sidebar`, `Header`, `Footer`, `StatusPill`, `Card`, `StatCard`,
`Row`, `Button` (primary/secondary/danger), `Pill`, `Field` (label + input),
`Slider`, `ProgressBar`, `VideoPlayer`, `Thumbnail`, `NavItem`,
`KeyValueList`, `WarningCallout`, `ConfirmDialog`, `Toast`.

- One small **inline-SVG stroke icon set**, only the icons actually used —
  the mockups contain no icons.
- Status colour is never the only signal: always pair it with a word.
- A11y: real `<button>/<a>/<input>` only, `aria-label` on icon-only buttons,
  visible `--accent` focus ring, contrast ≥ 4.5:1.

## Navigation (one click from the sidebar)

```
Overview · Live View · Recordings · Snapshots · Timelapse · AI Detection
Storage · Integrations (MQTT, Home Assistant, Network Storage)
Settings (General, Camera, Recording, Motion, AI Models, Network,
          Security, System)
```

## The seven required states — build once, use everywhere

Every page must implement all of them (SPEC §16.7, `DESIGN.md` §6):

1. Initial loading — skeleton rows / card placeholders.
2. Empty state — no recordings, no snapshots, no models.
3. Error state with a retry action.
4. Offline/degraded — a `--warning` callout naming the failed subsystem; it
   must **not** block camera, recording or local playback.
5. Success feedback — toast.
6. Authentication failure and session expiry.
7. Confirmation dialog for destructive actions — delete media, delete
   recording, format card, factory reset, rollback model (typed confirmation
   for format/factory reset).

Never render a green "Connected" from a saved setting — only from a real
connection result.

## Responsive (mockups are desktop-only, so design these yourself)

| Breakpoint | Behaviour |
|---|---|
| Desktop | Full sidebar, multi-column cards, side panels, full timeline |
| Tablet | Collapsible sidebar, two-column cards, player controls below video |
| Mobile | Drawer nav, one-column cards, video 100% width, 2-column media grid where space allows, timeline scrolls horizontally, no hover-only actions, no wide tables |

## State and data rules

- React context or Zustand for session/device state; a lightweight REST
  cache. TanStack Query only if the bundle allows.
- **HTTP-only cookie session.** Never store credentials or tokens in
  `localStorage` or URL parameters.
- One WebSocket for status push (max 2 clients) with polling fallback; keep
  the video stream connection **separate** from API state.
- Cancel stale requests on navigation. Paginate media and events — never
  preload an entire index.
- Typed API client only; centralised error handling.

## Page checklist (run for every page)

1. Route + nav entry + active item styling (`--elevated` fill, `--text`).
2. All seven states present and reachable.
3. Typed client method; no `any`; no raw `fetch` outside `lib/`.
4. Destructive actions behind `ConfirmDialog`.
5. Responsive verified at 1280 / 768 / 375.
6. Keyboard reachable, screen-reader labelled, contrast checked.
7. Compared against the matching `mockup-ui/*.png`.
8. Bundle delta measured if new dependencies were added.

## Screen → mockup map

Login · Main (Overview) · Live · Recordings · Snapshots · Timelapse · AI ·
Storage · Integrations · Settings · SettingsCamera · SettingsRecording ·
SettingsMotion · SettingsAIModels · SettingsNetwork · SettingsSecurity ·
SettingsSystem.

Still to design (record your design in `docs/UI_UX_SPEC.md`): mobile/tablet
layouts, light theme, snapshot full-screen viewer + metadata panel, recording
download/integrity detail, header notifications + user menu, first-login
password-change screen, Integrations sub-pages as separate views.
