// ai_manager.cpp — AI inference manager (Phase 6; SPEC §4.4).
//
// Orchestrates the AI provider, runs inference on selected frames (after
// motion/event per R1's usage pattern), and publishes ObjectDetected events
// on the typed event bus. Runs at LOW priority so streaming/storage always
// win.
//
// Providers:
//   NoAiProvider — default, AI fully disabled
//   ExternalAiProvider — primary path for original ESP32 (HTTP POST /api/v1/infer)
//   TfliteMicroProvider — experimental on-device (owner decision D4)

#include "ai_manager.h"

#include <algorithm>
#include <cstring>
#include <ctime>

#include "esp_heap_caps.h"
#include "esp_http_client.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"
#include "freertos/task.h"
#include "nvs_flash.h"
#include "nvs.h"

#include "event_bus.h"
#include "external_ai_provider.h"
#include "frame_manager.h"
#include "storage_manager.h"
#include "tasks.h"
#include "tflite_micro_provider.h"
#include "wall_clock.h"

namespace visioncam {

static const char* TAG = "vc_ai";

// ---- NVS namespace for AI config (SPEC 7) --------------------------------
constexpr char kAINvsNs[] = "ai";
constexpr char kAIKeyVer[] = "ai_ver";
constexpr char kAIKeyEnabled[] = "ai_enabled";
constexpr char kAIKeyProvider[] = "ai_provider";
constexpr char kAIKeyBaseURL[] = "ai_base_url";
constexpr char kAIKeyTimeout[] = "ai_timeout";
constexpr char kAIKeyInterval[] = "ai_interval";
constexpr char kAIKeyActiveModel[] = "ai_active_model";
constexpr char kAIKeyToken[] = "ai_token";       // device-local secret; never in a GET response
constexpr char kAIKeyConf[] = "ai_conf";         // confidence x1000 (NVS has no float)
constexpr uint32_t kAIConfigSchema = 1;

// Persist the whole config as explicit versioned keys (SPEC §7). Secrets
// live in NVS only — never logged, never echoed by GET /ai/config.
static esp_err_t persistConfig(const AIConfig& cfg) {
    nvs_handle_t h = 0;
    esp_err_t err = nvs_open(kAINvsNs, NVS_READWRITE, &h);
    if (err != ESP_OK) return err;
    nvs_set_u32(h, kAIKeyVer, kAIConfigSchema);
    nvs_set_u8(h, kAIKeyEnabled, cfg.enabled ? 1 : 0);
    nvs_set_str(h, kAIKeyProvider, cfg.provider.c_str());
    nvs_set_u32(h, kAIKeyTimeout, cfg.timeout_ms);
    nvs_set_u32(h, kAIKeyInterval, cfg.inference_interval_ms);
    nvs_set_str(h, kAIKeyBaseURL, cfg.base_url.c_str());
    nvs_set_str(h, kAIKeyActiveModel, cfg.active_model_id.c_str());
    nvs_set_str(h, kAIKeyToken, cfg.auth_token.c_str());
    float ct = cfg.confidence_threshold;
    if (!(ct >= 0.0f)) ct = 0.0f;   // also catches NaN
    if (ct > 1.0f) ct = 1.0f;
    nvs_set_u32(h, kAIKeyConf, static_cast<uint32_t>(ct * 1000.0f + 0.5f));
    err = nvs_commit(h);
    nvs_close(h);
    return err;
}

// ---- NoAiProvider — default, AI fully disabled -----------------------------

class NoAiProvider : public IInferenceProvider {
public:
    esp_err_t begin() override { return ESP_OK; }
    bool isReady() const override { return true; }
    esp_err_t infer(const FrameView& /*frame*/,
                    InferenceResult* out) override {
        out->detections.clear();
        return ESP_OK;
    }
    void stop() override {}
    const char* providerId() const override { return kProviderNone; }
};

// ---- Module state ---------------------------------------------------------
static AIConfig g_config{};
static AIStatus g_status{};
static bool g_initialized = false;

// Active provider:
static IInferenceProvider* g_provider = nullptr;
static NoAiProvider* g_no_ai = nullptr;
static ExternalAiProvider* g_external_ai = nullptr;
static TfliteMicroProvider* g_tflite = nullptr;

// Bounded detection event cache:
static AIDetectionEvent g_detections[kAIDetectionMaxEvents] = {};
static uint32_t g_detection_count = 0;
static uint32_t g_next_event_id = 1;

// Inference rate limiting:
static uint32_t g_next_inference_ms = 0;

// Motion-triggered inference (owner decision D6): the event-bus handler
// below only gives a task notification — quick and non-blocking per
// event_bus.h — and the AI task does all the work.
static TaskHandle_t g_ai_task_handle = nullptr;
static FrameSubscriberId g_ai_sub = 0;
static bool g_ai_frame_ok = false;          // frame subscription active
static uint8_t* g_ai_frame_buf = nullptr;   // per-consumer copy buffer
// Bound for one frame accepted for inference (AGENTS rule 13: a fixed
// ceiling, never sized from user input or the frame itself).
static constexpr uint32_t kAIFrameMaxBytes = 256 * 1024;

// ---- Helpers -------------------------------------------------------------

static uint32_t wallClockNow() {
    return static_cast<uint32_t>(wallClockSeconds());
}

static esp_err_t allocateEventId(char* out_id, size_t out_cap) {
    if (out_id == nullptr || out_cap < 24) return ESP_ERR_INVALID_ARG;
    if (g_next_event_id >= kAIDetectionMaxEvents) return ESP_ERR_NO_MEM;
    int n = std::snprintf(out_id, out_cap, "evt-%08u",
                          static_cast<unsigned int>(g_next_event_id++));
    return (n > 0 && static_cast<size_t>(n) < out_cap) ? ESP_OK
                                                       : ESP_ERR_INVALID_SIZE;
}

// Publish an ObjectDetected event on the event bus.
static void publishObjectDetected(const AIDetectionEvent& det) {
    eventBusPublish(EventType::ObjectDetected,
                    static_cast<uint16_t>(det.confidence * 100), 0);
}

// ---- AI task (motion-triggered, D6) ---------------------------------------

// Event-bus subscriber: motion start/end wakes the AI task. CONTRACT:
// quick, non-blocking, never publishes (event_bus.h).
static void aiOnMotionEvent(const Event& ev) {
    if (ev.type == EventType::MotionStarted ||
        ev.type == EventType::MotionEnded) {
        if (g_ai_task_handle != nullptr) xTaskNotifyGive(g_ai_task_handle);
    }
}

static void aiTask(void* /*param*/) {
    FrameLease lease{};
    while (true) {
        // Block until motion wakes us, or tick to re-check configuration.
        ulTaskNotifyTake(pdTRUE, pdMS_TO_TICKS(500));

        // Gates: framework on, a real provider selected (D6: "none" idles
        // cheaply — no frame is taken for an unconfigured provider), and
        // the provider actually ready.
        if (!g_initialized || !g_config.enabled ||
            g_provider == nullptr || !g_ai_frame_ok) {
            continue;
        }
        if (g_config.provider == kProviderNone) continue;
        if (!g_provider->isReady()) continue;

        // Rate limit: at most one inference per inference_interval_ms
        // (also the floor for a continuous motion burst).
        const uint32_t now_ms =
            static_cast<uint32_t>(esp_timer_get_time() / 1000);
        if (now_ms < g_next_inference_ms) continue;
        g_next_inference_ms = now_ms + g_config.inference_interval_ms;

        // Take a frame, COPY it into the AI-owned buffer and release the
        // lease immediately — the provider then does blocking HTTP/SD work
        // on the copy (SPEC 4.1.1 rule 9: never hold a lease across I/O).
        if (!frameAcquire(g_ai_sub, lease, 0)) continue;   // no frame yet
        const uint32_t len = lease.len;
        const uint16_t w = lease.width;
        const uint16_t h = lease.height;
        const uint64_t ts = lease.ts_us;

        if (g_ai_frame_buf == nullptr) {
            g_ai_frame_buf = static_cast<uint8_t*>(
                heap_caps_malloc(kAIFrameMaxBytes, MALLOC_CAP_DEFAULT));
            if (g_ai_frame_buf == nullptr) {
                frameRelease(lease);
                g_status.last_error = "frame buffer alloc failed";
                ESP_LOGE(TAG, "AI frame buffer alloc failed (%u B)",
                         static_cast<unsigned>(kAIFrameMaxBytes));
                continue;
            }
        }
        const esp_err_t cerr = frameLeaseCopyTo(
            lease, g_ai_frame_buf, kAIFrameMaxBytes);
        frameRelease(lease);
        if (cerr != ESP_OK) {
            // Frame larger than the bound: counted, never truncated.
            g_status.last_error = "frame too large for AI buffer";
            continue;
        }

        // Inference on our own copy — no lease is held.
        FrameView view{};
        view.data = g_ai_frame_buf;
        view.len = len;
        view.width = w;
        view.height = h;
        view.ts_us = ts;
        aiManagerInferFrame(view);
    }
}

// ---- Public API ----------------------------------------------------------

esp_err_t aiManagerInit() {
    // Load config from NVS (SPEC §7: versioned schema, explicit keys).
    // A missing namespace, version key or schema mismatch means "fresh
    // device": keep the D6 defaults (enabled=true, provider="none") and
    // persist them so boot 1 writes a stable baseline.
    bool loaded = false;
    nvs_handle_t h = 0;
    if (nvs_open(kAINvsNs, NVS_READONLY, &h) == ESP_OK) {
        struct Guard {
            nvs_handle_t hh;
            ~Guard() { nvs_close(hh); }
        } guard{h};
        uint32_t v = 0;
        if (nvs_get_u32(h, kAIKeyVer, &v) == ESP_OK &&
            v == kAIConfigSchema) {
            loaded = true;
            uint8_t en = 0;
            if (nvs_get_u8(h, kAIKeyEnabled, &en) == ESP_OK)
                g_config.enabled = (en != 0);
            char provider[32] = {};
            size_t provider_len = sizeof(provider);
            if (nvs_get_str(h, kAIKeyProvider, provider, &provider_len) ==
                ESP_OK)
                g_config.provider = provider;
            nvs_get_u32(h, kAIKeyTimeout, &g_config.timeout_ms);
            nvs_get_u32(h, kAIKeyInterval, &g_config.inference_interval_ms);
            char base_url[256] = {};
            size_t base_url_len = sizeof(base_url);
            if (nvs_get_str(h, kAIKeyBaseURL, base_url, &base_url_len) ==
                ESP_OK)
                g_config.base_url = base_url;
            char token[128] = {};
            size_t token_len = sizeof(token);
            if (nvs_get_str(h, kAIKeyToken, token, &token_len) == ESP_OK)
                g_config.auth_token = token;
            char model[64] = {};
            size_t model_len = sizeof(model);
            if (nvs_get_str(h, kAIKeyActiveModel, model, &model_len) == ESP_OK)
                g_config.active_model_id = model;
            uint32_t conf_x1000 = 0;
            if (nvs_get_u32(h, kAIKeyConf, &conf_x1000) == ESP_OK &&
                conf_x1000 <= 1000)
                g_config.confidence_threshold =
                    static_cast<float>(conf_x1000) / 1000.0f;
        }
    }
    if (!loaded) {
        const esp_err_t perr = persistConfig(g_config);
        if (perr == ESP_OK) {
            ESP_LOGI(TAG,
                     "AI defaults persisted (D6: enabled=1 provider=none)");
        } else {
            ESP_LOGW(TAG, "AI default persist failed: %s — will retry on "
                     "first config change", esp_err_to_name(perr));
        }
    }

    // Create NoAiProvider (always available):
    g_no_ai = new NoAiProvider();
    if (g_no_ai == nullptr) return ESP_ERR_NO_MEM;
    esp_err_t err = g_no_ai->begin();
    if (err != ESP_OK) {
        delete g_no_ai;
        g_no_ai = nullptr;
        return err;
    }

    // Create ExternalAiProvider and apply any stored connection settings
    // BEFORE begin() — otherwise a configured endpoint would never become
    // ready after a reboot (base_url_ would still be empty).
    g_external_ai = new ExternalAiProvider();
    if (g_external_ai == nullptr) return ESP_ERR_NO_MEM;
    g_external_ai->setConfig(g_config);
    err = g_external_ai->begin();
    if (err != ESP_OK) {
        // External AI may fail if no base URL configured yet — that's OK,
        // it just won't be ready until configured (reported by /ai/status).
    }

    // Create TfliteMicroProvider (selectable per D4, but the runtime is
    // not linked: begin()/infer() return ESP_ERR_NOT_SUPPORTED and the
    // status carries the honest "unavailable" message):
    g_tflite = new TfliteMicroProvider();
    if (g_tflite == nullptr) return ESP_ERR_NO_MEM;
    g_tflite->setConfig(g_config);

    // Select the stored provider (sets g_config.provider + status):
    err = aiManagerSelectProvider(g_config.provider.c_str());
    if (err != ESP_OK) {
        // Corrupt/unknown stored ID: fall back to none and fix NVS.
        ESP_LOGW(TAG, "stored provider '%s' invalid — falling back to none",
                 g_config.provider.c_str());
        g_provider = g_no_ai;
        g_config.provider = kProviderNone;
        persistConfig(g_config);
    }

    // Frame subscription for motion-triggered inference. Tolerated: with
    // no camera the AI subsystem still boots and serves /ai/* (D6) — it
    // simply never receives a frame.
    err = frameManagerSubscribe(&g_ai_sub, "ai");
    if (err == ESP_OK) {
        g_ai_frame_ok = true;
    } else {
        g_ai_frame_ok = false;
        ESP_LOGW(TAG, "AI frame subscription unavailable: %s — "
                 "inference idle until the camera runs",
                 esp_err_to_name(err));
    }

    // Motion wake-up (D6: inference runs after motion, R1's pattern).
    err = eventBusSubscribe(aiOnMotionEvent);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "AI event subscription failed: %s — motion will not "
                 "trigger inference", esp_err_to_name(err));
    }

    g_initialized = true;

    // Create AI task (lowest priority — storage/streaming always win):
    if (xTaskCreate(aiTask, "vc_ai", VC_STACK_AI, nullptr,
                    VC_PRIO_AI, &g_ai_task_handle) != pdPASS) {
        g_initialized = false;
        return ESP_ERR_NO_MEM;
    }

    g_status.enabled = g_config.enabled;
    g_status.provider = g_config.provider;
    g_status.provider_ready = (g_provider != nullptr &&
                               g_provider->isReady());
    ESP_LOGI(TAG, "AI manager init: provider=%s enabled=%d ready=%d",
             g_config.provider.c_str(), g_config.enabled ? 1 : 0,
             g_status.provider_ready ? 1 : 0);
    return ESP_OK;
}

