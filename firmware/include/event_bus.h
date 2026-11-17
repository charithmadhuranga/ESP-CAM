// event_bus.h — typed, bounded internal event bus (SPEC section 8.3).
//
// Queue capacity and overflow behaviour (AGENTS rule 12): the queue holds
// VC_EVENT_QUEUE_DEPTH events (fixed at create time, ~256 B of RAM). When
// it is full the OLDEST event is dropped to make room for the newest
// (drop-oldest): these are state notifications, so the latest information
// always matters more than a stale one, and the drop is counted and
// exposed in metrics rather than hidden. Publishing never blocks.

#pragma once

#include <cstdint>

#include "esp_err.h"

#include "events.h"

namespace visioncam {

// Fixed depth of the publish queue (bounded — AGENTS rule 12).
#define VC_EVENT_QUEUE_DEPTH    16
// Maximum number of subscriber callbacks (registration is static and
// bounded; a fifth subscribe() fails loudly instead of allocating).
#define VC_EVENT_MAX_HANDLERS   4

// Subscriber callback. Runs in the dispatcher task, one event at a time.
// CONTRACT: must be quick and must never block, take semaphores with long
// timeouts, or call back into eventBusPublish() in a loop — the bus is a
// notification path, not a work queue (SPEC 8.3: "event payloads must be
// small"; heavy work goes to the owning module's own task).
using EventHandler = void (*)(const Event& event);

// Creates the queue and the dispatcher task. Idempotent-safe: a second call
// returns ESP_ERR_INVALID_STATE.
esp_err_t eventBusInit();

// Publishes an event. Never blocks (timeout 0); on a full queue the oldest
// event is dropped first (see overflow policy above). Returns false only if
// the bus is not initialised. Assigns seq and ts_us.
bool eventBusPublish(EventType type, uint16_t value = 0, int32_t code = 0);

// Registers a subscriber. At most VC_EVENT_MAX_HANDLERS; returns
// ESP_ERR_NO_MEM when full (a bounded registry, no dynamic allocation).
// Register before publishing anything the handler cares about.
esp_err_t eventBusSubscribe(EventHandler handler);

struct EventBusStats {
    uint32_t published = 0;   // accepted by publish()
    uint32_t delivered = 0;   // handed to at least one handler
    uint32_t dropped = 0;     // evicted by a full queue (overflow policy)
};

void eventBusGetStats(EventBusStats& out);

}  // namespace visioncam
