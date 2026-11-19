// tasks.h — the single source of truth for FreeRTOS task priorities and
// stack sizes (firmware/AGENTS.md conventions).
//
// Every number here is a STARTING ESTIMATE (SPEC section 8.1): priorities
// are chosen against the measured R1 reference (SPEC 8.1.1 — capture 6,
// HTTP/stream 5, playback 4, telemetry 3) and stacks against R1's 4 KB
// capture / 4 KB HTTP on the original ESP32. The stack high-water mark of
// each task is MEASURED on the board and recorded in docs/ARCHITECTURE.md
// section 4 at the end of Phase 1 — never estimated twice.

#pragma once

// --- frame manager capture task --------------------------------------------
// Sole owner of esp_camera_fb_get/return (AGENTS rule 9). Priority 6 puts
// it above the HTTP server (5) so a slow stream client can never delay
// frame acquisition — the pattern R1 measures on this board.
#define VC_PRIO_CAPTURE         6
#define VC_STACK_CAPTURE        5120   // bytes (R1: 4096 for its capture task)

// --- esp_http_server's own task --------------------------------------------
// The stack is set in the web/ server config from this constant so the two
// can never drift apart (SPEC 6.0: R1 uses 4 KB on ESP32, 7 KB on S3).
// Long-running responses (stream) run inside this task.
//
// 4096 was R1's figure, but R1's handlers do not nest the way ours do: a
// single request here can be on the stack simultaneously with the auth
// middleware (readSessionCookie char[512]), the handler's own buffers
// (body[256] + user[64] + pass[64] + csrf_b64[64] + token_b64[64] +
// set_cookie[512]), sendEnvelope's char[512] and ringLogWrite's
// line[192]+msg[144] — ~1.9 KB before esp_http_server's own frames and
// any callee (mbedtls SHA-256 inside authCreateSession, snprintf).
//
// Two runs crashed at the same point with StoreProhibited at 0xffffffa0:
// vPortYieldFromInt computed (TCB.pxEndOfStack - offset_cpsa) & ~0xF from
// a ZEROED pxEndOfStack, i.e. a task TCB in the heap had been overwritten
// by a stack that ran off its bottom (a stack grows down into the block
// below it, and FreeRTOS allocates the stack BEFORE the TCB, so an earlier
// task's TCB sits there). Evidence: docs/evidence/phase2-stack-boot-*.log.
// 8192 also leaves headroom for the MJPEG stream handler that will run in
// this same task. MEASURE the HWM in /metrics after Phase 2 and record it
// in docs/ARCHITECTURE.md §4 — do not shrink this on estimate.
#define VC_STACK_HTTPD          8192

// --- event bus dispatcher ---------------------------------------------------
// Runs subscriber callbacks one at a time. Callbacks must be quick and must
// never block (contract documented in event_bus.h).
#define VC_PRIO_EVENTBUS        4
#define VC_STACK_EVENTBUS       3072

// --- resource monitor -------------------------------------------------------
// Samples heap/task metrics and enforces the heap warning thresholds
// (SPEC 8.4.1: ~32 KB free, ~16 KB largest block). Low priority: metrics
// must be the first thing starved, never the capture loop.
#define VC_PRIO_MONITOR         3
#define VC_STACK_MONITOR        4096

// --- network manager --------------------------------------------------------
// Reconnect-with-backoff task (Phase 1 task 1.1.2). The esp_wifi driver's
// own tasks and the TCP/IP task run above all of these — they are IDF's.
#define VC_PRIO_NETWORK         4
#define VC_STACK_NETWORK        4096

// --- shared limits ----------------------------------------------------------
// Maximum stream clients on the original ESP32 (SPEC 4.1: default 1).
#define VC_MAX_STREAM_CLIENTS   1

// --- storage writer task (Phase 3) -----------------------------------------
// Sole file writer for recording segments: pulls frame leases from its
// bounded queue and writes 00dc JPEG chunks to SD in sector-aligned
// whole-buffer writes. Runs BELOW the capture task (6) so camera
// acquisition can never be delayed by SD card latency; runs ABOVE
// playback (4) so recording always wins SD access (SPEC 4.6 concurrency
// rule). Uses an 8 KB staging buffer + FATFS frames on its stack.
#define VC_PRIO_WRITER          5
#define VC_STACK_WRITER         6144

// --- playback task (Phase 3) -----------------------------------------------
// Server-side MJPEG replay from SD. Runs BELOW the writer (5) so
// recording always wins SD card access. Its stack holds read buffers +
// multipart framing; JPEG payloads come from the heap. Consumes one of
// the limited stream slots (max VC_MAX_STREAM_CLIENTS).
#define VC_PRIO_PLAYBACK        4
#define VC_STACK_PLAYBACK       5120

// --- motion engine task (Phase 4) ------------------------------------------
// Decodes JPEG to a small grayscale bitmap, compares with the previous
// sample, and publishes MotionStarted/MotionEnded events. Runs at MEDIUM
// priority (below capture at 6, above HTTP at 5): motion analysis must
// never delay frame acquisition but should not starve either. Uses a
// fixed 96x96 grayscale buffer (~9 KB) plus a diff/change map.
// Sample rate is higher while idle (~5 checks/s) and lower while
// recording (1 check/2 s) — both configurable.
#define VC_PRIO_MOTION          4
#define VC_STACK_MOTION         8192

// --- timelapse scheduler task (Phase 5) ------------------------------------
// Captures JPEG frames on a configurable interval for timelapse projects.
// Appends frames directly to a per-project AVI (direct-to-AVI mode, default)
// using the Phase 3 AVI writer code. Runs at LOW priority (below motion at
// 4) so streaming, recording and motion always win. Uses the frame manager
// lease mechanism like all other consumers.
// Sample interval is in seconds (configurable, default 5 s); captures one
// frame per interval — no continuous streaming when idle.
#define VC_PRIO_TIMELAPSE       3
#define VC_STACK_TIMELAPSE      6144

// --- AI inference task (Phase 6) -------------------------------------------
// Runs inference on selected frames (after motion/event trigger per R1's
// usage pattern). Runs at LOW priority (below timelapse at 3) so streaming,
// recording, motion and timelapse always win. Uses a bounded tensor arena
// allocated from PSRAM; never blocks streaming/storage on inference.
#define VC_PRIO_AI              2
#define VC_STACK_AI             8192

// --- MQTT worker task (Phase 7) ---------------------------------------------
// Publishes availability, state and events on visioncam/{device_id}/...
// Runs at LOW priority (below AI at 2) so camera/storage/motion/timelapse
// always win. Uses a bounded publish queue; never blocks camera/storage
// tasks on broker availability. Connects with exponential backoff + jitter.
// Publishes retained online/offline availability with Last Will.
#define VC_PRIO_MQTT              1
#define VC_STACK_MQTT             8192

// --- shared limits ---
// Maximum concurrent inference requests in the bounded queue.
#define VC_MAX_AI_QUEUE         4
