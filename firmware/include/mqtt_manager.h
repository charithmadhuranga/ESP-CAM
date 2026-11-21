// mqtt_manager.h — MQTT worker + Home Assistant discovery
// (Phase 7 tasks 7.1-7.4; SPEC §4.10, §4.11, §6.9).
//
// A separate worker task: camera/storage/recording tasks NEVER wait on the
// broker (AGENTS rule 11). Outbound messages go through a bounded queue
// (drop-oldest for telemetry, QoS 1 for events) so a broker outage cannot
// grow RAM or block anything — local recording and motion keep running
// whether the broker is present or not (SPEC §9).
//
// Secrets (password) live in NVS only and are never returned by GET
// (AGENTS rule 17); the response carries a boolean instead.

#pragma once

#include <atomic>
#include <cstdint>
#include <string>

#include "esp_err.h"

namespace visioncam {

// --- Configuration ----------------------------------------------------------
struct MqttConfig {
    bool enabled = false;
    std::string broker_uri;        // mqtt://host:port | mqtts://host:port (empty = unconfigured)
    std::string username;
    std::string password;          // write-only; never in a GET response
    bool tls_verify = true;        // mqtts certificate/CN verification
    std::string base_topic = "visioncam";
    std::string client_id;         // empty -> derived from the device id
    bool ha_discovery = true;      // publish Home Assistant discovery configs
    std::string ha_prefix = "homeassistant";
    uint32_t keepalive_s = 30;
    uint32_t timeout_ms = 5000;    // connect/publish timeout
};

// --- Status ---------------------------------------------------------------
struct MqttStatus {
    bool enabled = false;
    // Plain bool: the manager keeps an atomic in its own translation unit
    // and mirrors it here, so this struct stays copyable for callers.
    bool connected = false;
    std::string state;             // disabled | unconfigured | connecting |
                                   // online | offline | error
    uint32_t connects = 0;
    uint32_t disconnects = 0;
    uint32_t published = 0;
    uint32_t dropped = 0;          // queue overflow (documented policy)
    std::string last_error;
    bool ha_discovery_published = false;
};

// --- Public API ------------------------------------------------------------

// Initialise the worker: load config from NVS, then start the task. Safe to
// call with no broker configured (state stays "unconfigured"). Idempotent.
esp_err_t mqttManagerInit();

// Stop the worker, release the client. The app keeps running without it.
esp_err_t mqttManagerShutdown();

// Get config (copy). The password is NOT included: callers that need to
// know whether one is stored use mqttManagerStatus/mqttManagerPasswordSet.
esp_err_t mqttManagerGetConfig(MqttConfig* out);

// Whether a broker password is stored (AGENTS rule 17).
bool mqttManagerPasswordSet();

// SPEC §7 update process: validate → persist → apply → return the
// sanitized effective config. A connection change reconnects live.
esp_err_t mqttManagerSetConfig(const MqttConfig* candidate,
                               MqttConfig* effective);

// Status snapshot.
esp_err_t mqttManagerStatus(MqttStatus* out);

// Republish the Home Assistant discovery configs (UI action, task 7.3 #19).
esp_err_t mqttManagerRepublishDiscovery();

// One-shot connection test: connects to the configured broker, reports the
// result, then disconnects. Never blocks the caller beyond its timeout.
esp_err_t mqttManagerTestConnection();

// Publish one retained test message on the device's own topic tree
// (POST /integrations/mqtt/test-publish). Requires a live connection.
esp_err_t mqttManagerPublishTest();

// Stable device identity used in topics/unique_ids (generated once, NVS).
const char* mqttManagerDeviceId();

}  // namespace visioncam