esp_err_t aiManagerShutdown() {
    g_initialized = false;
    g_config.enabled = false;
    if (g_ai_frame_ok) {
        frameManagerUnsubscribe(g_ai_sub);
        g_ai_frame_ok = false;
    }
    g_ai_task_handle = nullptr;
    if (g_ai_frame_buf != nullptr) {
        heap_caps_free(g_ai_frame_buf);
        g_ai_frame_buf = nullptr;
    }
    if (g_provider != nullptr) {
        g_provider->stop();
        g_provider = nullptr;
    }
    if (g_no_ai != nullptr) {
        delete g_no_ai;
        g_no_ai = nullptr;
    }
    if (g_external_ai != nullptr) {
        delete g_external_ai;
        g_external_ai = nullptr;
    }
    if (g_tflite != nullptr) {
        delete g_tflite;
        g_tflite = nullptr;
    }
    return ESP_OK;
}

esp_err_t aiManagerGetConfig(AIConfig* out) {
    if (out == nullptr) return ESP_ERR_INVALID_ARG;
    *out = g_config;
    return ESP_OK;
}

esp_err_t aiManagerSetConfig(const AIConfig* candidate,
                             AIConfig* effective) {
    if (candidate == nullptr || effective == nullptr)
        return ESP_ERR_INVALID_ARG;

    // SPEC §7 update process: validate EVERYTHING before persisting.
    // The caller overlays the candidate on the current config, so each
    // field already carries the current value when the client omitted it.
    if (candidate->timeout_ms > 60000) return ESP_ERR_INVALID_ARG;
    if (candidate->inference_interval_ms > 3600000)
        return ESP_ERR_INVALID_ARG;
    if (!(candidate->confidence_threshold >= 0.0f &&
          candidate->confidence_threshold <= 1.0f))
        return ESP_ERR_INVALID_ARG;   // also rejects NaN
    if (candidate->upload_quality > 63) return ESP_ERR_INVALID_ARG;
    if (candidate->provider != kProviderNone &&
        candidate->provider != kProviderExternal &&
        candidate->provider != kProviderTflite)
        return ESP_ERR_INVALID_ARG;

    AIConfig new_cfg = g_config;
    new_cfg.enabled = candidate->enabled;              // D6: user toggle
    new_cfg.provider = candidate->provider;
    new_cfg.confidence_threshold = candidate->confidence_threshold;
    if (candidate->timeout_ms > 0)
        new_cfg.timeout_ms = candidate->timeout_ms;    // 0 = keep current
    if (candidate->inference_interval_ms > 0)
        new_cfg.inference_interval_ms =
            candidate->inference_interval_ms;          // 0 = keep current
    if (!candidate->base_url.empty())
        new_cfg.base_url = candidate->base_url;
    if (!candidate->auth_token.empty())
        new_cfg.auth_token = candidate->auth_token;
    if (candidate->upload_width > 0)
        new_cfg.upload_width = candidate->upload_width;
    if (candidate->upload_height > 0)
        new_cfg.upload_height = candidate->upload_height;
    if (candidate->upload_quality > 0)
        new_cfg.upload_quality = candidate->upload_quality;
    if (!candidate->active_model_id.empty())
        new_cfg.active_model_id = candidate->active_model_id;

    // Persist first — a failed write must not leave RAM ahead of NVS.
    const esp_err_t perr = persistConfig(new_cfg);
    if (perr != ESP_OK) return perr;

    // Apply. NOTE: capture the previous provider BEFORE the assignment —
    // comparing g_config.provider against new_cfg after copying is always
    // false and silently disabled provider switching (fixed 2026-11-20).
    const std::string prev_provider = g_config.provider;
    const bool conn_changed =
        (new_cfg.base_url != g_config.base_url ||
         new_cfg.auth_token != g_config.auth_token ||
         new_cfg.timeout_ms != g_config.timeout_ms);
    g_config = new_cfg;

    if (g_config.provider != prev_provider) {
        const esp_err_t serr =
            aiManagerSelectProvider(g_config.provider.c_str());
        if (serr != ESP_OK) {
            g_provider = g_no_ai;
            g_config.provider = kProviderNone;
            persistConfig(g_config);   // keep NVS honest after fallback
        }
    } else if (g_config.provider == kProviderExternal &&
               conn_changed && g_external_ai != nullptr) {
        // Endpoint/token/timeout changed while external is selected:
        // apply live without a reboot (D6 — "configure and use").
        g_external_ai->setConfig(g_config);
        g_external_ai->begin();   // fails only when the URL is empty
    }

    g_status.enabled = g_config.enabled;
    g_status.provider = g_config.provider;
    g_status.provider_ready = (g_provider != nullptr &&
                               g_provider->isReady());
    *effective = g_config;
    return ESP_OK;
}

