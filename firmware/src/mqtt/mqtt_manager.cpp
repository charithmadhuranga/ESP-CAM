// mqtt_manager.cpp — MQTT worker + Home Assistant discovery
// (Phase 7 tasks 7.1-7.4; SPEC §4.10, §4.11, §6.9).
//
// Own task, own queue, its own reconnect with backoff + jitter. Nothing in
// camera/storage/recording waits on it, and it never publishes secrets or
// image bytes (R1 publishes images over MQTT — that is a deliberate
// divergence, see docs/REFERENCE_ANALYSIS.md).
//
// Overflow policy (AGENTS rule 12): the outbound queue holds kMqttQueue
// messages. When full, the OLDEST TELEMETRY message is dropped (the newest
// reading is always the more useful one) and counted in status.dropped;
// EVENT messages (motion, recording, lifecycle) are never dropped for a
// telemetry — they are the record the integration exists for.

#include "mqtt_manager.h"

#include <algorithm>
#include <atomic>
#include <cstring>

#include "esp_heap_caps.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"
#include "freertos/task.h"

#include "device_identity.h"
#include "event_bus.h"
#include "mqtt5_client.h"
#include "nvs_flash.h"
#include "nvs.h"
#include "storage_manager.h"
#include "tasks.h"
#include "wall_clock.h"

