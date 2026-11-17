// resource_monitor.cpp — see resource_monitor.h.

#include "resource_monitor.h"

#include <atomic>
#include <cstdio>
#include <cstring>

#include "esp_heap_caps.h"
#include "esp_log.h"
#include "esp_psram.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#include "event_bus.h"
#include "ring_log.h"
#include "tasks.h"

namespace visioncam {
namespace {

const char* TAG = "vc_resmon";

// Counters: each has a single writer task (see header). std::atomic with
// relaxed ordering — atomic because FreeRTOS tasks on two cores really do
// read them concurrently, relaxed because they carry no ordering contract
// beyond the value itself. IDF compiles with -Werror=volatile, and plain
// volatile would still be the wrong tool (no atomicity guarantee).
std::atomic<uint32_t> s_frames_captured{0};
std::atomic<uint32_t> s_frames_dropped{0};
std::atomic<uint32_t> s_frames_invalid{0};
std::atomic<uint32_t> s_stream_fps_x10{0};
std::atomic<uint32_t> s_capture_fps_x10{0};   // written by the monitor task only

// Monitor-owned state.
TaskHandle_t s_task = nullptr;
uint32_t s_last_frames = 0;
int64_t  s_last_sample_us = 0;
std::atomic<uint8_t> s_heap_state{0};   // 0 ok, 1 low, 2 critical
uint32_t s_low_streak = 0;   // consecutive samples below threshold

// Task table computed by the monitor task and copied out by snapshots.
portMUX_TYPE s_table_mux = portMUX_INITIALIZER_UNLOCKED;
TaskInfo s_table[VC_MAX_TASKS];
uint16_t s_table_count = 0;
uint16_t s_task_total = 0;

// Static scratch for enumeration: uxTaskGetSystemState needs one entry per
// task in the system (~20 on this build). Static, not stack: ~1.5 KB that
// must not live on the monitor's 4 KB stack.
constexpr UBaseType_t kMaxSystemTasks = 32;
TaskStatus_t s_status[kMaxSystemTasks];

void sampleTasks() {
    UBaseType_t n = uxTaskGetSystemState(s_status, kMaxSystemTasks, nullptr);
    if (n == 0) return;  // trace facility off or scheduler busy

    // Copy out, worst stack high-water mark first (the number an OOM starts
    // with). Bounded to VC_MAX_TASKS entries; the total is reported too.
    TaskInfo tmp[VC_MAX_TASKS];
    uint16_t kept = 0;
    for (UBaseType_t i = 0; i < n && kept < VC_MAX_TASKS; ++i) {
        const TaskStatus_t& st = s_status[i];
        TaskInfo& ti = tmp[kept];
        memset(&ti, 0, sizeof(ti));
        strncpy(ti.name, st.pcTaskName != nullptr ? st.pcTaskName : "?",
                sizeof(ti.name) - 1);
        ti.stack_hwm = st.usStackHighWaterMark;   // bytes on ESP-IDF
        ti.prio = static_cast<uint8_t>(st.uxCurrentPriority);
        // Mirror the struct's own gate for the affinity field: when the
        // mask exists, 0x1 = core 0, 0x2 = core 1, anything else = no
        // fixed affinity.
#if ( configUSE_CORE_AFFINITY == 1 ) && ( configNUMBER_OF_CORES > 1 )
        ti.core = (st.uxCoreAffinityMask == 0x1)   ? 0
                  : (st.uxCoreAffinityMask == 0x2) ? 1
                                                   : -1;
#else
        ti.core = -1;
#endif
        ti.state = static_cast<uint8_t>(st.eCurrentState);
        ++kept;
    }
    // Insertion sort ascending by stack_hwm (kept <= 16: tiny).
    for (uint16_t i = 1; i < kept; ++i) {
        TaskInfo key = tmp[i];
        uint16_t j = i;
        while (j > 0 && tmp[j - 1].stack_hwm > key.stack_hwm) {
            tmp[j] = tmp[j - 1];
            --j;
        }
        tmp[j] = key;
    }
    portENTER_CRITICAL(&s_table_mux);
    memcpy(s_table, tmp, sizeof(TaskInfo) * kept);
    s_table_count = kept;
    s_task_total = static_cast<uint16_t>(n);
    portEXIT_CRITICAL(&s_table_mux);
}

void setHeapState(uint8_t next, const char* why) {
    if (next == s_heap_state.load(std::memory_order_relaxed)) return;
    s_heap_state.store(next, std::memory_order_relaxed);
    if (next == 0) {
        ESP_LOGI(TAG, "heap recovered (%s)", why);
        VC_RING_INFO(TAG, "heap recovered (%s)", why);
        eventBusPublish(EventType::HeapRecovered);
    } else if (next == 1) {
        ESP_LOGW(TAG, "HEAP LOW: %s", why);
        VC_RING_WARN(TAG, "HEAP LOW: %s", why);
        eventBusPublish(EventType::HeapLow);
    } else {
        ESP_LOGE(TAG, "HEAP CRITICAL: %s", why);
        VC_RING_ERROR(TAG, "HEAP CRITICAL: %s", why);
        eventBusPublish(EventType::HeapCritical);
    }
}

void monitorTask(void*) {
    s_last_sample_us = esp_timer_get_time();
    // First sample immediately: a snapshot taken during early boot must
    // already see the task table (the first hardware run showed tasks=0
    // because the table only existed after the first 5 s period).
    sampleTasks();
    for (;;) {
        vTaskDelay(pdMS_TO_TICKS(VC_MONITOR_PERIOD_MS));

        // --- heap thresholds (SPEC 8.4.1) ---------------------------------
        const uint32_t internal = heap_caps_get_free_size(MALLOC_CAP_INTERNAL);
        const uint32_t largest =
            heap_caps_get_largest_free_block(MALLOC_CAP_INTERNAL);
        char why[96];
        if (largest < VC_HEAP_CRITICAL_BLOCK) {
            snprintf(why, sizeof(why),
                     "largest internal block %u B < %u B (free %u B)",
                     static_cast<unsigned>(largest), VC_HEAP_CRITICAL_BLOCK,
                     static_cast<unsigned>(internal));
            setHeapState(2, why);
        } else if (internal < VC_HEAP_LOW_BYTES) {
            snprintf(why, sizeof(why), "internal free %u B < %u B",
                     static_cast<unsigned>(internal), VC_HEAP_LOW_BYTES);
            setHeapState(1, why);
        } else {
            setHeapState(0, "internal heap above thresholds");
        }
        // While below threshold, re-log every 12th sample (~1 min) so the
        // evidence survives a crash without flooding the log.
        const uint8_t heap_now = s_heap_state.load(std::memory_order_relaxed);
        if (heap_now > 0 && ++s_low_streak % 12 == 0) {
            VC_RING_WARN(TAG, "heap still %s: free %u B, largest %u B",
                         heap_now == 2 ? "CRITICAL" : "LOW",
                         static_cast<unsigned>(internal),
                         static_cast<unsigned>(largest));
        } else if (heap_now == 0) {
            s_low_streak = 0;
        }

        // --- capture rate from the frame manager's counter -----------------
        const int64_t now = esp_timer_get_time();
        const int64_t elapsed_ms = (now - s_last_sample_us) / 1000;
        const uint32_t frames = s_frames_captured.load(std::memory_order_relaxed);
        if (elapsed_ms > 0) {
            const uint32_t delta = frames - s_last_frames;
            s_last_frames = frames;
            s_last_sample_us = now;
            // fps_x10 = delta / elapsed_s * 10
            s_capture_fps_x10.store(
                static_cast<uint32_t>(delta * 10000ull / elapsed_ms),
                std::memory_order_relaxed);
        }

        sampleTasks();
    }
}

}  // namespace

const char* resetReasonName(esp_reset_reason_t reason) {
    switch (reason) {
        case ESP_RST_POWERON:   return "POWERON";
        case ESP_RST_EXT:       return "EXT";
        case ESP_RST_SW:        return "SW";
        case ESP_RST_PANIC:     return "PANIC";
        case ESP_RST_INT_WDT:   return "INT_WDT";
        case ESP_RST_TASK_WDT:  return "TASK_WDT";
        case ESP_RST_WDT:       return "WDT";
        case ESP_RST_DEEPSLEEP: return "DEEPSLEEP";
        case ESP_RST_BROWNOUT:  return "BROWNOUT";
        case ESP_RST_SDIO:      return "SDIO";
        default:                return "UNKNOWN";
    }
}

esp_err_t resourceMonitorInit() {
    if (s_task != nullptr) return ESP_ERR_INVALID_STATE;
    const BaseType_t ok =
        xTaskCreate(monitorTask, "vc_monitor", VC_STACK_MONITOR, nullptr,
                    VC_PRIO_MONITOR, &s_task);
    if (ok != pdPASS) {
        s_task = nullptr;
        return ESP_ERR_NO_MEM;
    }
    ESP_LOGI(TAG, "monitor up: period %d ms, heap low <%u B, critical block <%u B",
             VC_MONITOR_PERIOD_MS, VC_HEAP_LOW_BYTES, VC_HEAP_CRITICAL_BLOCK);
    return ESP_OK;
}

void resourceMonitorSnapshot(ResourceSnapshot& out) {
    memset(&out, 0, sizeof(out));

    out.uptime_s = static_cast<uint32_t>(esp_timer_get_time() / 1000000);
    out.reset_reason = static_cast<uint8_t>(esp_reset_reason());
    out.boot_count = ringLogBootCount();
    out.abnormal_boots = ringLogAbnormalBoots();

    out.free_heap = esp_get_free_heap_size();
    out.internal_free = heap_caps_get_free_size(MALLOC_CAP_INTERNAL);
    out.min_free_heap = esp_get_minimum_free_heap_size();
    out.largest_block = heap_caps_get_largest_free_block(MALLOC_CAP_INTERNAL);
    const size_t psram = esp_psram_get_size();
    out.psram_total = static_cast<uint32_t>(psram);
    if (psram > 0) {
        out.psram_free = heap_caps_get_free_size(MALLOC_CAP_SPIRAM);
        out.psram_largest =
            heap_caps_get_largest_free_block(MALLOC_CAP_SPIRAM);
    }
    out.heap_state = s_heap_state.load(std::memory_order_relaxed);

    portENTER_CRITICAL(&s_table_mux);
    out.task_count = s_task_total;
    out.tasks_reported = s_table_count;
    memcpy(out.tasks, s_table, sizeof(TaskInfo) * s_table_count);
    portEXIT_CRITICAL(&s_table_mux);

    out.frames_captured = s_frames_captured.load(std::memory_order_relaxed);
    out.frames_dropped = s_frames_dropped.load(std::memory_order_relaxed);
    out.frames_invalid = s_frames_invalid.load(std::memory_order_relaxed);
    out.stream_fps_x10 = s_stream_fps_x10.load(std::memory_order_relaxed);
    out.capture_fps_x10 = s_capture_fps_x10.load(std::memory_order_relaxed);
}

void resMonCountFrame() { s_frames_captured.fetch_add(1, std::memory_order_relaxed); }
void resMonCountDropped() { s_frames_dropped.fetch_add(1, std::memory_order_relaxed); }
void resMonCountInvalid() { s_frames_invalid.fetch_add(1, std::memory_order_relaxed); }
void resMonSetStreamFpsX10(uint16_t fps_x10) {
    s_stream_fps_x10.store(fps_x10, std::memory_order_relaxed);
}

}  // namespace visioncam
