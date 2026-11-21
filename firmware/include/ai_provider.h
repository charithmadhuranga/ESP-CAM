// ai_provider.h — AI inference provider abstraction (Phase 6 task 6.1;
// SPEC §4.4).
//
// Provider interface for pluggable AI inference. Three providers:
//   NoAiProvider     — default, AI fully disabled
//   ExternalAiProvider — primary path for original ESP32
//   TfliteMicroProvider — experimental on-device (owner decision D4)
//
// All providers run at LOW priority (VC_PRIO_AI) and never block streaming
// or storage. Inference is triggered after motion/event (R1's usage
// pattern), not per-frame.
//
// FrameView is a bounded view into a JPEG frame — the provider may copy
// the frame into its own bounded buffer but must not hold the frame lease
// across blocking I/O (AGENTS rule 9, SPEC 4.1.1 rule 9).

#pragma once

#include <cstddef>
#include <cstdint>
#include <vector>
#include <string>

#include "esp_err.h"

namespace visioncam {

// Normalised detection — consistent schema from every provider (SPEC §4.4.3).
// Bbox is [x, y, width, height], normalised 0.0–1.0.
struct Detection {
    int class_id = 0;
    std::string label;
    float confidence = 0.0f;   // 0.0–1.0
    float bbox[4] = {0, 0, 0, 0}; // x, y, width, height (normalised)
};

// A single inference result.
struct InferenceResult {
    std::string model_id;
    uint32_t inference_ms = 0;
    std::vector<Detection> detections;
};

// A view into a camera frame (JPEG bytes). The frame manager owns the
// buffer; the view is a non-owning reference valid only until the lease
// is released.
struct FrameView {
    const uint8_t* data = nullptr;
    size_t len = 0;
    uint32_t width = 0;
    uint32_t height = 0;
    uint64_t ts_us = 0;
};

// Provider interface — implement for each AI backend.
class IInferenceProvider {
public:
    virtual ~IInferenceProvider() = default;

    // Initialise the provider (load model, allocate arena, etc.).
    virtual esp_err_t begin() = 0;

    // True when the provider is ready to infer.
    virtual bool isReady() const = 0;

    // Run inference on a frame. Returns detections in the normalised schema.
    virtual esp_err_t infer(const FrameView& frame,
                            InferenceResult* out) = 0;

    // Stop the provider and release resources.
    virtual void stop() = 0;

    // Provider identifier string (e.g. "external_ai", "tflite_micro", "none").
    virtual const char* providerId() const = 0;
};

// Provider IDs.
constexpr char kProviderNone[] = "none";
constexpr char kProviderExternal[] = "external_ai";
constexpr char kProviderTflite[] = "tflite_micro";

// Model manifest (SPEC §4.4.1).
struct ModelManifest {
    uint32_t schema_version = 1;
    std::string model_id;
    std::string display_name;
    std::string format;           // "tflite_micro" | "external"
    std::string file;             // filename under /models/
    std::string sha256;
    // Input tensor:
    uint32_t input_width = 0;
    uint32_t input_height = 0;
    uint32_t input_channels = 0;
    std::string input_dtype;      // "int8" | "uint8" | "float32"
    std::string input_layout;     // "NHWC" | "NCHW"
    // Output tensor:
    std::string output_dtype;
    std::string output_description;
    uint32_t required_tensor_arena_bytes = 0;
    std::vector<std::string> operators;
    std::vector<std::string> classes;
    std::string preprocessing;
};

// AI configuration (SPEC §4.4.2 for external AI).
struct AIConfig {
    // Owner decision D6 (2026-11-20): the AI framework ships ENABLED with
    // provider "none" — the subsystem is live on every boot and the user
    // only picks a provider in the UI. Supersedes SPEC §7's
    // "enabled": false example default (recorded in owner-decisions.md).
    bool enabled = true;
    // Default provider "none" = NoAiProvider (SPEC §4.4): AI on, nothing
    // selected yet — not "AI off".
    std::string provider = kProviderNone; // "none" | "external_ai" | "tflite_micro"
    // External AI:
    std::string base_url;
    std::string auth_token;
    uint32_t timeout_ms = 5000;
    uint32_t upload_width = 640;
    uint32_t upload_height = 480;
    uint32_t upload_quality = 12;
    // SPEC §7 ai schema example default (5000 ms) — also the minimum gap
    // between two motion-triggered inferences.
    uint32_t inference_interval_ms = 5000;
    // SPEC §7 ai schema: detections below this are discarded before they
    // are recorded or published.
    float confidence_threshold = 0.6f;
    std::vector<std::string> event_types; // which events trigger inference
    uint32_t max_retries = 3;
    bool tls_verify = true;
    uint32_t max_upload_bytes = 256 * 1024;
    // TFLM:
    std::string active_model_id;
    uint32_t tensor_arena_bytes = 65536;
};

}  // namespace visioncam
