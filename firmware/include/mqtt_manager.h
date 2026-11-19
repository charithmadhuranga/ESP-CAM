// mqtt_manager.h — MQTT + Home Assistant integration (Phase 7 task 7.1-7.4;
// SPEC §4.10, §4.11, §6.9).
//
// MQTT runs in its own task at LOW priority so camera/storage/motion/
// timelapse tasks never block on broker availability (AGENTS rule 11).
// Uses a bounded publish queue with drop/coalesce for low-priority
// telemetry when full. Publishes retained online/offline availability
// with Last Will.
//
// All MQTT messages use the topic prefix visioncam/{device_id}/... and
// never carry credentials or binary images (deliberate divergence from R1
// which published images over MQTT). Snapshot paths are device-local
// paths, not automatically accessible to Home Assistant.

#pragma once

#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

#include "esp_err.h"

namespace visioncam {

// ---- MQTT configuration (SPEC §4.10) ------------------------------------

struct MqttConfig {
    bool enabled = false;
    std::string host;
    uint16_t port = 1883;
    std::string username;
    std::string password;        // stored in NVS, never returned by GET
    std::string client_id;       // defaults to device_id if empty
    std::string base_topic = "visioncam/{device_id}";
    uint8_t qos = 0;             // 0 for telemetry, 1 for important events
    uint16_t keepalive = 60;     // seconds
    bool tls_enable = false;
    std::string tls_cert;        // PEM certificate, stored in NVS
    bool ha_discovery = false;   // Home Assistant discovery enable
    std::string ha_prefix = "homeassistant";
    uint32_t publish_interval_ms = 5000;  // telemetry publish interval
    std::vector<std::string> event_categories;  // which events to publish
    bool availability_retained = true;
};

// ---- MQTT status ---------------------------------------------------------

struct MqttStatus {
    bool enabled = false;
    bool connected = false;
    bool broker_reachable = false;
    uint32_t connects_total = 0;
    uint32_t disconnects_total = 0;
    uint32_t publishes_total = 0;
    uint32_t publishes_failed = 0;
    uint32_t queue_dropped = 0;
    uint32_t queue_depth = 0;
    int32_t rssi = 0;            // last known Wi-Fi RSSI
    std::string last_error;
};

// ---- Publish queue entry (bounded, documented overflow) ------------------

struct MqttPublishEntry {
    std::string topic;
    std::string payload;         // small payload only, never images
    uint8_t qos = 0;
    bool retain = false;
    uint32_t priority = 0;       // higher = more important, kept on overflow
};

// ---- Public API ----------------------------------------------------------

// Initialise MQTT manager. Call after event bus, network manager and
// NVS are available. Spawns the MQTT worker task.
esp_err_t mqttManagerInit(const MqttConfig* config);

// Shut down MQTT manager — stops worker, publishes retained offline LWT.
esp_err_t mqttManagerShutdown();

// Get config (copy). Password is NOT included (never returned by GET).
esp_err_t mqttManagerGetConfig(MqttConfig* out);

// Set config (parse → validate → persist → apply → return sanitised,
// SPEC 7 process). Partial updates overlay on current config.
esp_err_t mqttManagerSetConfig(const MqttConfig* candidate,
                               MqttConfig* effective);

// Get MQTT status (connection state, queue depth, publish stats).
esp_err_t mqttManagerStatus(MqttStatus* out);

// Test MQTT connection — attempts a single connect/publish/disconnect cycle.
esp_err_t mqttManagerTestConnection();

// Reconnect — force a reconnect attempt.
esp_err_t mqttManagerReconnect();

// Publish test message — publishes a test payload to the base topic.
esp_err_t mqttManagerTestPublish();

// Home Assistant discovery republish — re-sends all retained discovery
// configs (triggered by UI action or HA birth message).
esp_err_t mqttManagerRepublishDiscovery();

// Get HA discovery status.
esp_err_t mqttManagerHaStatus(bool* out_enabled, bool* out_connected,
                              uint32_t* out_entities_published);

// ---- Internal: called by event bus / other modules ----------------------

// Publish an event to the appropriate MQTT topic. Called by the event bus
// subscriber when motion/detection/recording/snapshot/storage events occur.
// Bounded queue with drop/coalesce for low-priority entries.
esp_err_t mqttPublishEvent(const std::string& event_type,
                           const std::string& source,
                           const std::string& label,
                           float confidence,
                           const std::string& snapshot_id,
                           uint32_t timestamp);

// Publish telemetry (storage used/free/percentage, Wi-Fi RSSI, inference
// time). Called periodically by the resource monitor.
esp_err_t mqttPublishTelemetry(const std::string& key, double value,
                               const std::string& unit);

}  // namespace visioncam
