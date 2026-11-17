// events.h — typed internal events (SPEC section 8.3).
//
// Payloads must be SMALL: large image data is referenced by media ID/path
// and is never placed into the event queue (SPEC 8.3, AGENTS rule 12 for
// the queue itself — see event_bus.h for the bounded queue and its
// documented overflow policy).

#pragma once

#include <cstdint>

namespace visioncam {

enum class EventType : uint8_t {
    // --- SPEC 8.3 list (all present from day one so the enum never
    // renumbers; producers arrive with their phase) ------------------------
    MotionStarted = 0,
    MotionEnded,
    ObjectDetected,
    SnapshotCreated,
    RecordingStarted,
    RecordingStopped,
    StorageWarning,
    StorageError,
    MqttConnected,
    MqttDisconnected,
    AiProviderFailed,
    NetworkStorageUploadFailed,

    // --- Phase 1 additions ------------------------------------------------
    CameraStarted,        // camera bring-up succeeded
    CameraFailed,         // SPEC 9.1: NULL-frame recovery limit reached
    CameraRecovered,      // a recovery rung succeeded after failure
    WifiConnected,        // real association + IP (not before)
    WifiDisconnected,
    HeapLow,              // SPEC 8.4.1 thresholds
    HeapCritical,
    HeapRecovered,

    _Count  // keep last: the bus validates against it
};

// Human-readable name for logs and metrics. Never returns nullptr.
const char* eventTypeName(EventType type);

// The whole event. 16 bytes — small enough that a full queue costs ~256 B.
// ts_ms is milliseconds since boot (wraps at ~49 days): event ordering
// needs no better resolution, and a 64-bit timestamp would push the struct
// to 24 bytes through alignment alone.
struct Event {
    EventType type = EventType::_Count;
    uint16_t  value = 0;   // small payload: RSSI, attempt count, client slot…
    int32_t   code = 0;    // error/health code (esp_err_t, threshold id)
    uint32_t  seq = 0;     // assigned by the bus at publish time
    uint32_t  ts_ms = 0;   // esp_timer ms at publish time
};

static_assert(sizeof(Event) <= 16,
              "SPEC 8.3: event payloads must be small — image data is "
              "referenced by media ID, never queued");

}  // namespace visioncam
