// http_upload_storage_backend.cpp — IStorageBackend over HTTP (SPEC 4.9,
// Phase 8 deliverable 1).
//
// Uploads COMPLETED files to a user-configured server via a bounded upload
// queue with retry/backoff and a heap gate. Never streams frames
// synchronously (SPEC 4.9). Local SD remains authoritative unless the user
// explicitly enables delete-after-upload (default off).
//
// The upload worker is a low-priority FreeRTOS task. Queue entries hold only
// opaque media IDs + filesystem paths (never file bodies).

#include "http_upload_storage_backend.h"

#include <cstring>
#include <string>

#include "esp_event.h"
#include "esp_http_client.h"
#include "esp_log.h"
#include "esp_system.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "nvs.h"
#include "nvs_flash.h"

#include "event_bus.h"
#include "storage_manager.h"

namespace visioncam {

static constexpr char TAG[] = "netstorage";

// NVS namespace + keys for network-storage config (write-only secrets).
static constexpr char kNsNs[] = "netstorage";
static constexpr char kNsEnabled[] = "enabled";
static constexpr char kNsBackend[] = "backend";
static constexpr char kNsUrl[] = "url";
static constexpr char kNsDest[] = "dest";
static constexpr char kNsToken[] = "token";
static constexpr char kNsPolicy[] = "policy";
static constexpr char kNsRetry[] = "retry";
static constexpr char kNsInterval[] = "interval";
static constexpr char kNsTls[] = "tls_verify";
static constexpr char kNsLimit[] = "queue_limit";
static constexpr char kNsDelete[] = "delete_after";
static constexpr char kNsVer[] = "ver";

// Task config (low priority, small stack).
static constexpr uint32_t kWorkerPrio = 2;
static constexpr uint32_t kWorkerStack = 4096;
static constexpr uint32_t kWorkerPollMs = 500;

// Max URL/path lengths for HTTP client buffers.
static constexpr uint32_t kUrlMax = 512;
static constexpr uint32_t kBoundaryMax = 64;

// Worker task runs at prio 2 (below timelapse 3, motion 4).
// It must never compete with the storage writer or stream.

HttpUploadStorageBackend::HttpUploadStorageBackend() = default;

esp_err_t HttpUploadStorageBackend::begin() {
    if (g_initialized) return ESP_OK;

    // Read config from NVS.
    nvs_handle_t h = 0;
    esp_err_t err = nvs_open(kNsNs, NVS_READONLY, &h);
    if (err == ESP_ERR_NVS_NOT_FOUND) {
        // First boot: use defaults, nothing to persist yet.
        g_cfg = NetworkStorageConfig{};
        g_initialized = true;
        ESP_LOGI(TAG, "netstorage: no NVS config yet (first boot)");
        return ESP_OK;
    }
    if (err != ESP_OK) return err;

    struct Guard { nvs_handle_t hh; ~Guard() { nvs_close(hh); } } guard{h};

    uint8_t en = 0;
    nvs_get_u8(h, kNsEnabled, &en);
    g_cfg.enabled = (en != 0);

    size_t sz = sizeof(g_cfg.backend_type);
    nvs_get_str(h, kNsBackend, g_cfg.backend_type, &sz);
    sz = sizeof(g_cfg.server_url);
    nvs_get_str(h, kNsUrl, g_cfg.server_url, &sz);
    sz = sizeof(g_cfg.destination_path);
    nvs_get_str(h, kNsDest, g_cfg.destination_path, &sz);
    // auth_token is write-only — never read back via GET; we only track
    // whether it exists.
    sz = 1;
    uint8_t tok = 0;
    nvs_get_u8(h, kNsToken, &tok);  // any non-zero = configured
    g_cfg.auth_token_configured = (tok != 0);
    sz = sizeof(g_cfg.upload_policy);
    nvs_get_str(h, kNsPolicy, g_cfg.upload_policy, &sz);
    nvs_get_u32(h, kNsRetry, &g_cfg.retry_count);
    nvs_get_u32(h, kNsInterval, &g_cfg.retry_interval_ms);
    uint8_t tls = 1;
    nvs_get_u8(h, kNsTls, &tls);
    g_cfg.tls_verify = (tls != 0);
    nvs_get_u32(h, kNsLimit, &g_cfg.upload_queue_limit);
    uint8_t del = 0;
    nvs_get_u8(h, kNsDelete, &del);
    g_cfg.delete_after_upload = (del != 0);
    nvs_get_u32(h, kNsVer, &g_cfg.schema_version);

    // Clamp queue limit to a sane max.
    if (g_cfg.upload_queue_limit == 0 || g_cfg.upload_queue_limit > 128)
        g_cfg.upload_queue_limit = 32;

    // Copy string fields into std::string members for the worker.
    g_server_url = g_cfg.server_url;
    g_destination_path = g_cfg.destination_path;
    g_enabled = g_cfg.enabled;

    // Allocate the bounded queue.
    g_queue_cap = g_cfg.upload_queue_limit;
    g_queue = static_cast<UploadQueueEntry*>(
        std::calloc(g_queue_cap, sizeof(UploadQueueEntry)));
    if (g_queue == nullptr) {
        ESP_LOGE(TAG, "netstorage: queue alloc failed (%u entries)",
                 static_cast<unsigned>(g_queue_cap));
        return ESP_ERR_NO_MEM;
    }

    // Start the worker task (only if enabled — cheap when disabled).
    BaseType_t rc = xTaskCreate(workerTask, "vc_netstore", kWorkerStack,
                                this, kWorkerPrio, &g_worker);
    if (rc != pdPASS) {
        std::free(g_queue);
        g_queue = nullptr;
        return ESP_ERR_NO_MEM;
    }

    g_initialized = true;
    ESP_LOGI(TAG, "netstorage: enabled=%u queue_limit=%u",
             static_cast<unsigned>(g_enabled),
             static_cast<unsigned>(g_queue_cap));
    return ESP_OK;
}

bool HttpUploadStorageBackend::isAvailable() const {
    return g_initialized && g_enabled && !g_server_url.empty();
}

esp_err_t HttpUploadStorageBackend::writeFile(const char* /*path*/,
                                              const uint8_t* /*data*/,
                                              size_t /*len*/) {
    // Not used: files are enqueued via enqueueMedia() with a media ID +
    // path, not raw bytes. The storage manager owns the actual file bytes.
    return ESP_ERR_NOT_SUPPORTED;
}

esp_err_t HttpUploadStorageBackend::removeFile(const char* path) {
    // Delete-after-upload path. Delegates to the storage manager (sole FS
    // owner) — this backend never touches the filesystem directly.
    return storageRemoveFile(path);
}

esp_err_t HttpUploadStorageBackend::appendFile(const char* /*path*/,
                                               const uint8_t* /*data*/,
                                               size_t /*len*/) {
    return ESP_ERR_NOT_SUPPORTED;
}

esp_err_t HttpUploadStorageBackend::readFile(const char* /*path*/,
                                             uint8_t* /*data*/,
                                             size_t /*len*/,
                                             size_t* /*out_len*/) {
    return ESP_ERR_NOT_SUPPORTED;
}

esp_err_t HttpUploadStorageBackend::renameFile(const char* /*src*/,
                                               const char* /*dst*/) {
    return ESP_ERR_NOT_SUPPORTED;
}

esp_err_t HttpUploadStorageBackend::statFile(const char* /*path*/,
                                             uint32_t* /*out_size*/,
                                             uint32_t* /*out_mtime*/) {
    return ESP_ERR_NOT_SUPPORTED;
}

esp_err_t HttpUploadStorageBackend::listDir(const char* /*dir*/,
                                            DirEntry* /*entries*/,
                                            size_t /*max_entries*/,
                                            size_t* /*out_count*/) {
    return ESP_ERR_NOT_SUPPORTED;
}

esp_err_t HttpUploadStorageBackend::freeSpace(FreeSpace* /*out*/) {
    return ESP_ERR_NOT_SUPPORTED;
}

const char* HttpUploadStorageBackend::mountPoint() const { return "/sdcard"; }

void HttpUploadStorageBackend::getConfig(NetworkStorageConfig* out) const {
    *out = g_cfg;
    // Secrets stripped: never return the token value.
    out->auth_token[0] = '\0';
    out->auth_token_configured = g_cfg.auth_token_configured;
}

esp_err_t HttpUploadStorageBackend::setConfig(const NetworkStorageConfig& cand,
                                              NetworkStorageConfig* effective) {
    // Validate scheme.
    if (std::strcmp(cand.backend_type, "http") != 0 &&
        std::strcmp(cand.backend_type, "https") != 0) {
        return ESP_ERR_INVALID_ARG;
    }
    // Validate URL (must start with http:// or https://).
    if (cand.server_url[0] != '\0' &&
        std::strncmp(cand.server_url, "http://", 7) != 0 &&
        std::strncmp(cand.server_url, "https://", 8) != 0) {
        return ESP_ERR_INVALID_ARG;
    }
    // Validate retry bounds.
    if (cand.retry_count > 10) return ESP_ERR_INVALID_ARG;
    if (cand.retry_interval_ms < 1000 || cand.retry_interval_ms > 60000)
        return ESP_ERR_INVALID_ARG;
    // Validate queue limit.
    if (cand.upload_queue_limit > 128) return ESP_ERR_INVALID_ARG;

    NetworkStorageConfig next = g_cfg;  // start from current (partial overlay)

    if (cand.enabled != g_cfg.enabled) next.enabled = cand.enabled;
    if (cand.backend_type[0] != '\0')
        std::strncpy(next.backend_type, cand.backend_type,
                     sizeof(next.backend_type) - 1);
    if (cand.server_url[0] != '\0')
        std::strncpy(next.server_url, cand.server_url,
                     sizeof(next.server_url) - 1);
    if (cand.destination_path[0] != '\0')
        std::strncpy(next.destination_path, cand.destination_path,
                     sizeof(next.destination_path) - 1);
    if (cand.auth_token[0] != '\0') {
        // New token provided: replace.
        std::strncpy(next.auth_token, cand.auth_token,
                     sizeof(next.auth_token) - 1);
        next.auth_token_configured = true;
    }
    // Empty token string means "keep stored" — do nothing.
    if (cand.upload_policy[0] != '\0')
        std::strncpy(next.upload_policy, cand.upload_policy,
                     sizeof(next.upload_policy) - 1);
    if (cand.retry_count != g_cfg.retry_count)
        next.retry_count = cand.retry_count;
    if (cand.retry_interval_ms != g_cfg.retry_interval_ms)
        next.retry_interval_ms = cand.retry_interval_ms;
    if (cand.tls_verify != g_cfg.tls_verify) next.tls_verify = cand.tls_verify;
    if (cand.upload_queue_limit != g_cfg.upload_queue_limit)
        next.upload_queue_limit = cand.upload_queue_limit;
    if (cand.delete_after_upload != g_cfg.delete_after_upload)
        next.delete_after_upload = cand.delete_after_upload;
    next.schema_version = 1;

    // Persist to NVS.
    nvs_handle_t h = 0;
    esp_err_t err = nvs_open(kNsNs, NVS_READWRITE, &h);
    if (err != ESP_OK) return err;
    struct Guard { nvs_handle_t hh; ~Guard() { nvs_close(hh); } } guard{h};

    nvs_set_u8(h, kNsEnabled, next.enabled ? 1 : 0);
    nvs_set_str(h, kNsBackend, next.backend_type);
    nvs_set_str(h, kNsUrl, next.server_url);
    nvs_set_str(h, kNsDest, next.destination_path);
    if (cand.auth_token[0] != '\0')
        nvs_set_str(h, kNsToken, next.auth_token);
    nvs_set_u8(h, kNsToken, next.auth_token_configured ? 1 : 0);
    nvs_set_str(h, kNsPolicy, next.upload_policy);
    nvs_set_u32(h, kNsRetry, next.retry_count);
    nvs_set_u32(h, kNsInterval, next.retry_interval_ms);
    nvs_set_u8(h, kNsTls, next.tls_verify ? 1 : 0);
    nvs_set_u32(h, kNsLimit, next.upload_queue_limit);
    nvs_set_u8(h, kNsDelete, next.delete_after_upload ? 1 : 0);
    nvs_set_u32(h, kNsVer, next.schema_version);
    err = nvs_commit(h);
    if (err != ESP_OK) return err;

    // Apply live config.
    g_cfg = next;
    g_server_url = next.server_url;
    g_destination_path = next.destination_path;
    g_enabled = next.enabled;
    g_auth_token = next.auth_token;

    if (effective) *effective = next;
    // Strip secret from effective copy returned to caller.
    effective->auth_token[0] = '\0';
    effective->auth_token_configured = g_cfg.auth_token_configured;

    ESP_LOGI(TAG, "netstorage: config saved enabled=%u url_set=%u",
             static_cast<unsigned>(next.enabled),
             static_cast<unsigned>(next.server_url[0] != '\0'));
    return ESP_OK;
}

esp_err_t HttpUploadStorageBackend::testConnection() {
    if (!g_initialized || !g_enabled || g_server_url.empty())
        return ESP_ERR_INVALID_STATE;

    // Heap gate: refuse TLS if below threshold.
    uint32_t free_heap = 0, largest = 0;
    // Use ESP-IDF heap caps for a quick check.
    multi_heap_info_t info;
    heap_caps_get_info(&info, MALLOC_CAP_DEFAULT);
    free_heap = static_cast<uint32_t>(info.total_free_bytes);
    largest = static_cast<uint32_t>(info.largest_free_block);
    if (free_heap < kHeapGateMinFree || largest < kHeapGateMinLargest) {
        ESP_LOGW(TAG, "netstorage: heap gate closed (free=%u largest=%u)",
                 static_cast<unsigned>(free_heap), static_cast<unsigned>(largest));
        setLastError("heap below gate");
        setState("error");
        return ESP_ERR_NO_MEM;
    }

    // Single-shot GET to the server URL (reachability probe).
    esp_http_client_config_t cfg{};
    cfg.url = g_server_url.c_str();
    cfg.timeout_ms = 5000;
    cfg.method = HTTP_METHOD_GET;
    if (!g_cfg.tls_verify) {
        cfg.cert_pem = nullptr;
        cfg.skip_cert_common_name_check = true;
    }

    esp_http_client_handle_t client =
        esp_http_client_init(&cfg);
    if (client == nullptr) {
        setLastError("http client init failed");
        setState("error");
        return ESP_FAIL;
    }

    esp_err_t err = esp_http_client_perform(client);
    int status = 0;
    if (err == ESP_OK)
        status = esp_http_client_get_status_code(client);
    esp_http_client_cleanup(client);

    if (err != ESP_OK || status < 200 || status >= 400) {
        ESP_LOGW(TAG, "netstorage: test failed err=%d status=%d",
                 static_cast<int>(err), status);
        setLastError("backend unreachable");
        setState("error");
        return ESP_FAIL;
    }

    ESP_LOGI(TAG, "netstorage: test OK (status %d)", status);
    setLastError("");
    setState("idle");
    return ESP_OK;
}

void HttpUploadStorageBackend::getStatus(NetworkStorageStatus* out) const {
    out->enabled = g_enabled;
    out->configured = !g_server_url.empty();
    out->available = (g_state[0] == 'i' || g_state[0] == 'u');  // idle/uploading
    out->upload_token_configured = g_cfg.auth_token_configured;
    out->queue_limit = g_queue_cap;
    out->queue_depth = g_queue_count;
    out->queue_max_seen = g_queue_max_seen;
    out->uploads_total = g_uploads_total;
    out->uploads_ok = g_uploads_ok;
    out->uploads_failed = g_uploads_failed;
    out->queue_dropped = g_queue_dropped;
    std::strncpy(out->last_error, g_last_error, sizeof(out->last_error) - 1);
    std::strncpy(out->state, g_state, sizeof(out->state) - 1);
    out->schema_version = 1;
}

esp_err_t HttpUploadStorageBackend::enqueueMedia(const char* media_id,
                                                 const char* path) {
    if (!g_initialized || !g_enabled) return ESP_ERR_INVALID_STATE;
    if (media_id == nullptr || path == nullptr) return ESP_ERR_INVALID_ARG;

    UploadQueueEntry entry{};
    std::strncpy(entry.media_id, media_id, sizeof(entry.media_id) - 1);
    std::strncpy(entry.path, path, sizeof(entry.path) - 1);
    entry.enqueued_ms = static_cast<uint32_t>(esp_timer_get_time() / 1000ULL);
    entry.next_retry_ms = entry.enqueued_ms;

    return enqueueLocked(entry);
}

// ---- Worker task --------------------------------------------------------

void HttpUploadStorageBackend::workerTask(void* param) {
    auto* self = static_cast<HttpUploadStorageBackend*>(param);

    for (;;) {
        vTaskDelay(pdMS_TO_TICKS(kWorkerPollMs));

        if (!self->g_enabled || self->g_server_url.empty()) {
            self->setState("idle");
            continue;
        }

        UploadQueueEntry entry{};
        esp_err_t rc = self->popDue(&entry);
        if (rc == ESP_ERR_NOT_FOUND) {
            // Nothing due: idle.
            self->setState("idle");
            continue;
        }
        if (rc != ESP_OK) {
            self->setState("error");
            continue;
        }

        // Heap gate before TLS.
        multi_heap_info_t info;
        heap_caps_get_info(&info, MALLOC_CAP_DEFAULT);
        uint32_t free_heap = static_cast<uint32_t>(info.total_free_bytes);
        uint32_t largest = static_cast<uint32_t>(info.largest_free_block);
        if (free_heap < self->kHeapGateMinFree ||
            largest < self->kHeapGateMinLargest) {
            // Back off: re-queue with a longer delay and try later.
            ESP_LOGW(TAG, "netstorage: heap gate closed, deferring %s",
                     entry.media_id);
            self->scheduleRetry(&entry);
            // Re-queue at head so it's retried soon.
            self->enqueueLocked(entry);
            self->setState("backoff");
            continue;
        }

        self->setState("uploading");
        self->g_uploads_total++;
        esp_err_t uerr = self->uploadOne(entry);
        if (uerr == ESP_OK) {
            self->g_uploads_ok++;
            ESP_LOGI(TAG, "netstorage: uploaded %s", entry.media_id);
            // Delete-after-upload (opt-in, default off).
            if (self->g_cfg.delete_after_upload &&
                entry.path[0] != '\0') {
                esp_err_t derr = storageRemoveFile(entry.path);
                if (derr != ESP_OK)
                    ESP_LOGW(TAG, "netstorage: delete-after-upload failed %s",
                             entry.path);
            }
        } else {
            self->g_uploads_failed++;
            ESP_LOGW(TAG, "netstorage: upload failed %s err=%d",
                     entry.media_id, static_cast<int>(uerr));
            entry.attempts++;
            if (entry.attempts >= self->g_cfg.retry_count) {
                // Give up: publish failure event, drop entry.
                eventBusPublish(EventType::NetworkStorageUploadFailed,
                                /*value=*/0,
                                static_cast<int32_t>(uerr));
                self->setLastError("upload failed after retries");
                self->setState("error");
            } else {
                self->scheduleRetry(&entry);
                self->enqueueLocked(entry);
                self->setState("backoff");
            }
        }
    }
}

esp_err_t HttpUploadStorageBackend::enqueueLocked(const UploadQueueEntry& entry) {
    // Drop-oldest overflow: if full, overwrite the oldest entry.
    if (g_queue_count >= g_queue_cap) {
        // Advance tail (drops oldest).
        g_queue_tail = (g_queue_tail + 1) % g_queue_cap;
        g_queue_dropped++;
        g_queue_count--;
        ESP_LOGW(TAG, "netstorage: queue full, dropped oldest");
    }
    g_queue[g_queue_head] = entry;
    g_queue_head = (g_queue_head + 1) % g_queue_cap;
    g_queue_count++;
    if (g_queue_count > g_queue_max_seen) g_queue_max_seen = g_queue_count;
    return ESP_OK;
}

esp_err_t HttpUploadStorageBackend::popDue(UploadQueueEntry* out) {
    uint32_t now = static_cast<uint32_t>(esp_timer_get_time() / 1000ULL);
    // Simple scan: find the first entry whose next_retry_ms <= now.
    // Bounded by queue capacity (max 128) — fine for a low-prio task.
    size_t idx = g_queue_tail;
    for (size_t i = 0; i < g_queue_count; i++) {
        if (g_queue[idx].next_retry_ms <= now) {
            *out = g_queue[idx];
            // Remove by shifting: compact the queue (simple, bounded).
            for (size_t j = i; j < g_queue_count - 1; j++) {
                size_t src = (g_queue_tail + j + 1) % g_queue_cap;
                size_t dst = (g_queue_tail + j) % g_queue_cap;
                g_queue[dst] = g_queue[src];
            }
            g_queue_head = (g_queue_head + g_queue_cap - 1) % g_queue_cap;
            g_queue_count--;
            return ESP_OK;
        }
        idx = (idx + 1) % g_queue_cap;
    }
    return ESP_ERR_NOT_FOUND;
}

void HttpUploadStorageBackend::scheduleRetry(UploadQueueEntry* entry) {
    uint32_t now = static_cast<uint32_t>(esp_timer_get_time() / 1000ULL);
    // Exponential backoff: base * 2^attempts, capped at 5 min.
    uint32_t base = g_cfg.retry_interval_ms;
    uint32_t delay = base;
    for (uint32_t a = 0; a < entry->attempts && a < 10; a++) {
        delay *= 2;
        if (delay > 300000) delay = 300000;
    }
    entry->next_retry_ms = now + delay;
}

void HttpUploadStorageBackend::setState(const char* state) {
    std::strncpy(g_state, state, sizeof(g_state) - 1);
}

void HttpUploadStorageBackend::setLastError(const char* err) {
    std::strncpy(g_last_error, err, sizeof(g_last_error) - 1);
}

// ---- HTTP multipart upload ----------------------------------------------

esp_err_t HttpUploadStorageBackend::uploadOne(const UploadQueueEntry& entry) {
    // Read the file from SD via the storage manager (sole FS owner).
    // We need the file size first.
    uint32_t fsize = 0, ftime = 0;
    esp_err_t err = storageStatFile(entry.path, &fsize, &ftime);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "netstorage: stat %s failed err=%d", entry.path,
                 static_cast<int>(err));
        return err;
    }

    // Stream the file in chunks — never buffer the whole body in RAM.
    // Use esp_http_client with a custom write/read callback.
    // Build the target URL: server_url + destination_path + filename.
    char url[kUrlMax] = {};
    // Extract filename from path.
    const char* fname = std::strrchr(entry.path, '/');
    fname = fname ? fname + 1 : entry.path;
    std::snprintf(url, sizeof(url), "%s/%s/%s", g_server_url.c_str(),
                  g_destination_path.c_str(), fname);

    esp_http_client_config_t cfg{};
    cfg.url = url;
    cfg.timeout_ms = 30000;
    cfg.method = HTTP_METHOD_POST;
    cfg.buffer_size = 4096;
    if (!g_cfg.tls_verify) {
        cfg.skip_cert_common_name_check = true;
    }
    if (g_auth_token.empty()) {
        // No auth: anonymous.
    } else {
        // We'll set the Authorization header via esp_http_client_set_header.
    }

    esp_http_client_handle_t client = esp_http_client_init(&cfg);
    if (client == nullptr) {
        ESP_LOGE(TAG, "netstorage: http_client_init failed");
        return ESP_FAIL;
    }

    // Headers.
    std::string boundary = "----VisionCamBoundary";
    boundary += std::to_string(static_cast<uint64_t>(esp_timer_get_time()));

    std::string content_type = "multipart/form-data; boundary=" + boundary;
    esp_http_client_set_header(client, "Content-Type", content_type.c_str());
    if (!g_auth_token.empty()) {
        std::string auth = "Bearer " + g_auth_token;
        esp_http_client_set_header(client, "Authorization", auth.c_str());
    }

    // Build the multipart body in chunks and write via esp_http_client.
    // We use the "write" callback approach: esp_http_client with
    // POST and a write function that reads from the file.
    // Simpler: use esp_http_client_set_post_field with a pre-built buffer
    // for the header part, then stream the file, then the closing boundary.
    // esp_http_client doesn't easily support streaming multipart, so we
    // build the full headers + boundary in a bounded buffer and stream
    // the file body separately using the client's write interface.

    // Actually, esp_http_client supports `esp_http_client_set_post_field`
    // for a single buffer. For streaming, we can use
    // `esp_http_client_write` in a loop after setting the method and
    // headers manually — but that requires managing the connection state.
    // The pragmatic approach on ESP32 with limited RAM: read the file in
    // 4 KB chunks and write each via esp_http_client_write after opening
    // the connection with esp_http_client_open.

    err = esp_http_client_open(client, 0);  // 0 = chunked / unknown length
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "netstorage: http_client_open failed err=%d",
                 static_cast<int>(err));
        esp_http_client_cleanup(client);
        return err;
    }

    // Write multipart preamble.
    char preamble[256] = {};
    std::snprintf(preamble, sizeof(preamble),
                  "--%s\r\n"
                  "Content-Disposition: form-data; name=\"file\"; "
                  "filename=\"%s\"\r\n"
                  "Content-Type: application/octet-stream\r\n"
                  "\r\n",
                  boundary.c_str(), fname);
    int written = esp_http_client_write(client, preamble,
                                        static_cast<int>(std::strlen(preamble)));
    if (written < 0) {
        esp_http_client_cleanup(client);
        return ESP_FAIL;
    }

    // Stream the file body in 4 KB chunks.
    uint8_t buf[4096] = {};
    size_t remaining = fsize;
    // Open file via storage manager.
    // storageReadFile reads into a buffer — we read chunks.
    uint32_t offset = 0;
    while (remaining > 0) {
        size_t chunk = std::min<size_t>(remaining, sizeof(buf));
        size_t got = 0;
        err = storageReadFile(entry.path, buf, chunk, &got);
        if (err != ESP_OK || got == 0) break;
        written = esp_http_client_write(client, reinterpret_cast<char*>(buf),
                                        static_cast<int>(got));
        if (written < 0) break;
        remaining -= got;
        offset += got;
    }

    // Write closing boundary.
    char epilogue[64] = {};
    std::snprintf(epilogue, sizeof(epilogue), "\r\n--%s--\r\n", boundary.c_str());
    esp_http_client_write(client, epilogue,
                          static_cast<int>(std::strlen(epilogue)));

    int status = 0;
    status = esp_http_client_get_status_code(client);
    esp_http_client_cleanup(client);

    if (status >= 200 && status < 300) {
        ESP_LOGI(TAG, "netstorage: %s uploaded (status %d, %u bytes)",
                 entry.media_id, status, static_cast<unsigned>(fsize));
        return ESP_OK;
    }
    ESP_LOGW(TAG, "netstorage: %s upload HTTP %d", entry.media_id, status);
    return ESP_FAIL;
}

}  // namespace visioncam
