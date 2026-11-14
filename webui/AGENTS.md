# AGENTS.md — `webui/`

Loaded when working under `webui/`. Read the repository-root `AGENTS.md`
first; this file only adds frontend-specific guidance.

## Stack

React + TypeScript + Vite, CSS variables, no runtime CDN, no remote fonts,
no remote images. Static assets are content-hashed, minified, gzip-compressed
and embedded in firmware by `tools/embed_web_assets.py`.

## Layout

```
webui/
├── src/
│   ├── app/          # shell, routing, providers
│   ├── components/   # design-system primitives (Card, Button, Field, …)
│   ├── features/     # feature-specific composites
│   ├── hooks/        # data hooks (session, status, media queries)
│   ├── lib/          # typed API client, ws client, formatters, errors
│   ├── pages/        # one file per route
│   ├── styles/       # tokens.css, base.css, utilities
│   └── types/        # generated/shared API types
└── public/           # non-embedded static files only
```

## Design authority

1. `DESIGN.md` — tokens, typography, spacing, components, states, responsive
   rules. It wins over the mockup where they differ.
2. `mockup-ui/*.png` — layout and content reference. `[bracketed]` values are
   placeholders, never real data.
3. `SPEC.md` §5 and §16 — page requirements and required states.

## Non-negotiables

- **Every page implements all seven states:** loading (skeleton), empty,
  error + retry, degraded/offline banner, success toast, auth failure /
  session expiry, destructive confirmation (typed confirm for format card,
  factory reset, model rollback).
- Status colour is never the only signal — pair it with a word
  ("Connected", "Not tested", "Incompatible operator").
- Never show a green "Connected" from a saved setting; it must reflect a
  real connection result.
- Real `<button>`, `<a>`, `<input>` elements only; `aria-label` on icon-only
  buttons; visible `--accent` focus ring; text contrast ≥ 4.5:1.
- No credentials in `localStorage` or URLs. Session is an HTTP-only cookie.
- Separate the video stream connection from API state; cancel stale requests
  on navigation; paginate media and events, never preload an index.
- Responsive: desktop full sidebar → tablet collapsible 2-column → mobile
  drawer, 1-column, 44px touch targets, no hover-only actions, no wide
  tables. The mockups are desktop-only, so mobile/tablet must be designed
  here.
- Bundle budget: measure `dist/` size against the partition headroom before
  adding icons or fonts. Lazy-load large pages.

## Checklist for a new page

1. Route + nav entry + active state.
2. Loading / empty / error / degraded / toast / confirm states.
3. Typed API client method; no `any`; no `fetch` outside `lib/`.
4. Destructive actions behind `ConfirmDialog`.
5. Responsive at 1280 / 768 / 375.
6. Keyboard reachable and screen-reader labelled.
7. Verify against the matching `mockup-ui/*.png`.
