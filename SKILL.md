# SKILL.md

The VisionCam Pro development skill lives at
**`.opencode/skills/visioncam-dev/SKILL.md`**.

It is stored there rather than at the repository root because OpenCode only
discovers skills under `.opencode/skills/` (see
<https://opencode.ai/docs/skills/>). This file exists so the repository layout
required by `SPEC.md` §13 still has a `SKILL.md` entry point.

Load it with the `skill` tool using ID `visioncam-dev`, or write
`@visioncam-dev` in a prompt.

Companion skills in `.opencode/skills/`:

| ID | Use it for |
|---|---|
| `visioncam-dev` | The main development workflow and all subsystem rules |
| `visioncam-build-flash` | PlatformIO + ESP-IDF build, flash, monitor, partitions, `sdkconfig` |
| `visioncam-reference-first` | Clean-room study of R1/R2 with the adopt/adapt/defer/reject log |
| `visioncam-webui` | React/Vite embedded UI, design tokens, mockup conformance, UI states |
| `visioncam-api-contract` | `/api/v1` envelope, endpoints, config and index schemas |
| `visioncam-hardware-validation` | Bench tests, baseline measurement, resource metrics, acceptance runs |

`AGENTS.md` at the repository root is always loaded and must be read first.
