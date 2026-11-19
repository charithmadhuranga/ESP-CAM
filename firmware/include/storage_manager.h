// storage_manager.h — StorageManager interface (plan 02 task 2.1; SPEC
// sections 4.8, 4.9, 6.5, 6.7).
//
// StorageManager is the SOLE filesystem owner (AGENTS rule 10): nothing
// outside storage/ calls fopen/fwrite/fclose/remove/rename/mkdir. All file
// I/O goes through IStorageBackend implementations.
//
// Public API uses opaque media IDs (med-…, rec-…, evt-…), never filesystem
// paths (AGENTS rule 15, SPEC 6.0). Path traversal is rejected.
//
// Directory layout (SPEC §4.8):
//   /sdcard/system/          device.json, config-backup.json, index/
//   /sdcard/system/index/    media-index.jsonl, event-index.jsonl
//   /sdcard/snapshots/YYYY/MM/DD/{manual,motion,detection,timelapse}/
//   /sdcard/recordings/{continuous,events,manual}/YYYY/MM/DD/
//   /sdcard/timelapse/{project_id}/{frames,output,project.json}
//   /sdcard/thumbnails/
//   /sdcard/models/{staging,active,backup}/
//   /sdcard/logs/
//
// Index design (SPEC §4.8):
//   Append-only JSON Lines; validate each line independently; ignore
//   malformed trailing records; rebuild by directory scan; atomic
//   temp-file replacement for compaction; never load whole library into
//   RAM; paginate; filesystem is source of truth, indexes are caches.

#pragma once

#include <cstddef>
#include <cstdint>
#include <functional>
#include <string>
#include <vector>

#include "esp_err.h"

#include "storage_backend.h"