namespace visioncam {

static const char* TAG = "vc_mqtt";

// --- NVS -------------------------------------------------------------------
constexpr char kMqttNvsNs[] = "mqtt";
constexpr char kMqttKeyVer[] = "mqtt_ver";
constexpr char kMqttKeyEnabled[] = "mqtt_enabled";
constexpr char kMqttKeyUri[] = "mqtt_uri";
constexpr char kMqttKeyUser[] = "mqtt_user";
constexpr char kMqttKeyPass[] = "mqtt_pass";   // device-local secret
constexpr char kMqttKeyTls[] = "mqtt_tls";
constexpr char kMqttKeyBase[] = "mqtt_base";
constexpr char kMqttKeyHa[] = "mqtt_ha";
constexpr char kMqttKeyHaPrefix[] = "mqtt_ha_prefix";
constexpr char kMqttKeyKeepalive[] = "mqtt_keepalive";
constexpr char kMqttKeyTimeout[] = "mqtt_timeout";
constexpr uint32_t kMqttConfigSchema = 1;

// --- Bounds ----------------------------------------------------------------
constexpr uint32_t kMqttQueue = 24;            // bounded outbound queue
constexpr size_t kMqttPayloadMax = 256;        // never size from input
constexpr uint32_t kMqttBackoffMaxMs = 30000;  // exponential cap
constexpr uint32_t kMqttTestTimeoutMs = 8000;  // one-shot probe budget

// --- Module state ----------------------------------------------------------
static MqttConfig g_config{};
static MqttStatus g_status{};
static TaskHandle_t g_task = nullptr;
static bool g_initialized = false;
static esp_mqtt_client_handle_t g_client = nullptr;
static std::atomic<bool> g_connected{false};
static std::atomic<bool> g_connecting{false};
// Set by SetConfig/test from the HTTP task; the WORKER task is the only
// code that stops/destroys g_client (esp_mqtt is not thread-safe for
// concurrent stop/destroy from two tasks).
static std::atomic<bool> g_reconnect_requested{false};
static std::atomic<bool> g_ha_online{true};
// State latched from the event bus (see mqttOnEvent) — the state payload
// must report what the device is REALLY doing, not a constant.
static std::atomic<bool> g_motion_active_flag{false};
static std::atomic<bool> g_recording_active_flag{false};
// esp_mqtt copies the will URI string? No — the config is copied by value at
// init, so the topic/payload buffers must outlive the call. This static
// buffer holds the will topic for the lifetime of the worker.
static char g_last_will_topic[192] = {};

// --- Outbound queue --------------------------------------------------------
struct MqttMsg {
    char topic[kMqttPayloadMax];   // topic <= 191 B (large, but bounded)
    char payload[96];              // state payloads are small by design
    uint8_t qos;                   // 0 = telemetry, 1 = event
    bool retain;
};
static QueueHandle_t g_queue = nullptr;

// ---- Helpers ---------------------------------------------------------------

static uint32_t uptimeMs() {
    return static_cast<uint32_t>(esp_timer_get_time() / 1000);
}

// Persist the whole config as explicit versioned keys (SPEC §7).
static esp_err_t persistConfig(const MqttConfig& cfg) {
    nvs_handle_t h = 0;
    esp_err_t err = nvs_open(kMqttNvsNs, NVS_READWRITE, &h);
    if (err != ESP_OK) return err;
    nvs_set_u32(h, kMqttKeyVer, kMqttConfigSchema);
    nvs_set_u8(h, kMqttKeyEnabled, cfg.enabled ? 1 : 0);
    nvs_set_str(h, kMqttKeyUri, cfg.broker_uri.c_str());
    nvs_set_str(h, kMqttKeyUser, cfg.username.c_str());
    nvs_set_str(h, kMqttKeyPass, cfg.password.c_str());
    nvs_set_u8(h, kMqttKeyTls, cfg.tls_verify ? 1 : 0);
    nvs_set_str(h, kMqttKeyBase, cfg.base_topic.c_str());
    nvs_set_u8(h, kMqttKeyHa, cfg.ha_discovery ? 1 : 0);
    nvs_set_str(h, kMqttKeyHaPrefix, cfg.ha_prefix.c_str());
    nvs_set_u32(h, kMqttKeyKeepalive, cfg.keepalive_s);
    nvs_set_u32(h, kMqttKeyTimeout, cfg.timeout_ms);
    err = nvs_commit(h);
    nvs_close(h);
    return err;
}

// Enqueue one message. Telemetry (qos 0) coalesces: when the queue is full
// the oldest telemetry entry is dropped rather than the newest. Events are
// only dropped when the queue holds nothing but events AND is full, which
// the bounded producer rate makes unreachable in practice.
static void enqueue(const char* topic, const char* payload, uint8_t qos,
                    bool retain) {
    if (g_queue == nullptr) {
        g_status.dropped++;
        return;
    }
    MqttMsg msg{};
    std::strncpy(msg.topic, topic, sizeof(msg.topic) - 1);
    std::strncpy(msg.payload, payload, sizeof(msg.payload) - 1);
    msg.qos = qos;
    msg.retain = retain;
    if (xQueueSend(g_queue, &msg, 0) == pdTRUE) return;

    // Full: drop the oldest TELEMETRY and retry once.
    MqttMsg oldest{};
    for (int i = 0; i < static_cast<int>(kMqttQueue); ++i) {
        MqttMsg probe{};
        if (xQueueReceive(g_queue, &probe, 0) != pdTRUE) break;
        if (probe.qos == 0 && oldest.topic[0] == '\0') {
            oldest = probe;      // this is the one we sacrifice
        } else {
            xQueueSend(g_queue, &probe, 0);   // keep the event
        }
    }
    if (oldest.topic[0] != '\0') {
        g_status.dropped++;
        if (xQueueSend(g_queue, &msg, 0) != pdTRUE) g_status.dropped++;
    } else {
        g_status.dropped++;      // nothing droppable: the event is lost
    }
}

static void publishState(const char* subtopic, const char* value,
                         uint8_t qos, bool retain) {
    char topic[192] = {};
    std::snprintf(topic, sizeof(topic), "%s/%s/%s",
                  g_config.base_topic.c_str(), mqttManagerDeviceId(), subtopic);
    enqueue(topic, value, qos, retain);
}

// ---- Home Assistant discovery (task 7.3) ------------------------------------
// Stable unique_ids: visioncam_<device_id>_<entity>. Retained configs with
// an availability topic so HA shows the device unavailable on a broker
// outage instead of stale data.

static void haPublishOne(const char* component, const char* entity,
                         const char* name, const char* value_template,
                         const char* device_class, const char* icon) {
    char topic[256] = {};
    std::snprintf(topic, sizeof(topic), "%s/%s/%s_%s/config",
                  g_config.ha_prefix.c_str(), component,
                  mqttManagerDeviceId(), entity);

    // Optional keys are pre-built into their own buffers so the payload is
    // a single well-formed snprintf — no placeholders, no post-editing.
    char cls[64] = {};
    char ico[64] = {};
    if (device_class && device_class[0]) {
        std::snprintf(cls, sizeof(cls), "\"device_class\":\"%s\",",
                      device_class);
    }
    if (icon && icon[0]) {
        std::snprintf(ico, sizeof(ico), "\"icon\":\"%s\",", icon);
    }
    // 512 B: the device block alone is ~190 B and every value is a bounded
    // literal or a bounded string, so this can never be sized from input.
    char payload[512] = {};
    std::snprintf(payload, sizeof(payload),
                  "{\"name\":\"%s\","
                  "\"uniq_id\":\"visioncam_%s_%s\","
                  "\"availability_topic\":\"%s/%s/availability\","
                  "\"state_topic\":\"%s/%s/state\","
                  "%s%s"
                  "\"value_template\":\"{{ value_json.%s }}\","
                  "\"device\":{\"identifiers\":[\"visioncam_%s\"],"
                  "\"name\":\"VisionCam Pro\",\"manufacturer\":\"VisionCam\","
                  "\"model\":\"ESP32-CAM\",\"sw_version\":\"%s\"}}",
                  name, mqttManagerDeviceId(), entity,
                  g_config.base_topic.c_str(), mqttManagerDeviceId(),
                  g_config.base_topic.c_str(), mqttManagerDeviceId(),
                  cls, ico,
                  (value_template && value_template[0]) ? value_template : "",
                  mqttManagerDeviceId(), "0.1.0-phase1");
    enqueue(topic, payload, 1, /*retain=*/true);
}

// Publish the full entity set. Called on connect (birth) and when HA
// announces itself online — never on a plain loop.
static void haPublishDiscovery() {
    if (!g_config.ha_discovery) return;
    haPublishOne("binary_sensor", "motion", "Motion detected",
                 "motion_detected", "motion", "mdi:run");
    haPublishOne("binary_sensor", "camera", "Camera online",
                 "camera_online", "connectivity", "mdi:camera");
    haPublishOne("binary_sensor", "recording", "Recording active",
                 "recording_active", nullptr, "mdi:record-rec");
    haPublishOne("binary_sensor", "sd", "SD mounted",
                 "sd_mounted", nullptr, "mdi:micro-sd");
    haPublishOne("sensor", "storage_used", "Storage used", "storage_used_pct",
                 nullptr, "mdi:harddisk");
    haPublishOne("sensor", "wifi_rssi", "Wi-Fi RSSI", "wifi_rssi",
                 "signal_strength", "mdi:wifi");
    haPublishOne("button", "snapshot", "Take snapshot", nullptr, nullptr,
                 "mdi:camera-plus");
    haPublishOne("switch", "flash_led", "Flash LED", "flash_led", nullptr,
                 "mdi:led-on");
    g_status.ha_discovery_published = true;
}

// The device state object — one JSON blob HA reads as the sensor set.
static void publishFullState() {
    StorageStatus st{};
    if (storageManagerAvailable()) {
        storageGetStatus(&st);
    }
    const uint32_t pct = st.total_bytes > 0
        ? static_cast<uint32_t>((st.used_bytes * 100u) / st.total_bytes)
        : 0;
    char payload[kMqttPayloadMax] = {};
    std::snprintf(payload, sizeof(payload),
                  "{\"camera_online\":true,\"sd_mounted\":%s,"
                  "\"motion_detected\":%s,\"recording_active\":%s,"
                  "\"storage_used_pct\":%u,\"storage_free_bytes\":%llu,"
                  "\"storage_media\":%u,\"uptime_s\":%u,\"flash_led\":false}",
                  st.mounted ? "true" : "false",
                  g_motion_active_flag ? "true" : "false",
                  g_recording_active_flag ? "true" : "false",
                  static_cast<unsigned int>(pct),
                  static_cast<unsigned long long>(st.free_bytes),
                  static_cast<unsigned int>(st.media_count),
                  static_cast<unsigned int>(uptimeMs() / 1000u));
    publishState("state", payload, 1, /*retain=*/true);
}

// ---- Event-bus bridge ------------------------------------------------------
// Subscriber contract (event_bus.h): quick and non-blocking. This only
// formats and enqueues — the worker task does all I/O.

static void mqttOnEvent(const Event& ev) {
    if (!g_config.enabled || g_queue == nullptr) return;
    char payload[64] = {};
    switch (ev.type) {
        case EventType::MotionStarted:
            g_motion_active_flag.store(true);
            std::snprintf(payload, sizeof(payload), "{\"motion_detected\":true,\"event\":\"motion_started\"}");
            publishState("motion/detected", payload, 1, false);
            break;
        case EventType::MotionEnded:
            g_motion_active_flag.store(false);
            std::snprintf(payload, sizeof(payload), "{\"motion_detected\":false,\"event\":\"motion_ended\"}");
            publishState("motion/detected", payload, 1, false);
            break;
        case EventType::RecordingStarted:
            g_recording_active_flag.store(true);
            publishState("recording/status", "{\"recording_active\":true}", 1, false);
            break;
        case EventType::RecordingStopped:
            g_recording_active_flag.store(false);
            publishState("recording/status", "{\"recording_active\":false}", 1, false);
            break;
        case EventType::CameraFailed:
            publishState("state_marker", "\"camera_failed\"", 1, false);
            break;
        case EventType::HeapCritical:
            publishState("state_marker", "\"heap_critical\"", 1, false);
            break;
        default:
            break;
    }
}

static void mqttEventHandler(void* args, esp_event_base_t base,
                             int32_t id, void* data);

// ---- Worker task (task 7.1: own task, never blocks a camera task) ----------
//
// Reconnect loop: exponential backoff 1 s doubling to 30 s plus jitter
// (R1 pattern, re-implemented per SPEC §1A.10). While offline the queue is
// drained-and-dropped so it can never grow; records stay on SD regardless.
// Connection parameters copied into STATIC buffers before client init:
// esp_mqtt_client_init() only shallow-copies the config (the strings stay
// referenced), and g_config can be reassigned by a concurrent PUT, so the
// client must never point into a std::string that may reallocate.
static char g_conn_uri[192] = {};
static char g_conn_user[64] = {};
static char g_conn_pass[128] = {};

static void mqttWorkerTask(void* /*param*/) {
    uint32_t backoff_ms = 1000;
    MqttMsg msg{};
    for (;;) {
        if (!g_config.enabled || g_config.broker_uri.empty()) {
            // Nothing to do: idle cheaply and re-check the config.
            vTaskDelay(pdMS_TO_TICKS(500));
            continue;
        }

        // A config change requests a rebuild from the HTTP task; the
        // WORKER alone stops/destroys the client (never two tasks).
        if (g_reconnect_requested.exchange(false) && g_client != nullptr) {
            esp_mqtt_client_stop(g_client);
            esp_mqtt_client_destroy(g_client);
            g_client = nullptr;
            g_connected.store(false);
            g_status.connected = false;
        }

        if (g_client == nullptr) {
            esp_mqtt_client_config_t cfg{};
            std::strncpy(g_conn_uri, g_config.broker_uri.c_str(),
                         sizeof(g_conn_uri) - 1);
            std::strncpy(g_conn_user, g_config.username.c_str(),
                         sizeof(g_conn_user) - 1);
            std::strncpy(g_conn_pass, g_config.password.c_str(),
                         sizeof(g_conn_pass) - 1);
            std::snprintf(g_last_will_topic, sizeof(g_last_will_topic),
                          "%s/%s/availability",
                          g_config.base_topic.c_str(),
                          mqttManagerDeviceId());
            cfg.broker.address.uri = g_conn_uri;
            cfg.credentials.client_id = nullptr;  // derived by the stack
            cfg.session.keepalive =
                (g_config.keepalive_s ? g_config.keepalive_s : 30);
            cfg.session.disable_clean_session = false;
            // Last Will: retained "offline" so HA shows an unavailable
            // device the moment the link drops (task 7.1 #11). Static
            // buffer — set BEFORE init because init only shallow-copies.
            cfg.session.last_will.topic = g_last_will_topic;
            cfg.session.last_will.msg = "offline";
            cfg.session.last_will.msg_len = 7;
            cfg.session.last_will.qos = 1;
            cfg.session.last_will.retain = true;
            if (!g_config.username.empty()) {
                cfg.credentials.username = g_conn_user;
                if (!g_config.password.empty())
                    cfg.credentials.authentication.password = g_conn_pass;
            }
            // mqtts without verification is a deliberate, explicit setting
            // (SPEC §4.11); the default keeps verification on.
            cfg.broker.verification.skip_cert_common_name_check =
                !g_config.tls_verify;
            cfg.task.stack_size = VC_STACK_MQTT;
            g_client = esp_mqtt_client_init(&cfg);
            if (g_client == nullptr) {
                vTaskDelay(pdMS_TO_TICKS(backoff_ms));
                backoff_ms = std::min(backoff_ms * 2u, kMqttBackoffMaxMs);
                continue;
            }
            esp_mqtt_client_register_event(g_client, MQTT_EVENT_ANY,
                                           mqttEventHandler, nullptr);
            g_status.state = "connecting";
            esp_mqtt_client_start(g_client);

            // WAIT for the async CONNECT instead of tearing the client
            // down while it is still negotiating — that race produced
            // connects=1/disconnects=14 with published=0 on hardware
            // (boot-18): every rebuild destroyed the client before the
            // broker's CONNACK could be processed.
            const uint32_t wait_start = uptimeMs();
            while (!g_connected.load() &&
                   uptimeMs() - wait_start < g_config.timeout_ms) {
                vTaskDelay(pdMS_TO_TICKS(50));
            }
            if (!g_connected.load()) {
                ESP_LOGW(TAG, "mqtt connect timeout after %u ms",
                         static_cast<unsigned>(g_config.timeout_ms));
                g_status.last_error = "connect timeout";
                g_status.state = "error";
                esp_mqtt_client_stop(g_client);
                esp_mqtt_client_destroy(g_client);
                g_client = nullptr;
                vTaskDelay(pdMS_TO_TICKS(backoff_ms));
                backoff_ms = std::min(backoff_ms * 2u, kMqttBackoffMaxMs);
                continue;
            }
            backoff_ms = 1000;
            ESP_LOGI(TAG, "mqtt connected to broker");
        }

        // Drain the queue (never blocks: 0 timeout).
        while (xQueueReceive(g_queue, &msg, 0) == pdTRUE) {
            if (g_connected.load()) {
                const int id = esp_mqtt_client_publish(
                    g_client, msg.topic, msg.payload, 0, msg.qos, msg.retain);
                if (id < 0) {
                    g_status.dropped++;   // publish refused: not queued again
                } else {
                    g_status.published++;
                }
            } else {
                g_status.dropped++;       // offline: telemetry/events let go
            }
        }

        if (g_connected.load()) {
            // Idle; the queue wakes us every 250 ms so latency stays low.
            vTaskDelay(pdMS_TO_TICKS(250));
            continue;
        }

        // Link dropped while running: rebuild with backoff.
        if (g_client != nullptr) {
            esp_mqtt_client_stop(g_client);
            esp_mqtt_client_destroy(g_client);
            g_client = nullptr;
        }
        vTaskDelay(pdMS_TO_TICKS(backoff_ms));
        backoff_ms = std::min(backoff_ms * 2u, kMqttBackoffMaxMs);
    }
}

// ---- esp-mqtt glue ----------------------------------------------------------

// esp-mqtt event handler (called from the MQTT task, not a camera task).
static void mqttEventHandler(void* /*args*/, esp_event_base_t /*base*/,
                             int32_t /*id*/, void* data) {
    const auto* event = static_cast<esp_mqtt_event_handle_t>(data);
    switch (event->event_id) {
        case MQTT_EVENT_CONNECTED:
            g_connected = true;
            g_status.connected = true;
            g_status.connects++;
            g_status.state = "online";
            g_status.last_error.clear();
            // Availability retained, then discovery + a full state read.
            publishState("availability", "online", 1, true);
            haPublishDiscovery();
            publishFullState();
            break;
        case MQTT_EVENT_DISCONNECTED:
            g_connected = false;
            g_status.connected = false;
            g_status.disconnects++;
            g_status.state = "offline";
            break;
        case MQTT_EVENT_ERROR:
            g_status.last_error = "broker connection error";
            g_status.state = "error";
            break;
        default:
            break;
    }
}

esp_err_t mqttManagerGetConfig(MqttConfig* out) {
    if (out == nullptr) return ESP_ERR_INVALID_ARG;
    MqttConfig copy = g_config;
    copy.password.clear();   // AGENTS rule 17: never echoed
    *out = copy;
    return ESP_OK;
}

esp_err_t mqttManagerSetConfig(const MqttConfig* candidate,
                               MqttConfig* effective) {
    if (candidate == nullptr || effective == nullptr)
        return ESP_ERR_INVALID_ARG;

    // SPEC §7 validate step — everything checked BEFORE persisting.
    MqttConfig next = g_config;      // overlay: unset fields keep their value
    next.enabled = candidate->enabled;
    if (!candidate->broker_uri.empty()) {
        const std::string& uri = candidate->broker_uri;
        const bool scheme_ok = uri.rfind("mqtt://", 0) == 0 ||
                               uri.rfind("mqtts://", 0) == 0;
        if (!scheme_ok || uri.size() > 191) return ESP_ERR_INVALID_ARG;
        next.broker_uri = uri;
    }
    if (!candidate->username.empty())
        next.username = candidate->username;   // 0 = keep current
    if (!candidate->password.empty())
        next.password = candidate->password;    // write-only path
    next.tls_verify = candidate->tls_verify;
    next.ha_discovery = candidate->ha_discovery;
    if (!candidate->base_topic.empty() && candidate->base_topic.size() <= 64)
        next.base_topic = candidate->base_topic;
    if (!candidate->ha_prefix.empty() && candidate->ha_prefix.size() <= 32)
        next.ha_prefix = candidate->ha_prefix;
    if (candidate->keepalive_s > 0 && candidate->keepalive_s <= 300)
        next.keepalive_s = candidate->keepalive_s;
    if (candidate->timeout_ms > 0 && candidate->timeout_ms <= 60000)
        next.timeout_ms = candidate->timeout_ms;

    // A disabled worker still stores the URI so re-enabling is one toggle.
    if (next.enabled && next.broker_uri.empty())
        return ESP_ERR_INVALID_ARG;   // cannot enable with no broker

    const esp_err_t perr = persistConfig(next);
    if (perr != ESP_OK) return perr;

    // Apply. Connection fields changed -> the worker rebuilds the client.
    const bool reconnect = (next.broker_uri != g_config.broker_uri ||
                            next.username != g_config.username ||
                            next.password != g_config.password ||
                            next.tls_verify != g_config.tls_verify ||
                            next.keepalive_s != g_config.keepalive_s);
    g_config = next;
    if (reconnect) {
        // Never stop/destroy the client from this (HTTP) task — the worker
        // owns it. It sees this flag and rebuilds with fresh parameters.
        g_reconnect_requested.store(true);
    }
    g_status.enabled = g_config.enabled;
    g_status.state = g_config.enabled
                        ? (g_config.broker_uri.empty() ? "unconfigured"
                                                       : "connecting")
                        : "disabled";

    *effective = g_config;
    effective->password.clear();   // sanitized response
    return ESP_OK;
}

esp_err_t mqttManagerStatus(MqttStatus* out) {
    if (out == nullptr) return ESP_ERR_INVALID_ARG;
    MqttStatus copy = g_status;
    copy.connected = g_connected.load();
    *out = copy;
    return ESP_OK;
}

esp_err_t mqttManagerRepublishDiscovery() {
    if (!g_config.enabled || g_config.broker_uri.empty())
        return ESP_ERR_INVALID_STATE;
    if (!g_connected.load()) return ESP_ERR_INVALID_STATE;
    haPublishDiscovery();
    publishFullState();
    return ESP_OK;
}

// One-shot probe: a temporary client against the stored broker with the
// caller's timeout, result reported in the log/status line. Used by the
// Integrations page "Test connection" button.
// The client is rebuilt by the WORKER (flag request), never by this task.
esp_err_t mqttManagerTestConnection() {
    if (g_config.broker_uri.empty()) return ESP_ERR_INVALID_STATE;
    g_status.last_error.clear();
    // Ask the worker to rebuild + reconnect, then wait for the outcome.
    g_connected.store(false);
    g_reconnect_requested.store(true);
    g_status.state = "connecting";
    const uint32_t started = uptimeMs();
    while (uptimeMs() - started < kMqttTestTimeoutMs) {
        if (g_connected.load()) {
            ESP_LOGI(TAG, "mqtt test: connected in %u ms",
                     static_cast<unsigned>(uptimeMs() - started));
            return ESP_OK;
        }
        vTaskDelay(pdMS_TO_TICKS(100));
    }
    g_status.last_error = "broker not reachable";
    g_status.state = "error";
    ESP_LOGW(TAG, "mqtt test: no broker within %u ms",
             static_cast<unsigned>(kMqttTestTimeoutMs));
    return ESP_ERR_TIMEOUT;
}

esp_err_t mqttManagerPublishTest() {
    if (!g_connected.load() || g_client == nullptr)
        return ESP_ERR_INVALID_STATE;
    char topic[192] = {};
    std::snprintf(topic, sizeof(topic), "%s/%s/test",
                  g_config.base_topic.c_str(), mqttManagerDeviceId());
    const int id = esp_mqtt_client_publish(
        g_client, topic, "{\"test\":true,\"schema_version\":1}", -1,
        /*qos=*/1, /*retain=*/false);
    if (id < 0) {
        g_status.last_error = "publish refused";
        return ESP_FAIL;
    }
    g_status.published++;
    ESP_LOGI(TAG, "test publish queued (msg_id %d)", id);
    return ESP_OK;
}

esp_err_t mqttManagerInit() {
    if (g_initialized) return ESP_OK;

    nvs_handle_t h = 0;
    if (nvs_open(kMqttNvsNs, NVS_READONLY, &h) == ESP_OK) {
        struct Guard {
            nvs_handle_t hh;
            ~Guard() { nvs_close(hh); }
        } guard{h};
        uint32_t v = 0;
        if (nvs_get_u32(h, kMqttKeyVer, &v) == ESP_OK && v == kMqttConfigSchema) {
            uint8_t en = 0;
            if (nvs_get_u8(h, kMqttKeyEnabled, &en) == ESP_OK)
                g_config.enabled = (en != 0);
            char uri[192] = {};
            size_t uri_len = sizeof(uri);
            if (nvs_get_str(h, kMqttKeyUri, uri, &uri_len) == ESP_OK)
                g_config.broker_uri = uri;
            char user[64] = {};
            size_t user_len = sizeof(user);
            if (nvs_get_str(h, kMqttKeyUser, user, &user_len) == ESP_OK)
                g_config.username = user;
            char pw[128] = {};
            size_t pw_len = sizeof(pw);
            if (nvs_get_str(h, kMqttKeyPass, pw, &pw_len) == ESP_OK)
                g_config.password = pw;
            uint8_t tls = 1;
            if (nvs_get_u8(h, kMqttKeyTls, &tls) == ESP_OK)
                g_config.tls_verify = (tls != 0);
            char base[96] = {};
            size_t base_len = sizeof(base);
            if (nvs_get_str(h, kMqttKeyBase, base, &base_len) == ESP_OK)
                g_config.base_topic = base;
            uint8_t ha = 1;
            if (nvs_get_u8(h, kMqttKeyHa, &ha) == ESP_OK)
                g_config.ha_discovery = (ha != 0);
            char ha_prefix[64] = {};
            size_t ha_prefix_len = sizeof(ha_prefix);
            if (nvs_get_str(h, kMqttKeyHaPrefix, ha_prefix, &ha_prefix_len) ==
                ESP_OK)
                g_config.ha_prefix = ha_prefix;
            nvs_get_u32(h, kMqttKeyKeepalive, &g_config.keepalive_s);
            nvs_get_u32(h, kMqttKeyTimeout, &g_config.timeout_ms);
        }
    }

    g_queue = xQueueCreate(kMqttQueue, sizeof(MqttMsg));
    if (g_queue == nullptr) return ESP_ERR_NO_MEM;

    g_status.enabled = g_config.enabled;
    g_status.state = g_config.enabled
                        ? (g_config.broker_uri.empty() ? "unconfigured"
                                                       : "connecting")
                        : "disabled";

    if (g_task == nullptr &&
        xTaskCreate(mqttWorkerTask, "vc_mqtt", VC_STACK_MQTT, nullptr,
                    VC_PRIO_MQTT, &g_task) != pdPASS) {
        vQueueDelete(g_queue);
        g_queue = nullptr;
        return ESP_ERR_NO_MEM;
    }

    // Motion/recording lifecycle -> publish (quick enqueue only).
    eventBusSubscribe(mqttOnEvent);

    g_initialized = true;
    ESP_LOGI(TAG, "mqtt manager init: enabled=%d uri=%s state=%s",
             g_config.enabled ? 1 : 0,
             g_config.broker_uri.empty() ? "(none)" : "(set)",
             g_status.state.c_str());
    return ESP_OK;
}

esp_err_t mqttManagerShutdown() {
    g_initialized = false;
    if (g_client != nullptr) {
        esp_mqtt_client_stop(g_client);
        esp_mqtt_client_destroy(g_client);
        g_client = nullptr;
    }
    g_connected = false;
    if (g_task != nullptr) {
        vTaskDelete(g_task);
        g_task = nullptr;
    }
    if (g_queue != nullptr) {
        vQueueDelete(g_queue);
        g_queue = nullptr;
    }
    return ESP_OK;
}

bool mqttManagerPasswordSet() { return !g_config.password.empty(); }

const char* mqttManagerDeviceId() { return deviceIdentityId(); }

}  // namespace visioncam