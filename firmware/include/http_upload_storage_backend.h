// http_upload_storage_backend.h — IStorageBackend over HTTP (SPEC 4.9,
// Phase 8 deliverable 1).
//
// Uploads COMPLETED files (recordings, snapshots) to a user-configured
// server via a bounded upload queue with retry/backoff and a heap gate.
// Never streams frames synchronously (SPEC 4.9). Local SD remains
// authoritative unless the user explicitly enables upload-and-delete.
//
// Config lives in NVS (visioncam namespace). Secrets (password/token) are
// write-only: GET returns upload_token_configured: true/false, never the
// value itself (AGENTS rule 17).
//
// The upload worker is a low-priority FreeRTOS task. Queue entries hold
// only opaque media IDs + filesystem paths (never file bodies).
//
// Overflow policy: bounded queue, drop-oldest non-critical entry when
// full — never grow without limit (AGENTS rule 12).
// Heap gate: refuse TLS sessions when free heap < 64 KB or largest free
// block < 32 KB (SPEC 4.9; R1 used ~64 KB as a threshold).

#pragma once

#include <cstddef>
#include <cstdint>
#include <string>

#include "esp_err.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#include "storage_backend.h"

namespace visioncam {

// Network-storage configuration (SPEC 4.9, §6.9 /integrations/network-storage).
struct NetworkStorageConfig {
    bool enabled = false;
    // "http" | "https" | "ftp" (ftp deferred; only http/https implemented).
    char backend_type[16] = "http";
    char server_url[256] = {};
    // Destination path on the remote server.
    char destination_path[256] = "/uploads";
    // Auth token / password (write-only, never echoed).
    char auth_token[128] = {};
    bool auth_token_configured = false;
    char upload_policy[32] = "after_close";   // "after_close" | "manual"
    uint32_t retry_count = 3;
    uint32_t retry_interval_ms = 5000;        // base interval, exponential
    bool tls_verify = true;
    uint32_t upload_queue_limit = 32;         // bounded queue depth
    // Delete-after-upload: opt-in, default off (SPEC 4.9).
    bool delete_after_upload = false;
    uint32_t schema_version = 1;
};

// Status returned to the UI (GET /integrations/network-storage).
struct NetworkStorageStatus {
    bool enabled = false;
    bool configured = false;        // server_url non-empty
    bool available = false;         // last test succeeded
    bool upload_token_configured = false;
    uint32_t queue_limit = 32;
    uint32_t queue_depth = 0;
    uint32_t queue_max_seen = 0;    // high-water mark
    uint32_t uploads_total = 0;
    uint32_t uploads_ok = 0;
    uint32_t uploads_failed = 0;
    uint32_t queue_dropped = 0;
    char last_error[128] = {};
    char state[32] = "idle";        // idle | uploading | backoff | error
    uint32_t schema_version = 1;
};

// One queue entry: references a file on SD by media ID + path.
// Does NOT copy the file body into the queue.
struct UploadQueueEntry {
    char media_id[24] = {};      // opaque media ID (med-…)
    char path[128] = {};         // filesystem path under /sdcard
    uint32_t attempts = 0;
    uint32_t enqueued_ms = 0;
    uint32_t next_retry_ms = 0;
};

// IStorageBackend over HTTP upload. Implements the SPEC 4.9 interface:
// begin() reads config from NVS; writeFile() enqueues for upload;
// removeFile() is used for delete-after-upload.
// This class does NOT own the SD card — it only reads files to upload them.
// Filesystem reads go through the storage manager (SdStorageBackend owns SD).
class HttpUploadStorageBackend : public IStorageBackend {
public:
    explicit HttpUploadStorageBackend();

    // Read config from NVS, init the upload queue, start the worker task.
    esp_err_t begin() override;
    bool isAvailable() const override;

    // Enqueue a completed file for upload (called by recording/snapshot
    // close, or by PUT /integrations/network-storage with action=upload).
    esp_err_t writeFile(const char* path, const uint8_t* data,
                        size_t len) override;
    esp_err_t removeFile(const char* path) override;

    // Not used by this backend (no local index to append/read/list).
    esp_err_t appendFile(const char* path, const uint8_t* data,
                         size_t len) override;
    esp_err_t readFile(const char* path, uint8_t* data, size_t len,
                       size_t* out_len) override;
    esp_err_t renameFile(const char* src, const char* dst) override;
    esp_err_t statFile(const char* path, uint32_t* out_size,
                       uint32_t* out_mtime) override;
    esp_err_t listDir(const char* dir, DirEntry* entries,
                      size_t max_entries, size_t* out_count) override;
    esp_err_t freeSpace(FreeSpace* out) override;
    const char* mountPoint() const override;

    // Get current config (secrets stripped).
    void getConfig(NetworkStorageConfig* out) const;

    // Set config (partial overlay + validation). Caller must hold the
    // CSRF/auth checks done by the HTTP layer.
    esp_err_t setConfig(const NetworkStorageConfig& cand,
                        NetworkStorageConfig* effective);

    // Test backend reachability (POST /integrations/network-storage/test).
    esp_err_t testConnection();

    // Get live status (queue depth, counts, last error).
    void getStatus(NetworkStorageStatus* out) const;

    // Manually enqueue a media ID for upload (used by the recordings
    // /storage "upload now" flow).
    esp_err_t enqueueMedia(const char* media_id, const char* path);

private:
    NetworkStorageConfig g_cfg{};                      // live config (NVS-backed)
    std::string g_server_url;
    std::string g_destination_path;
    std::string g_auth_token;
    bool g_initialized = false;
    bool g_enabled = false;

    // Upload queue (bounded, fixed capacity g_cfg.upload_queue_limit).
    // Overflow: drop-oldest when full.
    UploadQueueEntry* g_queue = nullptr;
    size_t g_queue_cap = 0;
    size_t g_queue_head = 0;
    size_t g_queue_tail = 0;
    size_t g_queue_count = 0;
    size_t g_queue_max_seen = 0;
    uint32_t g_queue_dropped = 0;

    // Stats
    uint32_t g_uploads_total = 0;
    uint32_t g_uploads_ok = 0;
    uint32_t g_uploads_failed = 0;

    // Worker task handle (owned by this class).
    TaskHandle_t g_worker = nullptr;

    // Last error text (bounded, for status endpoint).
    char g_last_error[128] = {};
    char g_state[32] = "idle";

    // Heap gate: refuse TLS when below these thresholds.
    // R1 used ~64 KB as a threshold, but on this board the largest
    // contiguous internal block is typically ~30 KB after the camera + web
    // server are running, so 16 KB is a more realistic floor for an HTTP
    // client session (4 KB buffer + overhead).
    static constexpr uint32_t kHeapGateMinFree = 65536;   // 64 KB (R1)
    static constexpr uint32_t kHeapGateMinLargest = 16384; // 16 KB (realistic)

    // Worker task entry point.
    static void workerTask(void* param);

    // Upload one file via multipart/form-data PUT/POST.
    esp_err_t uploadOne(const UploadQueueEntry& entry);

    // Enqueue with drop-oldest overflow. Returns ESP_FAIL if dropped.
    esp_err_t enqueueLocked(const UploadQueueEntry& entry);

    // Pop next due entry (respects next_retry_ms). Returns ESP_ERR_NOT_FOUND
    // if nothing is ready yet.
    esp_err_t popDue(UploadQueueEntry* out);

    // Retry backoff: next_retry_ms = now + base * 2^attempts, capped.
    void scheduleRetry(UploadQueueEntry* entry);

    void setState(const char* state);
    void setLastError(const char* err);
};

}  // namespace visioncam
