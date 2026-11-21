// tflite_micro_provider.h — TFLite Micro on-device AI provider class header
// (Phase 6; SPEC §4.4.1; owner decision D4 — ANSWERED 2026-11-20:
// "selectable but unlinked").
//
// The TensorFlow Lite Micro library is NOT linked into this build. The
// provider choice nevertheless ships in the UI: selecting tflite_micro
// persists with provider_ready=false and last_error =
// "on-device runtime not included in this build (D4)", and model
// activation returns ESP_ERR_NOT_SUPPORTED (mapped to HTTP 400 with the
// D4 message). Model staging/validation paths still work so the upload
// flow is exercisable.
// To actually run on-device inference later (revisit D4 only when flash
// headroom is ~30 % free):
//   1. Add espressif/esp-tflmicro as a managed component
//   2. Uncomment the TFLM includes below
//   3. Implement the interpreter methods

#pragma once

#include <cstddef>
#include <cstdint>
#include <memory>
#include <string>
#include <vector>

#include "ai_provider.h"

namespace visioncam {

// Forward declare TFLM types to avoid including TFLM headers
namespace tflite {
class Model;
class MicroInterpreter;
template <size_t N> class MicroMutableOpResolver;
struct TfLiteTensor;
using TfLiteType = int; // placeholder
}  // namespace tflite

class TfliteMicroProvider : public IInferenceProvider {
public:
    TfliteMicroProvider() = default;

    esp_err_t begin() override;
    bool isReady() const override;
    esp_err_t infer(const FrameView& frame,
                    InferenceResult* out) override;
    void stop() override;
    const char* providerId() const override { return kProviderTflite; }

    // Configure the TFLM provider (called from aiManagerSetConfig).
    esp_err_t setConfig(const AIConfig& config);

    // Model management (called by ai_manager):
    esp_err_t modelUploadStage(const char* model_id,
                               const uint8_t* data, size_t len);
    esp_err_t modelValidate(const char* model_id,
                            ModelManifest* manifest);
    esp_err_t modelActivate(const char* model_id);
    esp_err_t modelRollback(const char* model_id);
    esp_err_t modelDelete(const char* model_id);

private:
    // TFLM interpreter and model (void* since TFLM not linked in stub mode)
    void* model_ = nullptr;
    void* resolver_ = nullptr;
    void* interpreter_ = nullptr;
    
    // Model data buffer (PSRAM)
    std::unique_ptr<uint8_t[]> model_data_;
    
    // JPEG decode buffer for 1:8 scale RGB888
    std::unique_ptr<uint8_t[]> jpeg_decode_buffer_;
    
    // TFLM tensor arena (PSRAM)
    std::vector<uint8_t> tensor_arena_;
    size_t tensor_arena_bytes_ = 65536;
    
    // Model state
    bool ready_ = false;
    std::string active_model_id_;
    int input_tensor_index_ = 0;
    int input_type_ = 0; // placeholder for TfLiteType

    // ---- Private methods ------------------------------------------------------
    esp_err_t initializeInterpreter(const char* model_path);
    uint8_t* getInputTensor();
    esp_err_t decodeJpegToTensor(const uint8_t* jpeg_data, 
                                 size_t jpeg_len,
                                 uint8_t* output_tensor);
    void parseOutputTensors(InferenceResult* out);

    // Constants
    static constexpr char kDirModels[] = "/sdcard/models";
    static constexpr char kDirModelsStaging[] = "/sdcard/models/staging";
    static constexpr char kDirModelsActive[] = "/sdcard/models/active";
    static constexpr char kDirModelsBackup[] = "/sdcard/models/backup";
    static constexpr uint32_t kTFLModelMagic = 0x4E4F4954; // "TINO" little-endian
    static constexpr uint32_t kTFLModelMaxBytes = 2 * 1024 * 1024;
    static constexpr size_t kJpegDecodeBufferSize = 32 * 1024;
    static constexpr char kManifestFileName[] = "manifest.json";
};

}  // namespace visioncam