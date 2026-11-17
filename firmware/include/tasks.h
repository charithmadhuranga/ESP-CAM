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
#define VC_STACK_HTTPD          4096

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
