// external_ai_provider.h — External AI inference provider (Phase 6; SPEC §4.4.2).
//
// Sends JPEG frames to an external HTTP(S) AI inference server via
// multipart/form-data POST to /api/v1/infer. Never blocks streaming or
// storage. All configuration (base URL, auth token, timeout, quality,
// interval) lives in AIConfig. Credentials are stored in NVS but never
// returned by GET handlers (AGENTS rule 17).

#pragma once

#include <cstddef>
#include <cstdint>
#include <string>

#include "ai_provider.h"

namespace visioncam {

class ExternalAiProvider : public IInferenceProvider {
public:
    ExternalAiProvider() = default;

    esp_err_t begin() override;
    bool isReady() const override;
    esp_err_t infer(const FrameView& frame,
                    InferenceResult* out) override;
    void stop() override;
    const char* providerId() const override { return kProviderExternal; }

    // Configure the external AI endpoint (called from aiManagerSetConfig).
    esp_err_t setConfig(const AIConfig& config);

private:
    bool ready_ = false;
    std::string base_url_;
    std::string auth_token_;
    uint32_t timeout_ms_ = 5000;
    uint32_t upload_width_ = 640;
    uint32_t upload_height_ = 480;
    uint32_t upload_quality_ = 12;
    bool tls_verify_ = true;

    esp_err_t httpPostJpeg(const uint8_t* jpeg_data, size_t jpeg_len,
                           InferenceResult* out);
};

}  // namespace visioncam