namespace visioncam {

// Snapshot source (SPEC 4.5).
enum class SnapshotSource : uint8_t {
    Manual = 0,
    Motion,
    Detection,
    Timelapse,
};

// Media index record (SPEC §4.8 example, schema_version=1).
struct MediaRecord {
    char media_id[24];       // "med-XXXXXXXX" zero-padded
    MediaType type = MediaType::Snapshot;
    MediaCategory category = MediaCategory::Manual;
    char path[128];          // filesystem path (internal, never exposed)
    char mime[16] = {};
    uint32_t size = 0;       // bytes
    char created_at[32] = {}; // ISO-8601
    char event_id[24] = {};  // "evt-XXXXXXXX" or empty
    uint32_t duration_ms = 0;
    char status[16] = "ready"; // "ready" | "writing" | "recovering" | "deleted"
};

// Event index record (SPEC §4.8 example, schema_version=1).
struct EventRecord {
    char event_id[24];       // "evt-XXXXXXXX" zero-padded
    char event_type[16] = {}; // "motion" | "detection" | …
    char created_at[32] = {};
    char ended_at[32] = {};
    char snapshot_id[24] = {}; // "med-XXXXXXXX" or empty
    char recording_id[24] = {}; // "rec-XXXXXXXX" or empty
    char notification_status[16] = "queued";
};

// Storage state for /storage/status.
struct StorageStatus {
    bool mounted = false;
    bool index_loaded = false;
    uint64_t total_bytes = 0;
    uint64_t used_bytes = 0;
    uint64_t free_bytes = 0;
    uint32_t media_count = 0;
    uint32_t event_count = 0;
    // Per-category counts (index 0..MediaCategory max).
    uint32_t category_count[4] = {};
};

// Retention policy (SPEC §4.8 retention manager).
struct RetentionPolicy {
    uint32_t retention_days_manual = 30;
    uint32_t retention_days_motion = 7;
    uint32_t retention_days_detection = 30;
    uint32_t retention_days_timelapse = 90;
    uint32_t max_used_percent = 85;   // 0 = disabled
    bool enabled = true;
};

// Deletion candidate (preview).
struct DeletionCandidate {
    char media_id[24];
    MediaCategory category = MediaCategory::Manual;
    char path[128];
    uint32_t size = 0;
    uint32_t age_days = 0;
};

// Allocate a new opaque media ID. Returns ESP_ERR_NO_MEM if index is full.
// caller provides buffer of at least 24 bytes.
esp_err_t storageAllocateMediaId(MediaType type, char* out_id,
                                 size_t out_cap);

// Allocate a new opaque event ID.
esp_err_t storageAllocateEventId(char* out_id, size_t out_cap);

// Record a snapshot in the media index. If media_id is non-empty it is used
// as-is (caller must have allocated it); otherwise a new ID is allocated.
// Returns the record (with media_id filled in) on success.
esp_err_t storageRecordSnapshot(const char* media_id,
                                MediaCategory category,
                                const char* event_id,
                                const char* path,
                                uint32_t size,
                                MediaRecord* out_record);

// Record an event in the event index.
esp_err_t storageRecordEvent(const char* event_type,
                             const char* snapshot_id,
                             const char* recording_id,
                             EventRecord* out_record);

// Lookup media by ID. Returns ESP_ERR_NOT_FOUND if unknown.
esp_err_t storageLookupMedia(const char* media_id, MediaRecord* out);

// Lookup event by ID.
esp_err_t storageLookupEvent(const char* event_id, EventRecord* out);

// Paginated media list. category_filter = MediaCategory::Manual etc.
// pass category_filter = MediaCategory(-1) for all.
// Returns count filled; *out_has_more set when more entries remain.
esp_err_t storageListMedia(MediaCategory category_filter,
                           uint32_t offset, uint32_t limit,
                           MediaRecord* out, size_t out_cap,
                           size_t* out_count, bool* out_has_more);

// Paginated event list.
esp_err_t storageListEvents(uint32_t offset, uint32_t limit,
                            EventRecord* out, size_t out_cap,
                            size_t* out_count, bool* out_has_more);

// Get storage status (mount, capacity, counts).
esp_err_t storageGetStatus(StorageStatus* out);

// Get retention policy.
esp_err_t storageGetRetention(RetentionPolicy* out);

// Set retention policy.
esp_err_t storageSetRetention(const RetentionPolicy* pol);

// Preview deletion candidates (what retention would delete).
// Returns count filled; max_candidates bounds output.
esp_err_t storagePreviewDeletions(DeletionCandidate* out,
                                  size_t max_candidates,
                                  size_t* out_count);

// Execute retention (delete oldest eligible until under max_used_percent
// or retention_days exceeded). Returns number deleted.
esp_err_t storageRunRetention(uint32_t* out_deleted);

// Rebuild indexes from directory scan.
esp_err_t storageRebuildIndex();

// Delete a media item by ID (and its file). Returns ESP_ERR_NOT_FOUND if
// unknown, ESP_ERR_INVALID_STATE if the file is "open"/in use.
esp_err_t storageDeleteMedia(const char* media_id);

// Check whether a path is under /sdcard (anti-traversal).
bool storagePathIsUnderSd(const char* path);

// Resolve a media ID to its filesystem path (internal use only).
esp_err_t storageResolvePath(const char* media_id, char* out_path,
                             size_t out_cap);

// Write bytes to a file via the storage backend (temp + rename).
// Creates parent directories as needed. Path must be under /sdcard.
esp_err_t storageWriteFile(const char* path, const uint8_t* data,
                           size_t len);

// Read bytes from a file via the storage backend. Path must be under /sdcard.
esp_err_t storageReadFile(const char* path, uint8_t* data, size_t len,
                          size_t* out_len);

// Rename a file (temp -> final). Paths must be under /sdcard.
esp_err_t storageRenameFile(const char* src, const char* dst);

// Remove a file. Path must be under /sdcard.
esp_err_t storageRemoveFile(const char* path);

// Initialise the storage manager with a backend. Call once after
// coreInit() and after the SD card is mounted.
esp_err_t storageManagerInit(IStorageBackend* backend);

// True after successful init.
bool storageManagerAvailable();

// Mount point (e.g. "/sdcard").
const char* storageMountPoint();

// Boot-time self-test of the JSONL index parsing (validation rejects torn,
// garbled, and ID-less records). Runs once at init; result is logged, never
// blocks boot. Used to prove acceptance item "corrupt trailing JSONL entry
// is ignored on load" on hardware.
bool storageSelfTestIndexParsing();

}  // namespace visioncam