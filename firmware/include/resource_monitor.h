// resource_monitor.h — resource and performance metrics (SPEC section 8.4).
//
// One snapshot type covers the whole SPEC 8.4 list; fields whose owning
// subsystem arrives in a later phase (SD, recording, AI, MQTT) exist from
// day one and read zero until that phase fills them, so /metrics never
// changes shape (AGENTS rule 21: versioned payloads).
//
// Heap thresholds (SPEC 8.4.1) live here and are enforced by the monitor
// task: internal free heap < 32 KB -> HeapLow, largest internal block
// < 16 KB -> HeapCritical, with HeapRecovered on the way back. Crossing a
// threshold logs (serial + RTC ring log) and publishes an event — the
// thresholds are the seed of every later degraded mode.

#pragma once

#include <cstdint>

#include "esp_err.h"
#include "esp_system.h"

namespace visioncam {

// Thresholds from SPEC 8.4.1 (R1-derived starting points). Measured
// against the INTERNAL heap: with PSRAM in the default-heap pool a
// combined figure would never trip while internal DRAM starves.
#define VC_HEAP_LOW_BYTES        32768u
#define VC_HEAP_CRITICAL_BLOCK   16384u

// Maximum tasks reported per snapshot, worst stack first. The system task
// count is reported separately, so a task beyond this cap is counted but
// not listed (bounded — no dynamic allocation, AGENTS rule 13).
#define VC_MAX_TASKS             16

// Sample period of the monitor task.
#define VC_MONITOR_PERIOD_MS     5000

struct TaskInfo {
    char     name[16];      // configMAX_TASK_NAME_LEN is 16 in this IDF
    uint32_t stack_hwm;     // minimum free stack ever, BYTES (IDF's
                            // StackType_t is byte-sized)
    uint8_t  prio;
    int8_t   core;          // -1 = no affinity
    uint8_t  state;         // FreeRTOS eTaskState
};

struct ResourceSnapshot {
    // --- identity / uptime -------------------------------------------------
    uint32_t uptime_s = 0;
    uint8_t  reset_reason = 0;      // esp_reset_reason_t
    uint32_t boot_count = 0;        // RTC-retained boots (ring log)
    uint32_t abnormal_boots = 0;    // consecutive pre-health boots

    // --- heap --------------------------------------------------------------
    uint32_t free_heap = 0;         // esp_get_free_heap_size() (internal+PSRAM)
    uint32_t internal_free = 0;     // MALLOC_CAP_INTERNAL — thresholds use this
    uint32_t min_free_heap = 0;     // lifetime minimum (esp_get_minimum_free_heap_size)
    uint32_t largest_block = 0;     // largest INTERNAL free block
    uint32_t psram_total = 0;
    uint32_t psram_free = 0;
    uint32_t psram_largest = 0;
    uint8_t  heap_state = 0;        // 0 ok, 1 HeapLow, 2 HeapCritical

    // --- tasks (worst stack first) ----------------------------------------
    uint16_t task_count = 0;        // tasks in the system
    uint16_t tasks_reported = 0;    // entries filled in tasks[]
    TaskInfo tasks[VC_MAX_TASKS] = {};

    // --- performance counters (SPEC 8.4) ----------------------------------
    uint32_t frames_captured = 0;   // frame manager
    uint32_t frames_dropped = 0;    // dropped for slow consumers
    uint32_t frames_invalid = 0;    // len==0 / oversized (SPEC 9.1 rule 5)
    uint32_t capture_fps_x10 = 0;   // measured by the monitor task
    uint32_t stream_fps_x10 = 0;    // set by the stream handler
    uint32_t sd_write_ms_last = 0;      // Phase 2
    uint32_t sd_write_failures = 0;     // Phase 2
    uint32_t recording_queue_depth = 0; // Phase 3
    uint32_t ai_inference_ms = 0;       // Phase 6
    uint32_t mqtt_queue_depth = 0;      // Phase 7
};

// esp_reset_reason() has no string accessor; shared by the ring log, the
// monitor and the boot banner so the spelling never diverges.
const char* resetReasonName(esp_reset_reason_t reason);

// Starts the monitor task (sampling + heap thresholds + task table).
// Idempotent-safe: second call returns ESP_ERR_INVALID_STATE.
esp_err_t resourceMonitorInit();

// Cheap live sample: heap/uptime/counters are read now, the task table is
// the monitor's last computation. Task context only (not an ISR).
void resourceMonitorSnapshot(ResourceSnapshot& out);

// Counter updates from owning modules (single writer each — the frame
// manager counts frames, the stream handler publishes stream FPS).
void resMonCountFrame();          // one valid frame captured
void resMonCountDropped();        // frame dropped for a slow consumer
void resMonCountInvalid();        // malformed frame rejected (SPEC 9.1)
void resMonSetStreamFpsX10(uint16_t fps_x10);

}  // namespace visioncam