esp_err_t aiManagerStatus(AIStatus* out) {
    if (out == nullptr) return ESP_ERR_INVALID_ARG;
    *out = g_status;
    return ESP_OK;
}

esp_err_t aiManagerListProviders(char* out_ids, size_t out_cap,
                                 size_t* out_count) {
    if (out_ids == nullptr || out_count == nullptr)
        return ESP_ERR_INVALID_ARG;

    // Serialised as a JSON array — the buffer is interpolated directly into
    // the /ai/providers response. A NUL-separated list (the first attempt,
    // 2026-11-20) produced {"providers":none,...} — invalid JSON.
    const char* providers[] = {kProviderNone, kProviderExternal,
                               kProviderTflite};
    size_t n = 0;
    int written = std::snprintf(out_ids, out_cap, "[");
    if (written < 0 || static_cast<size_t>(written) >= out_cap)
        return ESP_ERR_NO_MEM;
    n = static_cast<size_t>(written);
    for (size_t i = 0; i < sizeof(providers) / sizeof(providers[0]); i++) {
        const char* sep = (i > 0) ? ",\"" : "\"";
        written = std::snprintf(out_ids + n, out_cap - n, "%s%s\"",
                                sep, providers[i]);
        if (written < 0 || n + static_cast<size_t>(written) >= out_cap)
            return ESP_ERR_NO_MEM;
        n += static_cast<size_t>(written);
    }
    written = std::snprintf(out_ids + n, out_cap - n, "]");
    if (written < 0 || n + static_cast<size_t>(written) >= out_cap)
        return ESP_ERR_NO_MEM;
    n += static_cast<size_t>(written);
    *out_count = 3;
    return ESP_OK;
}

