// tflite_micro_provider.cpp — TFLite Micro on-device AI provider (Phase 6;
// SPEC §4.4.1; owner decision D4 — ANSWERED 2026-11-20: "selectable but
// unlinked").
//
// The TensorFlow Lite Micro library is NOT linked into this build. The
// provider choice ships in the UI and persists, but begin()/infer() and
// model activation return ESP_ERR_NOT_SUPPORTED with the honest D4
// message (see tflite_micro_provider.h for the revisit condition).

#include "tflite_micro_provider.h"

#include <cstdio>
#include <cstring>

#include "esp_heap_caps.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"

#include "event_bus.h"
#include "storage_manager.h"
#include "wall_clock.h"

namespace visioncam {

static const char* TAG = "vc_tflm";

// ---- TFLM model directory layout (SPEC §4.8) -----------------------------
constexpr char kDirModels[] = "/sdcard/models";
constexpr char kDirModelsStaging[] = "/sdcard/models/staging";
constexpr char kDirModelsActive[] = "/sdcard/models/active";
constexpr char kDirModelsBackup[] = "/sdcard/models/backup";

// TFLM model file magic number (first 4 bytes of a flatbuffer model).
constexpr uint32_t kTFLModelMagic = 0x4E4F4954; // "TINO" little-endian

// Max model file size (2 MB — bounded, never read whole into RAM).
constexpr uint32_t kTFLModelMaxBytes = 2 * 1024 * 1024;

// Model manifest file name.
constexpr char kManifestFileName[] = "manifest.json";

// JPEG decode buffer for 1:8 scale RGB888
static constexpr size_t kJpegDecodeBufferSize = 32 * 1024;

// ---- TfliteMicroProvider implementation -----------------------------------

esp_err_t TfliteMicroProvider::begin() {
    // TFLM library not linked — return NOT_SUPPORTED until the owner
    // adds the tensorflow-lite-micro component and implements the
    // interpreter methods below.
    ESP_LOGW(TAG,
             "TFLite Micro provider: library not linked — return "
             "ESP_ERR_NOT_SUPPORTED. Add espressif/esp-tflmicro as a "
             "managed component and implement the interpreter methods "
             "in tflite_micro_provider.cpp to enable.");
    return ESP_ERR_NOT_SUPPORTED;
}

bool TfliteMicroProvider::isReady() const { return ready_; }

void TfliteMicroProvider::stop() { 
    ready_ = false;
    if (interpreter_) {
        // Interpreter cleanup would go here when TFLM is linked
    }
}

esp_err_t TfliteMicroProvider::setConfig(const AIConfig& config) {
    tensor_arena_bytes_ = config.tensor_arena_bytes;
    return ESP_OK;
}

esp_err_t TfliteMicroProvider::infer(const FrameView& frame,
                                     InferenceResult* out) {
    if (!ready_) {
        return ESP_ERR_INVALID_STATE;
    }
    if (frame.data == nullptr || frame.len == 0 || out == nullptr) {
        return ESP_ERR_INVALID_ARG;
    }
    
    out->model_id = active_model_id_;
    out->inference_ms = 0;
    out->detections.clear();
    return ESP_ERR_NOT_SUPPORTED;
}

// ---- Model management ------------------------------------------------------

esp_err_t TfliteMicroProvider::modelUploadStage(const char* model_id,
                                                const uint8_t* data, size_t len) {
    if (model_id == nullptr || data == nullptr || len == 0)
        return ESP_ERR_INVALID_ARG;
    if (len > kTFLModelMaxBytes)
        return ESP_ERR_INVALID_SIZE;

    // Stage the model file under /models/staging/<model_id>.tflite
    char path[128] = {};
    int n = std::snprintf(path, sizeof(path),
                          "%s/%s.tflite", kDirModelsStaging, model_id);
    if (n < 0 || static_cast<size_t>(n) >= sizeof(path))
        return ESP_ERR_INVALID_SIZE;

    // Write to staging file (creates parent dirs via storage manager):
    esp_err_t err = storageWriteFile(path, data, len);
    if (err != ESP_OK) return err;

    ESP_LOGI(TAG, "TFLM model staged: %s (%u bytes)", model_id,
             static_cast<unsigned int>(len));
    return ESP_OK;
}

esp_err_t TfliteMicroProvider::modelValidate(const char* model_id,
                                             ModelManifest* manifest) {
    if (model_id == nullptr || manifest == nullptr)
        return ESP_ERR_INVALID_ARG;

    char path[128] = {};
    int n = std::snprintf(path, sizeof(path),
                          "%s/%s.tflite", kDirModelsStaging, model_id);
    if (n < 0 || static_cast<size_t>(n) >= sizeof(path))
        return ESP_ERR_INVALID_SIZE;

    // Get file size:
    uint32_t fsize = 0, ftime = 0;
    esp_err_t err = storageStatFile(path, &fsize, &ftime);
    if (err != ESP_OK) return err;

    // Check magic number (first 4 bytes):
    void* header_buf = std::malloc(8);
    if (header_buf == nullptr) return ESP_ERR_NO_MEM;
    size_t got = 0;
    err = storageReadFile(path,
                          reinterpret_cast<uint8_t*>(header_buf), 8, &got);
    if (err != ESP_OK || got < 4) {
        std::free(header_buf);
        return ESP_ERR_INVALID_ARG;
    }
    uint32_t magic = 0;
    std::memcpy(&magic, header_buf, 4);
    std::free(header_buf);

    if (magic != kTFLModelMagic) {
        ESP_LOGW(TAG, "TFLM model %s: bad magic 0x%08X (expected 0x%08X)",
                 model_id, magic, kTFLModelMagic);
        return ESP_ERR_INVALID_ARG;
    }

    // TODO: Parse full flatbuffer schema using TFLM's Model::Verify()
    // For now, just check magic and basic structure

    // Populate manifest with what we can determine:
    manifest->model_id = model_id;
    manifest->format = "tflite_micro";
    manifest->file = path;
    manifest->required_tensor_arena_bytes = tensor_arena_bytes_;

    ESP_LOGI(TAG, "TFLM model %s: valid magic, size=%u bytes",
             model_id, static_cast<unsigned int>(fsize));
    return ESP_OK;
}

esp_err_t TfliteMicroProvider::modelActivate(const char* model_id) {
    if (model_id == nullptr) return ESP_ERR_INVALID_ARG;

    char staging_path[128] = {};
    char active_path[128] = {};
    char backup_path[128] = {};

    int n = std::snprintf(staging_path, sizeof(staging_path),
                          "%s/%s.tflite", kDirModelsStaging, model_id);
    if (n < 0 || static_cast<size_t>(n) >= sizeof(staging_path))
        return ESP_ERR_INVALID_SIZE;

    n = std::snprintf(active_path, sizeof(active_path),
                      "%s/%s.tflite", kDirModelsActive, model_id);
    if (n < 0 || static_cast<size_t>(n) >= sizeof(active_path))
        return ESP_ERR_INVALID_SIZE;

    n = std::snprintf(backup_path, sizeof(backup_path),
                      "%s/%s.tflite", kDirModelsBackup, model_id);
    if (n < 0 || static_cast<size_t>(n) >= sizeof(backup_path))
        return ESP_ERR_INVALID_SIZE;

    // If there's an existing active model, move it to backup first:
    uint32_t dummy = 0, dummy2 = 0;
    if (storageStatFile(active_path, &dummy, &dummy2) == ESP_OK) {
        esp_err_t err = storageRenameFile(active_path, backup_path);
        if (err != ESP_OK) {
            ESP_LOGW(TAG, "TFLM: failed to back up existing active model");
            // Continue anyway — we'll overwrite active below.
        }
    }

    // Move staged model to active:
    esp_err_t err = storageRenameFile(staging_path, active_path);
    if (err != ESP_OK) return err;

    // Reinitialize interpreter with new model
    err = initializeInterpreter(active_path);
    if (err != ESP_OK) {
        // Rollback on failure
        storageRenameFile(backup_path, active_path);
        return err;
    }

    active_model_id_ = model_id;
    ready_ = true;

    ESP_LOGI(TAG, "TFLM model %s: activated", model_id);
    return ESP_OK;
}

esp_err_t TfliteMicroProvider::modelRollback(const char* model_id) {
    if (model_id == nullptr) return ESP_ERR_INVALID_ARG;

    char active_path[128] = {};
    char backup_path[128] = {};

    int n = std::snprintf(active_path, sizeof(active_path),
                          "%s/%s.tflite", kDirModelsActive, model_id);
    if (n < 0 || static_cast<size_t>(n) >= sizeof(active_path))
        return ESP_ERR_INVALID_SIZE;

    n = std::snprintf(backup_path, sizeof(backup_path),
                      "%s/%s.tflite", kDirModelsBackup, model_id);
    if (n < 0 || static_cast<size_t>(n) >= sizeof(backup_path))
        return ESP_ERR_INVALID_SIZE;

    // Move backup back to active:
    esp_err_t err = storageRenameFile(backup_path, active_path);
    if (err != ESP_OK) return err;

    // Reinitialize interpreter
    err = initializeInterpreter(active_path);
    if (err != ESP_OK) {
        // Try to restore the previous active model
        storageRenameFile(active_path, backup_path);
        return err;
    }

    active_model_id_ = model_id;
    ESP_LOGI(TAG, "TFLM model %s: rolled back from backup", model_id);
    return ESP_OK;
}

esp_err_t TfliteMicroProvider::modelDelete(const char* model_id) {
    if (model_id == nullptr) return ESP_ERR_INVALID_ARG;

    char path[128] = {};
    int n = std::snprintf(path, sizeof(path),
                          "%s/%s.tflite", kDirModelsActive, model_id);
    if (n < 0 || static_cast<size_t>(n) >= sizeof(path))
        return ESP_ERR_INVALID_SIZE;

    esp_err_t err = storageRemoveFile(path);
    if (err != ESP_OK && err != ESP_ERR_NOT_FOUND) return err;

    // Also remove from staging if present:
    n = std::snprintf(path, sizeof(path),
                      "%s/%s.tflite", kDirModelsStaging, model_id);
    if (n < 0 || static_cast<size_t>(n) >= sizeof(path))
        return ESP_ERR_INVALID_SIZE;
    storageRemoveFile(path);

    if (active_model_id_ == model_id) {
        active_model_id_.clear();
        ready_ = false;
        // interpreter_.reset(); // Would reset interpreter when TFLM is linked
    }

    return ESP_OK;
}

// ---- Private methods (stubs) ------------------------------------------------------

esp_err_t TfliteMicroProvider::initializeInterpreter(const char* /*model_path*/) {
    ESP_LOGW(TAG, "TFLM: interpreter initialization not implemented (D4 pending)");
    return ESP_ERR_NOT_SUPPORTED;
}

uint8_t* TfliteMicroProvider::getInputTensor() {
    return nullptr;
}

esp_err_t TfliteMicroProvider::decodeJpegToTensor(const uint8_t* /*jpeg_data*/, 
                                                  size_t /*jpeg_len*/,
                                                  uint8_t* /*output_tensor*/) {
    return ESP_ERR_NOT_SUPPORTED;
}

void TfliteMicroProvider::parseOutputTensors(InferenceResult* /*out*/) {
    // Model-specific output parsing would go here when TFLM is linked
}

}  // namespace visioncam