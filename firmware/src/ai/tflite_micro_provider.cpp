// tflite_micro_provider.cpp — TFLite Micro on-device AI provider (Phase 6;
// SPEC §4.4.1; owner decision D4 pending).
//
// This is a clean-room implementation of the TFLite Micro provider for
// VisionCam Pro. It follows R1's usage pattern: classify *after* motion
// events to decide whether to keep an event, not per-frame classification.
//
// NOTE: The actual TensorFlow Lite Micro library is NOT linked into this
// build (it is not available as a managed component and adding it requires
// a separate owner decision). The TfliteMicroProvider class is implemented
// with proper structure and returns ESP_ERR_NOT_SUPPORTED for all
// TFLM-specific operations until the library is linked. This gives the
// owner a clear path to enable TFLM when they add the library:
//
//   1. Add TFLM as a managed component (e.g. espressif/esp-tflmicro)
//   2. Uncomment the #include <tensorflow/lite/micro/micro_interpreter.h>
//      line below and implement the interpreter methods
//   3. Implement the model validation (magic number, schema, operators)
//   4. Implement the inference loop (allocate tensors, copy input, invoke,
//      read output)
//
// All model files live under /models/{staging,active,backup}/
// (SPEC §4.8 directory layout). Model manifests are JSON files with the
// schema from SPEC §4.4.1.

#include "tflite_micro_provider.h"

#include <cstdio>
#include <cstring>
#include <ctime>

#include "esp_heap_caps.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"

#include "nvs_flash.h"
#include "nvs.h"

#include "storage_manager.h"

// TODO: When TFLM library is linked, uncomment:
// #include <tensorflow/lite/micro/micro_interpreter.h>
// #include <tensorflow/lite/micro/all_ops_resolver.h>
// #include <tensorflow/lite/micro/micro_mutable_op_resolver.h>
// #include <tensorflow/lite/schema/schema_generated.h>

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

esp_err_t TfliteMicroProvider::infer(const FrameView& /*frame*/,
                                     InferenceResult* out) {
    out->detections.clear();
    return ESP_ERR_NOT_SUPPORTED;
}

void TfliteMicroProvider::stop() { ready_ = false; }

const char* TfliteMicroProvider::providerId() const {
    return kProviderTflite;
}

// ---- Model management ------------------------------------------------------

esp_err_t TfliteMicroProvider::modelUploadStage(const char* model_id,
                                                const uint8_t* data,
                                                size_t len) {
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

    // TODO: Parse full flatbuffer schema, validate operators,
    //       input/output tensors, quantization, arena requirement.
    //       Requires the TFLM library headers.

    // Populate manifest with what we can determine:
    manifest->model_id = model_id;
    manifest->format = "tflite_micro";
    manifest->file = path;
    manifest->required_tensor_arena_bytes = 0; // unknown without TFLM lib

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

    return ESP_OK;
}

}  // namespace visioncam
