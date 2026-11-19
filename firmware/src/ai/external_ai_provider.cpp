// external_ai_provider.cpp — External AI inference provider implementation.
// Sends JPEG frames via HTTP POST multipart/form-data to an external AI
// inference server at /api/v1/infer.

#include "external_ai_provider.h"

#include <cstdio>
#include <cstring>

#include "esp_http_client.h"
#include "esp_log.h"

#include "ai_provider.h"

namespace visioncam {

static const char* TAG = "vc_ext_ai";

// Boundary string for multipart/form-data.
static const char* kMultipartBoundary = "VisionCamAIBoundary12345";

esp_err_t ExternalAiProvider::begin() {
    if (base_url_.empty()) {
        ESP_LOGW(TAG, "External AI: no base URL configured");
        ready_ = false;
        return ESP_ERR_INVALID_STATE;
    }
    ready_ = true;
    ESP_LOGI(TAG, "External AI provider ready: %s", base_url_.c_str());
    return ESP_OK;
}

bool ExternalAiProvider::isReady() const { return ready_; }

void ExternalAiProvider::stop() { ready_ = false; }

esp_err_t ExternalAiProvider::setConfig(const AIConfig& config) {
    base_url_ = config.base_url;
    auth_token_ = config.auth_token;
    timeout_ms_ = config.timeout_ms;
    upload_width_ = config.upload_width;
    upload_height_ = config.upload_height;
    upload_quality_ = config.upload_quality;
    tls_verify_ = config.tls_verify;
    return ESP_OK;
}

esp_err_t ExternalAiProvider::infer(const FrameView& frame,
                                    InferenceResult* out) {
    if (!ready_) {
        return ESP_ERR_INVALID_STATE;
    }
    if (frame.data == nullptr || frame.len == 0 || out == nullptr) {
        return ESP_ERR_INVALID_ARG;
    }
    out->model_id = "external_ai";
    out->inference_ms = 0;
    out->detections.clear();
    return httpPostJpeg(frame.data, frame.len, out);
}

esp_err_t ExternalAiProvider::httpPostJpeg(const uint8_t* jpeg_data,
                                           size_t jpeg_len,
                                           InferenceResult* out) {
    if (jpeg_data == nullptr || jpeg_len == 0 || out == nullptr) {
        return ESP_ERR_INVALID_ARG;
    }

    esp_http_client_config_t client_config = {};
    client_config.url = base_url_.c_str();
    client_config.event_handler = nullptr;
    client_config.timeout_ms = timeout_ms_;
    client_config.disable_auto_redirect = false;
    if (!tls_verify_) {
        client_config.skip_cert_common_name_check = true;
    }

    esp_http_client_handle_t client =
        esp_http_client_init(&client_config);
    if (client == nullptr) {
        ESP_LOGE(TAG, "External AI: failed to init HTTP client");
        return ESP_ERR_NO_MEM;
    }

    esp_http_client_set_method(client, HTTP_METHOD_POST);
    std::string content_type = "multipart/form-data; boundary=";
    content_type += kMultipartBoundary;
    esp_http_client_set_header(client, "Content-Type", content_type.c_str());
    if (!auth_token_.empty()) {
        std::string auth_header = "Bearer " + auth_token_;
        esp_http_client_set_header(client, "Authorization",
                                   auth_header.c_str());
    }

    // Build multipart body: image part + metadata part.
    // We build the entire body in a bounded buffer and send it in one
    // call. The body is limited by max_upload_bytes (256 KB default).
    std::string body;

    // Image part.
    body += "--";
    body += kMultipartBoundary;
    body += "\r\n";
    body += "Content-Disposition: form-data; name=\"image\"; "
            "filename=\"frame.jpg\"\r\n";
    body += "Content-Type: image/jpeg\r\n\r\n";
    body.append(reinterpret_cast<const char*>(jpeg_data), jpeg_len);
    body += "\r\n";

    // Metadata part (JSON).
    body += "--";
    body += kMultipartBoundary;
    body += "\r\n";
    body +=
        "Content-Disposition: form-data; name=\"metadata\"\r\n";
    body += "Content-Type: application/json\r\n\r\n";
    body += "{\"schema_version\":1,\"source\":\"visioncam\"}\r\n";

    // Closing boundary.
    body += "--";
    body += kMultipartBoundary;
    body += "--\r\n";

    if (body.size() > 256 * 1024) {
        ESP_LOGE(TAG, "External AI: request body too large (%zu bytes)",
                 body.size());
        esp_http_client_cleanup(client);
        return ESP_ERR_INVALID_SIZE;
    }

    esp_err_t err = esp_http_client_set_post_field(
        client, body.c_str(), body.size());
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "External AI: set_post_field failed: %s",
                 esp_err_to_name(err));
        esp_http_client_cleanup(client);
        return err;
    }

    err = esp_http_client_perform(client);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "External AI: HTTP POST failed: %s",
                 esp_err_to_name(err));
        esp_http_client_cleanup(client);
        return err;
    }

    int status_code = esp_http_client_get_status_code(client);
    if (status_code != 200) {
        ESP_LOGE(TAG, "External AI: server returned HTTP %d", status_code);
        esp_http_client_cleanup(client);
        return ESP_FAIL;
    }

    // Read response body.
    char response_buf[2048] = {};
    int read_len = esp_http_client_read(client, response_buf,
                                        sizeof(response_buf) - 1);
    esp_http_client_cleanup(client);

    if (read_len <= 0) {
        ESP_LOGE(TAG, "External AI: empty response");
        return ESP_FAIL;
    }
    response_buf[read_len] = '\0';

    // Parse minimal JSON: we look for inference_ms and detections.
    // This is a best-effort parse; a full JSON parser would be needed
    // for production but we keep it simple for now.
    out->detections.clear();
    out->model_id = "external_ai";
    out->inference_ms = 0;

    // Simple string search for "inference_ms".
    const char* ms_key = "\"inference_ms\":";
    const char* ms_pos = strstr(response_buf, ms_key);
    if (ms_pos != nullptr) {
        ms_pos += std::strlen(ms_key);
        out->inference_ms = static_cast<uint32_t>(atoi(ms_pos));
    }

    // Simple string search for detections array - just count them.
    const char* det_key = "\"detections\":";
    const char* det_pos = strstr(response_buf, det_key);
    if (det_pos != nullptr) {
        // Count opening braces after detections to estimate count.
        const char* p = det_pos + std::strlen(det_key);
        while (*p != '\0') {
            if (*p == '{') {
                out->detections.emplace_back();
                Detection& d = out->detections.back();
                // Try to parse label.
                const char* label_key = "\"label\":\"";
                const char* label_pos = strstr(p, label_key);
                if (label_pos != nullptr &&
                    label_pos < p + 200) { // limit search
                    label_pos += std::strlen(label_key);
                    const char* label_end =
                        strchr(label_pos, '"');
                    if (label_end != nullptr) {
                        size_t label_len =
                            static_cast<size_t>(
                                label_end - label_pos);
                        d.label.assign(label_pos, label_len);
                    }
                }
                // Try to parse confidence.
                const char* conf_key = "\"confidence\":";
                const char* conf_pos = strstr(p, conf_key);
                if (conf_pos != nullptr &&
                    conf_pos < p + 200) {
                    conf_pos += std::strlen(conf_key);
                    d.confidence = strtof(conf_pos, nullptr);
                }
                // Try to parse bbox.
                const char* bbox_key = "\"bbox\":";
                const char* bbox_pos = strstr(p, bbox_key);
                if (bbox_pos != nullptr &&
                    bbox_pos < p + 200) {
                    bbox_pos += std::strlen(bbox_key);
                    // Expect [x, y, w, h]
                    float vals[4] = {0, 0, 0, 0};
                    int val_count = 0;
                    const char* bp = bbox_pos;
                    while (*bp != '\0' && val_count < 4) {
                        if (*bp == '[' || *bp == ',' || *bp == ' ') {
                            bp++;
                            continue;
                        }
                        if (*bp == ']') break;
                        vals[val_count] = strtof(bp, const_cast<char**>(&bp));
                        val_count++;
                    }
                    if (val_count >= 4) {
                        d.bbox[0] = vals[0];
                        d.bbox[1] = vals[1];
                        d.bbox[2] = vals[2];
                        d.bbox[3] = vals[3];
                    }
                }
                // Skip to end of this detection object.
                int brace_depth = 1;
                while (*p != '\0' && brace_depth > 0) {
                    if (*p == '{') brace_depth++;
                    if (*p == '}') brace_depth--;
                    p++;
                }
            } else {
                p++;
            }
        }
    }

    return ESP_OK;
}

}  // namespace visioncam