esp_err_t aiManagerGetModelManifest(ModelManifest* /*out*/) {
    // Manifest loading from /models/staging/ — not yet implemented
    return ESP_ERR_NOT_SUPPORTED;
}

// ---- Model listing (GET /ai/models) ---------------------------------------
// Scans /sdcard/models/{staging,active,backup} through the storage manager
// (AGENTS rule 10 — no opendir/fopen in this file). Bounded: at most cap
// rows in total; *out_has_more reports a truncated scan. A missing SD card
// yields count 0, not an error — an empty list is the honest answer.
esp_err_t aiManagerListModels(AIModelInfo* out, size_t cap,
                              size_t* out_count, bool* out_has_more) {
    if (out == nullptr || out_count == nullptr || out_has_more == nullptr)
        return ESP_ERR_INVALID_ARG;
    *out_count = 0;
    *out_has_more = false;

    static const struct { const char* dir; const char* state; } kDirs[] = {
        {"/sdcard/models/staging", "staged"},
        {"/sdcard/models/active",  "active"},
        {"/sdcard/models/backup",  "backup"},
    };
    static constexpr char kSuffix[] = ".tflite";
    constexpr size_t kSuffixLen = sizeof(kSuffix) - 1;
    DirEntry entries[8];   // bounded per-directory batch
    for (const auto& d : kDirs) {
        size_t n = 0;
        const esp_err_t err =
            storageListDir(d.dir, entries,
                           sizeof(entries) / sizeof(entries[0]), &n);
        if (err != ESP_OK) break;   // no SD / not mounted
        for (size_t i = 0; i < n; i++) {
            const DirEntry& e = entries[i];
            if (e.is_dir) continue;
            const size_t name_len = std::strlen(e.name);
            if (name_len <= kSuffixLen ||
                std::strcmp(e.name + name_len - kSuffixLen, kSuffix) != 0) {
                continue;   // skips "*.tflite.part" and foreign files
            }
            if (*out_count >= cap) {
                *out_has_more = true;
                return ESP_OK;
            }
            const size_t id_len = name_len - kSuffixLen;
            if (id_len >= sizeof(out[0].model_id)) continue;
            AIModelInfo& m = out[*out_count];
            std::memcpy(m.model_id, e.name, id_len);
            m.model_id[id_len] = '\0';
            std::strncpy(m.state, d.state, sizeof(m.state) - 1);
            m.state[sizeof(m.state) - 1] = '\0';
            m.size_bytes = e.size;
            m.mtime = e.mtime;
            (*out_count)++;
        }
    }
    return ESP_OK;
}

