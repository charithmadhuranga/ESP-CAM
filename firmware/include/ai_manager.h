// ai_manager.h — AI inference manager (Phase 6 task 6.1-6.4; SPEC §4.4).
//
// Orchestrates the AI provider, runs inference on selected frames (after
// motion/event per R1's usage pattern), and publishes ObjectDetected events
// on the typed event bus. Runs at LOW priority so streaming/storage always
// win.
//
// Bounds (AGENTS rules 12-13):
//   kAIEventQueueDepth — bounded event queue via event bus (drop-oldest)
//   kAIDetectionMaxEvents — bounded detection event cache
//   kAIRequestQueueMax — bounded external AI request queue

#pragma once

#include <cstddef>
#include <cstdint>
#include <vector>
#include <string>

#include "esp_err.h"

#include "ai_provider.h"

namespace visioncam {

// ---- Bounds (AGENTS rules 12-13) -----------------------------------------
constexpr uint32_t kAIEventQueueDepth = 8;   // event bus depth (drop-oldest)
constexpr uint32_t kAIDetectionMaxEvents = 256; // bounded detection cache
constexpr uint32_t kAIRequestQueueMax = 4;   // external AI request queue

// ---- Detection event (stored in event index) ------------------------------

struct AIDetectionEvent {
    char event_id[24];       // "evt-XXXXXXXX"
    char source[32];         // "external_ai" | "tflite_micro"
    int class_id = 0;
    std::string label;
    float confidence = 0.0f;
    float bbox[4] = {0, 0, 0, 0}; // x, y, width, height (normalised)
    uint32_t timestamp = 0;   // epoch seconds
    char snapshot_id[24] = {}; // associated snapshot, if any
    char notification_status[16] = "queued";
};

// ---- AI status ------------------------------------------------------------

struct AIStatus {
    bool enabled = false;
    std::string provider;        // current provider ID
    bool provider_ready = false;
    uint32_t inferences_total = 0;
    uint32_t inferences_ok = 0;
    uint32_t inferences_failed = 0;
    uint32_t detections_total = 0;
    uint32_t last_inference_ms = 0;
    uint32_t last_inference_duration_ms = 0;
    std::string last_error;
};

// ---- Public API ----------------------------------------------------------

// Initialise AI manager. Call after event bus, frame manager, motion manager.
// Loads config from NVS, selects provider, begins it.
esp_err_t aiManagerInit();

// Shut down AI manager — stops provider, releases arena.
esp_err_t aiManagerShutdown();

// Get config (copy).
esp_err_t aiManagerGetConfig(AIConfig* out);

// Set config (parse → validate → persist → apply → return sanitised,
// SPEC 7 process). Partial updates overlay on current config.
esp_err_t aiManagerSetConfig(const AIConfig* candidate, AIConfig* effective);

// Get status.
esp_err_t aiManagerStatus(AIStatus* out);

// List available providers.
esp_err_t aiManagerListProviders(char* out_ids, size_t out_cap,
                                 size_t* out_count);

// Get model manifest for the active model.
esp_err_t aiManagerGetModelManifest(ModelManifest* out);

// Upload a model to staging (streamed to file, never buffered in RAM).
esp_err_t aiManagerUploadModel(const char* model_id,
                               const uint8_t* data, size_t len);

// Validate a staged model (header, checksum, operators, arena).
esp_err_t aiManagerValidateModel(const char* model_id);

// Activate a validated model (never overwrites active; keeps backup).
esp_err_t aiManagerActivateModel(const char* model_id);

// Deactivate AI (switch to NoAiProvider).
esp_err_t aiManagerDeactivateAI();

// Roll back to last-known-good model (from backup).
esp_err_t aiManagerRollbackModel();

// List detection events (paginated).
esp_err_t aiManagerListEvents(uint32_t offset, uint32_t limit,
                              AIDetectionEvent* out, size_t out_cap,
                              size_t* out_count, bool* out_has_more);

// ---- Internal: called by AI task ----------------------------------------

// Run inference on one frame. Called from the AI task after a motion/event
// trigger.
esp_err_t aiManagerInferFrame(const FrameView& frame);

// ---- Provider selection --------------------------------------------------

// Select a provider by ID.
esp_err_t aiManagerSelectProvider(const char* provider_id);

}  // namespace visioncam
