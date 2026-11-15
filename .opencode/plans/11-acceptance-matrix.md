# Acceptance matrix — SPEC §11 and §11.10

Cross-reference from every acceptance item to the phase that proves it and the
skill that carries the procedure. Status values: `TODO` → `RUN PASS` /
`RUN FAIL` → `UNVERIFIED` (hardware not yet available).

Maintain the live status in `docs/TEST_PLAN.md`; this file is the map.

---

## 11.1 Hardware tests → Phase 0

| Item | Phase | Skill | Status |
|---|---|---|---|
| Board detected and flashes via ESP32-CAM-MB | 0 | `visioncam-hardware-validation` | **RUN PASS** 2026-11-15 (auto-reset, no BOOT press; `docs/evidence/phase0-smoke-boot-11-*.log`) |
| OV2640 initializes | 0 | `visioncam-hardware-validation` | **RUN PASS** 2026-11-15 (PID `0x0026`, 243,471 B JPEG; forced-failure retry `attempts=2`) |
| PSRAM detected and reported correctly | 0 | `visioncam-build-flash` | **RUN PASS** 2026-11-15 (8,388,608 B; 4 MB mapped — `docs/HARDWARE.md` §2) |
| SD card mounts in 1-bit mode | 0 | `visioncam-hardware-validation` | **RUN PASS** 2026-11-15 (20,000 kHz, `SSR: bus_width=1`, self-test; contact quirk §5.1) |
| Camera and SD operate together | 0 | `visioncam-hardware-validation` | **RUN PASS** 2026-11-15 (`step 14 result: OK`, Wi-Fi off and on) |
| Flash LED behaviour documented with SD mounted | 0 | `visioncam-hardware-validation` | **RUN PASS** 2026-11-15 (OFF for the whole boot — IDF driver does not light it) |
| Stable power under Wi-Fi TX and camera load | 0 | `visioncam-hardware-validation` | **RUN PASS** 2026-11-15 (3 s beacon TX + scan + camera/SD cycles, no reset) |
| Brownout behavior recorded | 0 | `visioncam-hardware-validation` | **RUN PASS** 2026-11-15 for the normal supply (1 boot, none observed); weak-supply drill still owed with **D2** |

## 11.2 Authentication → Phase 1

| Item | Phase | Skill | Status |
|---|---|---|---|
| Default login works on first boot | 1 | `visioncam-dev` | TODO |
| First login forces password change | 1 | `visioncam-dev` | TODO |
| Old default credentials no longer work | 1 | `visioncam-dev` | TODO |
| Password survives reboot | 1 | `visioncam-dev` | TODO |
| Logout invalidates session | 1 | `visioncam-dev` | TODO |
| Session expires | 1 | `visioncam-dev` | TODO |
| Protected media URLs reject unauthenticated requests | 1, 2, 3 | `visioncam-api-contract` | TODO |
| Login throttling works | 1 | `visioncam-dev` | TODO |
| No secret appears in logs/API responses | 1, 9 | `visioncam-dev` | TODO |

## 11.3 Camera and streaming → Phase 1

| Item | Phase | Skill | Status |
|---|---|---|---|
| Live stream loads on desktop | 1 | `visioncam-webui` | TODO |
| Live stream loads on mobile | 1 | `visioncam-webui` | TODO |
| Stream disconnect cleans resources | 1 | `visioncam-dev` | TODO |
| Snapshot is valid JPEG | 1, 2 | `visioncam-hardware-validation` | TODO |
| Camera settings apply and persist | 1 | `visioncam-api-contract` | TODO |
| Maximum client limit enforced | 1 | `visioncam-dev` | TODO |
| Streaming does not starve storage writer | 3 | `visioncam-hardware-validation` | TODO |

## 11.4 Storage → Phase 2

| Item | Phase | Skill | Status |
|---|---|---|---|
| SD mount/unmount state correct | 2 | `visioncam-dev` | TODO |
| Snapshot writes and downloads | 2 | `visioncam-api-contract` | TODO |
| Directory pagination works | 2 | `visioncam-api-contract` | TODO |
| Delete works and updates index | 2 | `visioncam-dev` | TODO |
| Index rebuild recovers missing entries | 2 | `visioncam-dev` | TODO |
| Corrupt trailing JSONL entry ignored | 2 | `visioncam-dev` | TODO |
| Full-card condition handled safely | 2, 9 | `visioncam-hardware-validation` | TODO |
| Active recording is not deleted | 3, 9 | `visioncam-dev` | TODO |
| Power interruption recovery works | 3, 9 | `visioncam-hardware-validation` | TODO |

## 11.5 Recording → Phase 3

| Item | Phase | Skill | Status |
|---|---|---|---|
| Continuous recording starts/stops | 3 | `visioncam-api-contract` | TODO |
| Event recording starts/stops | 4 | `visioncam-api-contract` | TODO |
| Segment closes at configured duration | 3 | `visioncam-dev` | TODO |
| AVI validates in desktop tools | 3 | `visioncam-hardware-validation` | TODO |
| Frame timestamps and rate reasonable | 3 | `visioncam-hardware-validation` | TODO |
| Interrupted segment marked/recovered | 3, 9 | `visioncam-hardware-validation` | TODO |
| Playback works through custom player | 3 | `visioncam-webui` | TODO |
| Downloaded segment plays in a compatible player | 3 | `visioncam-hardware-validation` | TODO |