esp_err_t aiManagerUploadModel(const char* model_id,
                               const uint8_t* data, size_t len) {
    if (g_tflite == nullptr) return ESP_ERR_INVALID_STATE;
    return g_tflite->modelUploadStage(model_id, data, len);
}

esp_err_t aiManagerValidateModel(const char* model_id) {
    if (g_tflite == nullptr) return ESP_ERR_INVALID_STATE;
    ModelManifest manifest{};
    esp_err_t err = g_tflite->modelValidate(model_id, &manifest);
    if (err != ESP_OK) return err;
    // Manifest details could be returned via an out parameter, but for now
    // we just validate:
    return ESP_OK;
}

esp_err_t aiManagerActivateModel(const char* model_id) {
    if (g_tflite == nullptr) return ESP_ERR_INVALID_STATE;
    return g_tflite->modelActivate(model_id);
}

esp_err_t aiManagerDeactivateAI() {
    // Owner decision D6: deactivation selects provider "none" — the AI
    // framework stays ENABLED and the choice persists across reboots.
    // "none" is "AI on, nothing selected", not "AI off"; the enabled flag
    // itself is toggled through PUT /ai/config {"enabled": false}.
    const esp_err_t serr = aiManagerSelectProvider(kProviderNone);
    if (serr != ESP_OK) return serr;
    return persistConfig(g_config);
}

