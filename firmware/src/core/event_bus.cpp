// event_bus.cpp — see event_bus.h.

#include "event_bus.h"

#include <cstdio>

#include "esp_log.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"
#include "freertos/task.h"

#include "tasks.h"

namespace visioncam {
namespace {

const char* TAG = "vc_events";

QueueHandle_t s_queue = nullptr;
EventHandler s_handlers[VC_EVENT_MAX_HANDLERS] = {};
uint8_t s_handler_count = 0;
uint32_t s_seq = 0;
EventBusStats s_stats = {};
// Guards s_stats only (publish can be called from several tasks). The queue
// has its own internal locking; this mux is held for three increments, so it
// never becomes a critical-section hotspot — events are notifications, not
// per-frame traffic (SPEC 8.3).
portMUX_TYPE s_stats_mux = portMUX_INITIALIZER_UNLOCKED;

void countStat(uint32_t EventBusStats::*field) {
    portENTER_CRITICAL(&s_stats_mux);
    (s_stats.*field)++;
    portEXIT_CRITICAL(&s_stats_mux);
}

void dispatchTask(void*) {
    Event ev;
    for (;;) {
        // Block forever: the dispatcher exists only to deliver events.
        if (xQueueReceive(s_queue, &ev, portMAX_DELAY) != pdTRUE) continue;
        countStat(&EventBusStats::delivered);
        for (uint8_t i = 0; i < s_handler_count; ++i) {
            s_handlers[i](ev);
        }
    }
}

}  // namespace

const char* eventTypeName(EventType type) {
    switch (type) {
        case EventType::MotionStarted:            return "MotionStarted";
        case EventType::MotionEnded:              return "MotionEnded";
        case EventType::ObjectDetected:           return "ObjectDetected";
        case EventType::SnapshotCreated:          return "SnapshotCreated";
        case EventType::RecordingStarted:         return "RecordingStarted";
        case EventType::RecordingStopped:         return "RecordingStopped";
        case EventType::StorageWarning:           return "StorageWarning";
        case EventType::StorageError:             return "StorageError";
        case EventType::MqttConnected:            return "MqttConnected";
        case EventType::MqttDisconnected:         return "MqttDisconnected";
        case EventType::AiProviderFailed:         return "AiProviderFailed";
        case EventType::NetworkStorageUploadFailed:
            return "NetworkStorageUploadFailed";
        case EventType::CameraStarted:            return "CameraStarted";
        case EventType::CameraFailed:             return "CameraFailed";
        case EventType::CameraRecovered:          return "CameraRecovered";
        case EventType::WifiConnected:            return "WifiConnected";
        case EventType::WifiDisconnected:         return "WifiDisconnected";
        case EventType::HeapLow:                  return "HeapLow";
        case EventType::HeapCritical:             return "HeapCritical";
        case EventType::HeapRecovered:            return "HeapRecovered";
        case EventType::_Count:                   break;
    }
    return "Unknown";
}

esp_err_t eventBusInit() {
    if (s_queue != nullptr) return ESP_ERR_INVALID_STATE;

    // Fixed depth, fixed item size — bounded by construction (rule 12).
    s_queue = xQueueCreate(VC_EVENT_QUEUE_DEPTH, sizeof(Event));
    if (s_queue == nullptr) return ESP_ERR_NO_MEM;

    const BaseType_t ok = xTaskCreate(dispatchTask, "vc_eventbus",
                                      VC_STACK_EVENTBUS, nullptr,
                                      VC_PRIO_EVENTBUS, nullptr);
    if (ok != pdPASS) {
        vQueueDelete(s_queue);
        s_queue = nullptr;
        return ESP_ERR_NO_MEM;
    }
    ESP_LOGI(TAG, "event bus up: depth %d, max %d handler(s)",
             VC_EVENT_QUEUE_DEPTH, VC_EVENT_MAX_HANDLERS);
    return ESP_OK;
}

bool eventBusPublish(EventType type, uint16_t value, int32_t code) {
    if (s_queue == nullptr) return false;

    Event ev;
    ev.type = type;
    ev.value = value;
    ev.code = code;
    ev.seq = ++s_seq;
    ev.ts_ms = static_cast<uint32_t>(esp_timer_get_time() / 1000);

    if (xQueueSend(s_queue, &ev, 0) == pdTRUE) {
        countStat(&EventBusStats::published);
        return true;
    }
    // Overflow policy: drop the oldest event, keep the newest state.
    Event discarded;
    if (xQueueReceive(s_queue, &discarded, 0) == pdTRUE) {
        countStat(&EventBusStats::dropped);
    }
    if (xQueueSend(s_queue, &ev, 0) == pdTRUE) {
        countStat(&EventBusStats::published);
        return true;
    }
    return false;  // queue vanished mid-flight (cannot happen in practice)
}

esp_err_t eventBusSubscribe(EventHandler handler) {
    if (handler == nullptr) return ESP_ERR_INVALID_ARG;
    if (s_handler_count >= VC_EVENT_MAX_HANDLERS) return ESP_ERR_NO_MEM;
    s_handlers[s_handler_count++] = handler;
    return ESP_OK;
}

void eventBusGetStats(EventBusStats& out) { out = s_stats; }

}  // namespace visioncam