## 11.6 Timelapse → Phase 5

| Item | Phase | Skill | Status |
|---|---|---|---|
| Scheduled capture occurs | 5 | `visioncam-api-contract` | TODO |
| Source frames persist | 5 | `visioncam-dev` | TODO |
| Project metadata survives reboot | 5 | `visioncam-hardware-validation` | TODO |
| Generation progress visible | 5 | `visioncam-webui` | TODO |
| Cancellation works | 5 | `visioncam-webui` | TODO |
| Output frame order correct | 5 | `visioncam-hardware-validation` | TODO |
| Output downloadable | 5 | `visioncam-api-contract` | TODO |
| Generation does not corrupt concurrent recording | 5 | `visioncam-hardware-validation` | TODO |

## 11.7 AI → Phase 6

| Item | Phase | Skill | Status |
|---|---|---|---|
| No-AI mode works | 6 | `visioncam-api-contract` | TODO |
| Invalid model rejected | 6 | `visioncam-dev` | TODO |
| Unsupported operator reported | 6 | `visioncam-dev` | TODO |
| Tensor arena allocation failure handled | 6 | `visioncam-dev` | TODO |
| Last-known-good model recoverable | 6 | `visioncam-dev` | TODO |
| Inference rate configurable | 6 | `visioncam-api-contract` | TODO |
| AI load does not crash streaming | 6 | `visioncam-hardware-validation` | TODO |
| External AI timeout/retry works | 6 | `visioncam-dev` | TODO |
| External AI outage does not stop local recording | 6, 9 | `visioncam-hardware-validation` | TODO |

## 11.8 MQTT / Home Assistant → Phase 7

| Item | Phase | Skill | Status |
|---|---|---|---|
| Broker connection succeeds | 7 | `visioncam-api-contract` | TODO |
| Bad credentials show useful error | 7 | `visioncam-dev` | TODO |
| Reconnect after broker restart | 7 | `visioncam-hardware-validation` | TODO |
| Last Will publishes offline | 7 | `visioncam-hardware-validation` | TODO |
| Online availability after reconnect | 7 | `visioncam-hardware-validation` | TODO |
| Motion event publishes | 7 | `visioncam-api-contract` | TODO |
| Detection event publishes | 7 | `visioncam-api-contract` | TODO |
| Discovery entities appear | 7 | `visioncam-hardware-validation` | TODO |
| Entity IDs stable across reboot | 7 | `visioncam-hardware-validation` | TODO |
| HA birth message republishes discovery | 7 | `visioncam-hardware-validation` | TODO |
| Removed entities cleaned up | 7 | `visioncam-hardware-validation` | TODO |
| Broker outage does not block camera tasks | 7, 9 | `visioncam-hardware-validation` | TODO |

## 11.9 Long-run acceptance → Phase 9

| Item | Phase | Skill | Status |
|---|---|---|---|
| ≥24 h mixed workload | 9 | `visioncam-hardware-validation` | TODO |
| Streaming + event recording + snapshots + motion + MQTT reconnects + storage | 9 | `visioncam-hardware-validation` | TODO |
| Heap, stacks, dropped frames, SD errors, resets tracked | 9 | `visioncam-hardware-validation` | TODO |
| Repeated reboot/power-interruption recovery | 9 | `visioncam-hardware-validation` | TODO |
| Exact deployment SD card models tested | 9 | `visioncam-hardware-validation` | TODO |

## 11.10 Reference parity → Phases 0, 3, 6, 9

| Item | Phase | Skill | Status |
|---|---|---|---|
| R1 baseline recorded on owner's board and card | 0 | `visioncam-reference-first` | **RUN PASS** 2026-11-15 (8 runs: QVGA 30.8, VGA 20.6, SVGA 20.4, XGA 5.3, SXGA 6.2 FPS at q12, VGA at q8/q16; SD 0.48–0.51 MB/s; `docs/HARDWARE.md` §4). Three items still need the owner: printed card label (D5), R1/Arduino `SD_MMC` lamp check, weak-supply drill |
| VisionCam recording FPS at VGA and QVGA within tolerance | 3, 9 | `visioncam-hardware-validation` | TODO |
| GPIO4 lamp stays off with SD mounted | 0, 9 | `visioncam-hardware-validation` | TODO |
| Camera init retry + PWDN power-cycle verified by forced failure | 0, 1 | `visioncam-hardware-validation` | TODO |
| NULL-frame recovery rate-limited, no task-watchdog trigger | 1, 9 | `visioncam-hardware-validation` | TODO |
| Power-loss segment recovered by chunk scanner and plays | 3, 9 | `visioncam-hardware-validation` | TODO |
| Header FPS equals measured FPS within 5 % | 3 | `visioncam-hardware-validation` | TODO |
| Playback never stalls recording or returns `PLAYBACK_BUSY` | 3 | `visioncam-hardware-validation` | TODO |
| Motion disabled above SXGA with explanatory message | 4 | `visioncam-webui` | TODO |
| Only one copy of `esp32-camera` linked | 0, 9 | `visioncam-build-flash` | TODO |

---

## Rules for recording results

1. Only `RUN PASS` after the command/test actually ran; otherwise
   `UNVERIFIED` (AGENTS rule 26).
2. Every performance claim needs its matching Phase 0 baseline row.
3. Any shortfall vs the R1 baseline is explained in
   `docs/REFERENCE_ANALYSIS.md`, not hidden.