esp_err_t aiManagerRollbackModel() {
    if (g_tflite == nullptr) return ESP_ERR_INVALID_STATE;
    // Rollback applies to the currently active model; find it from config:
    if (g_config.active_model_id.empty()) return ESP_ERR_INVALID_STATE;
    return g_tflite->modelRollback(g_config.active_model_id.c_str());
}

esp_err_t aiManagerListEvents(uint32_t offset, uint32_t limit,
                              AIDetectionEvent* out, size_t out_cap,
                              size_t* out_count, bool* out_has_more) {
    if (out == nullptr || out_count == nullptr || out_has_more == nullptr)
        return ESP_ERR_INVALID_ARG;

    *out_count = 0;
    *out_has_more = false;
    uint32_t total = 0;

    for (uint32_t i = 0; i < g_detection_count; i++) {
        total++;
        if (total <= offset) continue;
        if (*out_count >= limit) {
            *out_has_more = true;
            break;
        }
        if (*out_count >= out_cap) break;
        out[*out_count] = g_detections[i];
        (*out_count)++;
    }
    return ESP_OK;
}

esp_err_t aiManagerInferFrame(const FrameView& frame) {
    if (g_provider == nullptr || !g_provider->isReady())
        return ESP_ERR_INVALID_STATE;

    InferenceResult result{};
    result.model_id = g_config.active_model_id;
    esp_err_t err = g_provider->infer(frame, &result);
    if (err != ESP_OK) {
        g_status.inferences_failed++;
        g_status.last_error = "inference failed";
        return err;
    }

    g_status.inferences_total++;
    g_status.inferences_ok++;
    g_status.last_inference_duration_ms = result.inference_ms;

    // Record + publish detections that clear the confidence threshold
    // (SPEC §7 ai.confidence_threshold; sub-threshold results are
    // counted nowhere — they are not events).
    for (const auto& det : result.detections) {
        if (det.confidence < g_config.confidence_threshold) continue;
        AIDetectionEvent ev{};
        esp_err_t iderr = allocateEventId(ev.event_id, sizeof(ev.event_id));
        if (iderr != ESP_OK) break;
        std::strncpy(ev.source, g_config.provider.c_str(),
                     sizeof(ev.source) - 1);
        ev.class_id = det.class_id;
        ev.label = det.label;
        ev.confidence = det.confidence;
        ev.bbox[0] = det.bbox[0];
        ev.bbox[1] = det.bbox[1];
        ev.bbox[2] = det.bbox[2];
        ev.bbox[3] = det.bbox[3];
        ev.timestamp = wallClockNow();
        std::strncpy(ev.notification_status, "queued",
                     sizeof(ev.notification_status) - 1);

        // Add to bounded cache (drop-oldest):
        if (g_detection_count >= kAIDetectionMaxEvents) {
            // Drop oldest:
            for (uint32_t i = 1; i < g_detection_count; i++)
                g_detections[i - 1] = g_detections[i];
            g_detection_count--;
        }
        g_detections[g_detection_count++] = ev;
        g_status.detections_total++;

        // Publish on event bus:
        publishObjectDetected(ev);
    }

    return ESP_OK;
}

