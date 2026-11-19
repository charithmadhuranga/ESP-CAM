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
constexpr uint32_t kAIConfigSchema = 1;

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
static TfliteMicroProvider* g_tflite = nullptr;

// Bounded detection event cache:
static AIDetectionEvent g_detections[kAIDetectionMaxEvents] = {};
static uint32_t g_detection_count = 0;
static uint32_t g_next_event_id = 1;

// Inference rate limiting:
static uint32_t g_next_inference_ms = 0;

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

// ---- AI task -------------------------------------------------------------

static void aiTask(void* /*param*/) {
    while (true) {
        if (!g_initialized || !g_config.enabled || g_provider == nullptr) {
            vTaskDelay(pdMS_TO_TICKS(500));
            continue;
        }

        if (!g_provider->isReady()) {
            vTaskDelay(pdMS_TO_TICKS(1000));
            continue;
        }

        // Rate limit inference:
        uint32_t now_ms = static_cast<uint32_t>(esp_timer_get_time() / 1000);
        if (g_next_inference_ms == 0)
            g_next_inference_ms = now_ms + g_config.inference_interval_ms;
        if (now_ms < g_next_inference_ms) {
            vTaskDelay(pdMS_TO_TICKS(g_next_inference_ms - now_ms));
            continue;
        }
        g_next_inference_ms = now_ms + g_config.inference_interval_ms;

        // Acquire a frame from the motion event trigger path.
        // For now, the AI task is idle — inference is triggered externally
        // via aiManagerInferFrame() when motion/event occurs.
        // This keeps the task from spinning.
        vTaskDelay(pdMS_TO_TICKS(100));
    }
}

// ---- Public API ----------------------------------------------------------

esp_err_t aiManagerInit() {
    // Load config from NVS:
    nvs_handle_t h = 0;
    esp_err_t err = nvs_open(kAINvsNs, NVS_READONLY, &h);
    if (err == ESP_OK) {
        struct Guard {
            nvs_handle_t hh;
            ~Guard() { nvs_close(hh); }
        } guard{h};
        uint8_t en = 0;
        uint32_t v = 0;
        char provider[32] = {};
        size_t provider_len = sizeof(provider);
        if (nvs_get_u32(h, kAIKeyVer, &v) == ESP_OK &&
            v == kAIConfigSchema) {
            nvs_get_u8(h, kAIKeyEnabled, &en);
            g_config.enabled = (en != 0);
            nvs_get_str(h, kAIKeyProvider, provider, &provider_len);
            g_config.provider = provider;
            nvs_get_u32(h, kAIKeyTimeout, &g_config.timeout_ms);
            nvs_get_u32(h, kAIKeyInterval, &g_config.inference_interval_ms);
            char model[64] = {};
            size_t model_len = sizeof(model);
            if (nvs_get_str(h, kAIKeyActiveModel, model, &model_len) == ESP_OK)
                g_config.active_model_id = model;
        }
    }

    // Create NoAiProvider (always available):
    g_no_ai = new NoAiProvider();
    if (g_no_ai == nullptr) return ESP_ERR_NO_MEM;
    err = g_no_ai->begin();
    if (err != ESP_OK) {
        delete g_no_ai;
        g_no_ai = nullptr;
        return err;
    }

    // Create TfliteMicroProvider (available but returns NOT_SUPPORTED
    // until the TFLM library is linked — owner decision D4):
    g_tflite = new TfliteMicroProvider();
    if (g_tflite == nullptr) return ESP_ERR_NO_MEM;

    // Select provider:
    err = aiManagerSelectProvider(g_config.provider.c_str());
    if (err != ESP_OK) {
        // Fall back to NoAiProvider:
        g_provider = g_no_ai;
    }

    // Create AI task:
    if (xTaskCreate(aiTask, "vc_ai", VC_STACK_AI, nullptr,
                    VC_PRIO_AI, nullptr) != pdPASS) {
        return ESP_ERR_NO_MEM;
    }

    g_initialized = true;
    g_status.enabled = g_config.enabled;
    g_status.provider = g_config.provider;
    g_status.provider_ready = (g_provider != nullptr &&
                               g_provider->isReady());
    ESP_LOGI(TAG, "AI manager init: provider=%s enabled=%d",
             g_config.provider.c_str(), g_config.enabled ? 1 : 0);
    return ESP_OK;
}

