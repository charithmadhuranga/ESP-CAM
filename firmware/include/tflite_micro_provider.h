// tflite_micro_provider.h — TFLite Micro on-device AI provider class header
// (Phase 6; SPEC §4.4.1; owner decision D4 pending).
//
// The actual TensorFlow Lite Micro library is NOT linked into this build.
// This class provides the structure and model management (staging, validate,
// activate, rollback) with ESP_ERR_NOT_SUPPORTED returned for all TFLM-specific
// operations until the library is linked.

#pragma once

#include <cstddef>
#include <cstdint>

#include "ai_provider.h"

namespace visioncam {

class TfliteMicroProvider : public IInferenceProvider {
public:
    TfliteMicroProvider() = default;

    esp_err_t begin() override;
    bool isReady() const override;
    esp_err_t infer(const FrameView& frame,
                    InferenceResult* out) override;
    void stop() override;
    const char* providerId() const override;

    // Model management (called by ai_manager):
    esp_err_t modelUploadStage(const char* model_id,
                               const uint8_t* data, size_t len);
    esp_err_t modelValidate(const char* model_id,
                            ModelManifest* manifest);
    esp_err_t modelActivate(const char* model_id);
    esp_err_t modelRollback(const char* model_id);
    esp_err_t modelDelete(const char* model_id);

private:
    bool ready_ = false;
};

}  // namespace visioncam