esp_err_t aiManagerSelectProvider(const char* provider_id) {
    if (provider_id == nullptr) return ESP_ERR_INVALID_ARG;

    if (std::strcmp(provider_id, kProviderNone) == 0) {
        if (g_no_ai == nullptr) return ESP_ERR_INVALID_STATE;
        if (g_provider != nullptr && g_provider != g_no_ai)
            g_provider->stop();
        g_provider = g_no_ai;
        g_config.provider = kProviderNone;
        g_status.provider = kProviderNone;
        g_status.provider_ready = true;   // NoAiProvider is always ready
        g_status.last_error.clear();
        return ESP_OK;
    }

    if (std::strcmp(provider_id, kProviderExternal) == 0) {
        if (g_external_ai == nullptr) return ESP_ERR_INVALID_STATE;
        if (g_provider != nullptr && g_provider != g_external_ai)
            g_provider->stop();
        // Apply current connection settings, then begin. An unconfigured
        // endpoint is a STATE ("not configured"), not a selection failure:
        // D6 says the user picks first and configures second.
        g_external_ai->setConfig(g_config);
        g_external_ai->begin();
        g_provider = g_external_ai;
        g_config.provider = kProviderExternal;
        g_status.provider = kProviderExternal;
        g_status.provider_ready = g_external_ai->isReady();
        g_status.last_error =
            g_status.provider_ready ? "" : "not configured — set endpoint";
        return ESP_OK;
    }

    if (std::strcmp(provider_id, kProviderTflite) == 0) {
        if (g_tflite == nullptr) return ESP_ERR_INVALID_STATE;
        if (g_provider != nullptr && g_provider != g_tflite)
            g_provider->stop();
        g_tflite->setConfig(g_config);
        g_provider = g_tflite;
        g_config.provider = kProviderTflite;
        g_status.provider = kProviderTflite;
        // Owner decision D4 (2026-11-20): the choice ships and persists,
        // but the runtime is not linked — begin()/infer() return
        // ESP_ERR_NOT_SUPPORTED and the UI shows "Unavailable" from this
        // honest status instead of hiding the option.
        g_status.provider_ready = false;
        g_status.last_error =
            "on-device runtime not included in this build (D4)";
        return ESP_OK;
    }

    return ESP_ERR_INVALID_ARG;
}

}  // namespace visioncam