esp_err_t aiManagerShutdown() {
    g_initialized = false;
    g_config.enabled = false;
    if (g_provider != nullptr) {
        g_provider->stop();
        g_provider = nullptr;
    }
    if (g_no_ai != nullptr) {
        delete g_no_ai;
        g_no_ai = nullptr;
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

    AIConfig new_cfg = g_config;

    // Validate ranges:
    if (candidate->timeout_ms > 0) {
        if (candidate->timeout_ms > 60000) return ESP_ERR_INVALID_ARG;
        new_cfg.timeout_ms = candidate->timeout_ms;
    }
    if (candidate->inference_interval_ms > 0) {
        if (candidate->inference_interval_ms > 3600000)
            return ESP_ERR_INVALID_ARG;
        new_cfg.inference_interval_ms = candidate->inference_interval_ms;
    }
    if (!candidate->provider.empty()) {
        if (candidate->provider != kProviderNone &&
            candidate->provider != kProviderExternal &&
            candidate->provider != kProviderTflite)
            return ESP_ERR_INVALID_ARG;
        new_cfg.provider = candidate->provider;
    }
    if (!candidate->base_url.empty())
        new_cfg.base_url = candidate->base_url;
    if (!candidate->auth_token.empty())
        new_cfg.auth_token = candidate->auth_token;
    if (candidate->upload_width > 0)
        new_cfg.upload_width = candidate->upload_width;
    if (candidate->upload_height > 0)
        new_cfg.upload_height = candidate->upload_height;
    if (candidate->upload_quality > 0) {
        if (candidate->upload_quality > 63) return ESP_ERR_INVALID_ARG;
        new_cfg.upload_quality = candidate->upload_quality;
    }
    if (!candidate->active_model_id.empty())
        new_cfg.active_model_id = candidate->active_model_id;

    // Persist to NVS:
    nvs_handle_t h = 0;
    esp_err_t err = nvs_open(kAINvsNs, NVS_READWRITE, &h);
    if (err != ESP_OK) return err;
    struct Guard {
        nvs_handle_t hh;
        ~Guard() { nvs_close(hh); }
    } guard{h};
    nvs_set_u32(h, kAIKeyVer, kAIConfigSchema);
    nvs_set_u8(h, kAIKeyEnabled, new_cfg.enabled ? 1 : 0);
    nvs_set_str(h, kAIKeyProvider, new_cfg.provider.c_str());
    nvs_set_u32(h, kAIKeyTimeout, new_cfg.timeout_ms);
    nvs_set_u32(h, kAIKeyInterval, new_cfg.inference_interval_ms);
    nvs_set_str(h, kAIKeyActiveModel, new_cfg.active_model_id.c_str());
    err = nvs_commit(h);
    if (err != ESP_OK) return err;

    // Apply:
    g_config = new_cfg;
    if (g_config.provider != new_cfg.provider) {
        // Switch provider:
        if (g_provider != nullptr && g_provider != g_no_ai) {
            g_provider->stop();
        }
        g_provider = g_no_ai;
        err = aiManagerSelectProvider(new_cfg.provider.c_str());
        if (err != ESP_OK) {
            g_provider = g_no_ai;
        }
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

    *out_count = 0;
    size_t n = 0;
    const char* providers[] = {kProviderNone, kProviderExternal,
                               kProviderTflite};
    for (size_t i = 0; i < sizeof(providers) / sizeof(providers[0]); i++) {
        size_t len = std::strlen(providers[i]);
        if (n + len + 1 > out_cap) break;
        std::memcpy(out_ids + n, providers[i], len);
        n += len;
        out_ids[n++] = '\0';
    }
    *out_count = n;
    return ESP_OK;
}

esp_err_t aiManagerGetModelManifest(ModelManifest* /*out*/) {
    // Manifest loading from /models/staging/ — not yet implemented
    return ESP_ERR_NOT_SUPPORTED;
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
    g_config.enabled = false;
    g_status.enabled = false;
    return ESP_OK;
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

    // Publish detections as events:
    for (const auto& det : result.detections) {
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
        g_provider = g_no_ai;
        g_config.provider = kProviderNone;
        g_status.provider = kProviderNone;
        g_status.provider_ready = true;
        return ESP_OK;
    }

    if (std::strcmp(provider_id, kProviderExternal) == 0) {
        // External AI provider — not yet implemented (would use esp_http_client)
        g_provider = g_no_ai; // fall back to NoAi
        g_config.provider = kProviderNone;
        return ESP_ERR_NOT_SUPPORTED;
    }

    if (std::strcmp(provider_id, kProviderTflite) == 0) {
        // TFLite Micro provider — not yet implemented (owner decision D4)
        g_provider = g_no_ai; // fall back to NoAi
        g_config.provider = kProviderNone;
        return ESP_ERR_NOT_SUPPORTED;
    }

    return ESP_ERR_INVALID_ARG;
}

}  // namespace visioncam
