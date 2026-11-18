// storage_manager.cpp — StorageManager implementation (plan 02 task 2.1;
// SPEC sections 4.8, 4.9, 6.5, 6.7).
//
// SOLE filesystem owner (AGENTS rule 10): all file I/O goes through
// IStorageBackend implementations. Nothing outside storage/ calls
// fopen/fwrite/fclose/remove/rename/mkdir.
//
// Public API uses opaque media IDs (med-…, evt-…, rec-…), never filesystem
// paths (AGENTS rule 15, SPEC 6.0).

#include "storage_manager.h"

#include <algorithm>
#include <cinttypes>
#include <cstdio>
#include <cstring>
#include <ctime>

#include "esp_err.h"
#include "esp_log.h"
#include "esp_system.h"
#include "esp_timer.h"
#include "nvs.h"

#include "ring_log.h"

namespace visioncam {
namespace {

const char* TAG = "vc_storage";

// Index file paths (relative to /sdcard mount).
constexpr char kMediaIndexPath[] = "/sdcard/system/index/media-index.jsonl";
constexpr char kEventIndexPath[] = "/sdcard/system/index/event-index.jsonl";

// Max length of a single JSONL line.
constexpr size_t kIndexLineMax = 512;

// --- retention policy persistence (SPEC 7: NVS authoritative, namespaced
// keys with explicit version — never one large JSON blob; key length in
// NVS is capped at 15 characters).
constexpr char     kRetNvsNs[]      = "storage";      // storage config namespace
constexpr char     kRetKeyVer[]     = "ret_ver";      // policy schema version (u32)
constexpr char     kRetKeyDaysMan[] = "ret_days_man"; // per-category days (u32)
constexpr char     kRetKeyDaysMot[] = "ret_days_mot";
constexpr char     kRetKeyDaysDet[] = "ret_days_det";
constexpr char     kRetKeyDaysTl[]  = "ret_days_tl";
constexpr char     kRetKeyMaxPct[]  = "ret_maxpct";   // max used percent (u32)
constexpr char     kRetKeyEnabled[] = "ret_enabled";  // policy on/off (u8)
constexpr uint32_t kRetSchemaVersion = 1;

// Range validation for every retention field (SPEC 7 step 2 "validate all
// fields and ranges"). max_used_percent 0 disables size-based retention
// (storage_manager.h), 100 accepts a full card; days are bounded so a typo
// cannot round a category down to zero retention or a century.
bool retentionValid(const RetentionPolicy& p) {
    if (p.max_used_percent > 100) return false;
    auto days_ok = [](uint32_t d) { return d >= 1 && d <= 3650; };
    return days_ok(p.retention_days_manual) &&
           days_ok(p.retention_days_motion) &&
           days_ok(p.retention_days_detection) &&
           days_ok(p.retention_days_timelapse);
}

// Format ISO-8601 timestamp from seconds since epoch.
void formatIso8601(uint32_t epoch, char* out, size_t out_cap) {
    if (epoch == 0) epoch = static_cast<uint32_t>(esp_timer_get_time() / 1000000ULL);
    std::tm tm{};
    gmtime_r(reinterpret_cast<const time_t*>(&epoch), &tm);
    std::snprintf(out, out_cap, "%04d-%02d-%02dT%02d:%02d:%02dZ",
                  tm.tm_year + 1900, tm.tm_mon + 1, tm.tm_mday,
                  tm.tm_hour, tm.tm_min, tm.tm_sec);
}

// Parse ISO-8601 to seconds since epoch (best effort).
uint32_t parseIso8601(const char* s) {
    if (s == nullptr || *s == '\0') return 0;
    std::tm tm{};
    if (std::sscanf(s, "%4d-%2d-%2dT%2d:%2d:%2d",
                    &tm.tm_year, &tm.tm_mon, &tm.tm_mday,
                    &tm.tm_hour, &tm.tm_min, &tm.tm_sec) != 6) return 0;
    tm.tm_year -= 1900;
    tm.tm_mon -= 1;
    return static_cast<uint32_t>(std::mktime(&tm));
}

// JSON string escaping: escape backslash, quote, control chars.
size_t jsonEscape(const char* in, char* out, size_t out_cap) {
    size_t o = 0;
    for (const char* p = in; *p && o + 1 < out_cap; ++p) {
        char c = *p;
        switch (c) {
        case '\\': if (o + 2 < out_cap) { out[o++] = '\\'; out[o++] = '\\'; } break;
        case '"':  if (o + 2 < out_cap) { out[o++] = '\\'; out[o++] = '"'; } break;
        case '\n': if (o + 2 < out_cap) { out[o++] = '\\'; out[o++] = 'n'; } break;
        case '\r': if (o + 2 < out_cap) { out[o++] = '\\'; out[o++] = 'r'; } break;
        case '\t': if (o + 2 < out_cap) { out[o++] = '\\'; out[o++] = 't'; } break;
        default:   if (o + 1 < out_cap) out[o++] = c; break;
        }
    }
    out[o] = '\0';
    return o;
}

// Extract a JSON string field value from a JSONL line.
void extractJsonString(const char* json, const char* key,
                       char* out, size_t out_cap) {
    out[0] = '\0';
    char search_key[64] = {};
    std::snprintf(search_key, sizeof(search_key), "\"%s\":", key);
    const char* p = std::strstr(json, search_key);
    if (p == nullptr) return;
    p += std::strlen(search_key);
    while (*p == ' ') ++p;
    if (*p != '"') return;
    ++p;
    const char* end = std::strchr(p, '"');
    if (end == nullptr) return;
    size_t n = static_cast<size_t>(end - p);
    if (n >= out_cap) n = out_cap - 1;
    std::memcpy(out, p, n);
    out[n] = '\0';
}

// Extract a JSON integer field value.
void extractJsonInt(const char* json, const char* key, int* out) {
    char search_key[64] = {};
    std::snprintf(search_key, sizeof(search_key), "\"%s\":", key);
    const char* p = std::strstr(json, search_key);
    if (p == nullptr) return;
    p += std::strlen(search_key);
    while (*p == ' ') ++p;
    *out = std::atoi(p);
}

void extractJsonUInt(const char* json, const char* key,
                     uint32_t* out) {
    int val = 0;
    extractJsonInt(json, key, &val);
    *out = static_cast<uint32_t>(val);
}

// Read entire index file into a caller buffer. Bounded by out_cap.
// Tries to read directly instead of stat-first, because stat() on the
// ESP-IDF FATFS VFS can hang for paths under /sdcard.
esp_err_t readIndexLines(IStorageBackend* backend, const char* index_path,
                         char* out_buf, size_t out_cap,
                         size_t* out_len) {
    *out_len = 0;
    // Try to read directly. If the file doesn't exist, readFile returns
    // ESP_ERR_NOT_FOUND which we treat as "empty index".
    size_t got = 0;
    const esp_err_t rerr = backend->readFile(index_path,
                                             reinterpret_cast<uint8_t*>(out_buf),
                                             out_cap - 1, &got);
    if (rerr != ESP_OK) {
        if (rerr == ESP_ERR_NOT_FOUND) {
            *out_len = 0;
            return ESP_OK; // no index yet
        }
        return rerr;
    }
    out_buf[got] = '\0';
    *out_len = got;
    return ESP_OK;
}

}  // namespace

// ========================================================================
// StorageManager implementation
// ========================================================================

class StorageManager {
public:
    explicit StorageManager(IStorageBackend* backend)
        : backend_(backend) {}

    esp_err_t init() {
        if (backend_ == nullptr) return ESP_ERR_INVALID_ARG;

        // Retention policy is NVS-authoritative (SPEC 7) and independent of
        // the SD card: load it BEFORE the mount so a missing/failing card
        // still leaves the configured policy in effect.
        const esp_err_t rerr = loadRetention();
        if (rerr != ESP_OK) {
            ESP_LOGW(TAG, "retention load: %s — using built-in defaults",
                     esp_err_to_name(rerr));
        }

        const esp_err_t err = backend_->begin();
        if (err != ESP_OK) return err;

        // Load existing indexes to find highest IDs.
        const esp_err_t lerr = loadIndexes();
        if (lerr != ESP_OK) {
            ESP_LOGW(TAG, "init: loadIndexes returned %s — rebuilding",
                     esp_err_to_name(lerr));
            return rebuildIndex();
        }
        ESP_LOGI(TAG, "init: next_media=%u next_event=%u",
                 static_cast<unsigned>(id_tracker_.next_media),
                 static_cast<unsigned>(id_tracker_.next_event));
        return ESP_OK;
    }

    bool available() const {
        return backend_ != nullptr && backend_->isAvailable();
    }

    const char* mountPoint() const {
        return backend_ ? backend_->mountPoint() : "";
    }

    // ---- ID allocation ----------------------------------------------------
    esp_err_t allocateMediaId(MediaType type, char* out_id,
                              size_t out_cap) {
        if (out_id == nullptr || out_cap < 24) return ESP_ERR_INVALID_ARG;
        const uint32_t id = id_tracker_.next_media++;
        std::snprintf(out_id, out_cap, "med-%06" PRIu32, id);
        return ESP_OK;
    }

    esp_err_t allocateEventId(char* out_id, size_t out_cap) {
        if (out_id == nullptr || out_cap < 24) return ESP_ERR_INVALID_ARG;
        const uint32_t id = id_tracker_.next_event++;
        std::snprintf(out_id, out_cap, "evt-%06" PRIu32, id);
        return ESP_OK;
    }

    // ---- Snapshot recording ----------------------------------------------
    esp_err_t recordSnapshot(MediaCategory category,
                             const char* event_id,
                             const char* path,
                             uint32_t size,
                             MediaRecord* out_record) {
        if (!available()) return ESP_ERR_INVALID_STATE;
        if (path == nullptr || out_record == nullptr) return ESP_ERR_INVALID_ARG;

        MediaRecord rec{};
        const esp_err_t iderr = allocateMediaId(MediaType::Snapshot,
                                                rec.media_id, sizeof(rec.media_id));
        if (iderr != ESP_OK) return iderr;

        const uint32_t now = static_cast<uint32_t>(esp_timer_get_time() / 1000000ULL);
        formatIso8601(now, rec.created_at, sizeof(rec.created_at));
        rec.type = MediaType::Snapshot;
        rec.category = category;
        rec.size = size;
        std::strncpy(rec.path, path, sizeof(rec.path) - 1);
        rec.path[sizeof(rec.path) - 1] = '\0';
        std::strncpy(rec.mime, "image/jpeg", sizeof(rec.mime) - 1);
        std::strncpy(rec.status, "ready", sizeof(rec.status) - 1);
        if (event_id != nullptr) {
            std::strncpy(rec.event_id, event_id, sizeof(rec.event_id) - 1);
            rec.event_id[sizeof(rec.event_id) - 1] = '\0';
        }

        // Append to media index.
        char line[kIndexLineMax] = {};
        appendMediaIndexLine(rec, line, sizeof(line));
        const esp_err_t perr = backend_->appendFile(kMediaIndexPath,
            reinterpret_cast<const uint8_t*>(line), std::strlen(line));
        if (perr != ESP_OK) {
            ESP_LOGE(TAG, "recordSnapshot: append to media index failed: %s",
                     esp_err_to_name(perr));
            return perr;
        }

        if (out_record != nullptr) *out_record = rec;
        return ESP_OK;
    }

    // ---- Lookup -----------------------------------------------------------
    esp_err_t lookupMedia(const char* media_id, MediaRecord* out) {
        // Stub: no media index exists yet (snapshots created in task 2.2).
        // Returns NOT_FOUND for any media ID.
        return ESP_ERR_NOT_FOUND;
    }

    esp_err_t lookupEvent(const char* event_id, EventRecord* out) {
        // TODO: implement proper index reading (TBD)
        return ESP_ERR_NOT_FOUND;
    }

    // ---- List -------------------------------------------------------------
    esp_err_t listMedia(MediaCategory category_filter,
                        uint32_t offset, uint32_t limit,
                        MediaRecord* out, size_t out_cap,
                        size_t* out_count, bool* out_has_more) {
        if (out == nullptr || out_count == nullptr || out_has_more == nullptr)
            return ESP_ERR_INVALID_ARG;
        // Stub: no index reading yet
        *out_count = 0;
        *out_has_more = false;
        return ESP_OK;
    }

    esp_err_t listEvents(uint32_t offset, uint32_t limit,
                         EventRecord* out, size_t out_cap,
                         size_t* out_count, bool* out_has_more) {
        if (out == nullptr || out_count == nullptr || out_has_more == nullptr)
            return ESP_ERR_INVALID_ARG;

        // Stack buffer bounded to stay within the HTTP server's 4 KB task
        // stack (SPEC: no unbounded stack allocations in handler context).
        char buf[512] = {};
        size_t len = 0;
        const esp_err_t rerr = readIndexLines(backend_, kEventIndexPath,
                                              buf, sizeof(buf), &len);
        if (rerr != ESP_OK) return rerr;

        size_t pos = 0;
        size_t total = 0;
        size_t emitted = 0;
        bool has_more = false;

        while (pos < len) {
            const char* nl = std::strchr(buf + pos, '\n');
            size_t line_len = (nl != nullptr)
                ? static_cast<size_t>(nl - (buf + pos))
                : std::strlen(buf + pos);
            if (line_len == 0) break;

            EventRecord rec{};
            if (parseEventRecord(buf + pos, line_len, &rec) == ESP_OK) {
                total++;
                if (emitted < limit && total > offset) {
                    if (emitted < out_cap) {
                        out[emitted] = rec;
                    }
                    emitted++;
                } else if (total > offset) {
                    has_more = true;
                }
            }
            pos += line_len + 1;
        }

        *out_count = emitted;
        *out_has_more = has_more;
        return ESP_OK;
    }

    // ---- Status -----------------------------------------------------------
    esp_err_t getStatus(StorageStatus* out) {
        if (out == nullptr) return ESP_ERR_INVALID_ARG;
        *out = StorageStatus{};
        out->mounted = available();
        out->index_loaded = true;

        FreeSpace fs{};
        const esp_err_t ferr = backend_->freeSpace(&fs);
        if (ferr == ESP_OK) {
            out->total_bytes = fs.total_bytes;
            out->used_bytes = fs.used_bytes;
            out->free_bytes = fs.free_bytes;
        }
        return ESP_OK;
    }

    // ---- Retention --------------------------------------------------------
    esp_err_t getRetention(RetentionPolicy* out) {
        if (out == nullptr) return ESP_ERR_INVALID_ARG;
        *out = retention_;
        return ESP_OK;
    }

    // Config update per SPEC 7: validate all fields -> persist to NVS
    // (authoritative) -> apply to runtime. A persist failure leaves the
    // runtime policy untouched (rollback step), and the caller reads the
    // effective policy back with getRetention() for the sanitised response.
    esp_err_t setRetention(const RetentionPolicy* pol) {
        if (pol == nullptr) return ESP_ERR_INVALID_ARG;
        if (!retentionValid(*pol)) return ESP_ERR_INVALID_ARG;   // ranges
        const esp_err_t perr = saveRetention(*pol);
        if (perr != ESP_OK) {
            ESP_LOGE(TAG, "retention persist failed: %s (policy unchanged)",
                     esp_err_to_name(perr));
            return perr;
        }
        retention_ = *pol;   // apply (infallible for a plain struct copy)
        ESP_LOGI(TAG, "retention applied: days m/o/d/t=%u/%u/%u/%u "
                      "max=%u%% enabled=%d",
                 static_cast<unsigned>(retention_.retention_days_manual),
                 static_cast<unsigned>(retention_.retention_days_motion),
                 static_cast<unsigned>(retention_.retention_days_detection),
                 static_cast<unsigned>(retention_.retention_days_timelapse),
                 static_cast<unsigned>(retention_.max_used_percent),
                 retention_.enabled ? 1 : 0);
        return ESP_OK;
    }

    // Loads the policy from NVS at init. Missing namespace/key = first
    // boot -> built-in defaults (kept). Unknown schema version -> keep
    // defaults rather than guessing at foreign data (AGENTS rule 21).
    esp_err_t loadRetention() {
        nvs_handle_t h = 0;
        const esp_err_t oerr = nvs_open(kRetNvsNs, NVS_READONLY, &h);
        if (oerr == ESP_ERR_NVS_NOT_FOUND) return ESP_OK;   // never written
        if (oerr != ESP_OK) return oerr;
        struct Guard { nvs_handle_t hh; ~Guard() { nvs_close(hh); } } guard{h};

        uint32_t ver = 0;
        esp_err_t err = nvs_get_u32(h, kRetKeyVer, &ver);
        if (err == ESP_ERR_NVS_NOT_FOUND) return ESP_OK;    // no policy yet
        if (err != ESP_OK) return err;
        if (ver != kRetSchemaVersion) {
            ESP_LOGW(TAG, "retention schema %u != %u — keeping defaults",
                     static_cast<unsigned>(ver),
                     static_cast<unsigned>(kRetSchemaVersion));
            return ESP_OK;
        }

        RetentionPolicy p{};   // defaults survive any missing key
        auto get_u32 = [&](const char* key, uint32_t* dst) {
            uint32_t v = 0;
            const esp_err_t e = nvs_get_u32(h, key, &v);
            if (e == ESP_OK) *dst = v;
            // ESP_ERR_NVS_NOT_FOUND keeps the default; other errors
            // surface through the version read that already succeeded.
        };
        get_u32(kRetKeyDaysMan, &p.retention_days_manual);
        get_u32(kRetKeyDaysMot, &p.retention_days_motion);
        get_u32(kRetKeyDaysDet, &p.retention_days_detection);
        get_u32(kRetKeyDaysTl,  &p.retention_days_timelapse);
        get_u32(kRetKeyMaxPct,  &p.max_used_percent);
        uint8_t en = 1;
        if (nvs_get_u8(h, kRetKeyEnabled, &en) == ESP_OK) p.enabled = (en != 0);

        if (!retentionValid(p)) {
            ESP_LOGW(TAG, "stored retention fails validation — keeping defaults");
            return ESP_OK;
        }
        retention_ = p;
        ESP_LOGI(TAG, "retention loaded from NVS: max=%u%% enabled=%d",
                 static_cast<unsigned>(p.max_used_percent), p.enabled ? 1 : 0);
        return ESP_OK;
    }

    // Writes every policy field plus the schema version. Any set failure
    // aborts before commit so NVS never holds a half-written policy.
    esp_err_t saveRetention(const RetentionPolicy& p) {
        nvs_handle_t h = 0;
        const esp_err_t oerr = nvs_open(kRetNvsNs, NVS_READWRITE, &h);
        if (oerr != ESP_OK) return oerr;
        struct Guard { nvs_handle_t hh; ~Guard() { nvs_close(hh); } } guard{h};

        esp_err_t err = ESP_OK;
        auto set_u32 = [&](const char* key, uint32_t v) {
            if (err == ESP_OK) err = nvs_set_u32(h, key, v);
        };
        set_u32(kRetKeyVer,     kRetSchemaVersion);
        set_u32(kRetKeyDaysMan, p.retention_days_manual);
        set_u32(kRetKeyDaysMot, p.retention_days_motion);
        set_u32(kRetKeyDaysDet, p.retention_days_detection);
        set_u32(kRetKeyDaysTl,  p.retention_days_timelapse);
        set_u32(kRetKeyMaxPct,  p.max_used_percent);
        if (err == ESP_OK) err = nvs_set_u8(h, kRetKeyEnabled, p.enabled ? 1 : 0);
        if (err == ESP_OK) err = nvs_commit(h);
        return err;
    }

    esp_err_t previewDeletions(DeletionCandidate* out,
                               size_t max_candidates,
                               size_t* out_count) {
        if (out_count == nullptr) return ESP_ERR_INVALID_ARG;
        *out_count = 0;
        return ESP_OK;
    }

    esp_err_t runRetention(uint32_t* out_deleted) {
        if (out_deleted) *out_deleted = 0;
        return ESP_OK;
    }

    // ---- Index rebuild ----------------------------------------------------
    esp_err_t rebuildIndex() {
        id_tracker_.next_media = 1;
        id_tracker_.next_event = 1;
        return ESP_OK;
    }

    // ---- Delete -----------------------------------------------------------
    esp_err_t deleteMedia(const char* media_id) {
        MediaRecord rec{};
        const esp_err_t lerr = lookupMedia(media_id, &rec);
        if (lerr != ESP_OK) return lerr;

        const esp_err_t rerr = backend_->removeFile(rec.path);
        if (rerr != ESP_OK && rerr != ESP_ERR_NOT_FOUND) return rerr;

        return ESP_OK;
    }

    // ---- Path helpers -----------------------------------------------------
    bool pathIsUnderSd(const char* path) {
        if (path == nullptr) return false;
        return std::strncmp(path, "/sdcard", 7) == 0;
    }

    esp_err_t resolvePath(const char* media_id, char* out_path,
                          size_t out_cap) {
        MediaRecord rec{};
        const esp_err_t err = lookupMedia(media_id, &rec);
        if (err != ESP_OK) return err;
        std::strncpy(out_path, rec.path, out_cap - 1);
        out_path[out_cap - 1] = '\0';
        return ESP_OK;
    }

private:
    esp_err_t loadIndexes() {
        // TODO: implement proper index reading (TBD)
        id_tracker_.next_media = 1;
        id_tracker_.next_event = 1;
        return ESP_OK;
    }

    void appendMediaIndexLine(const MediaRecord& rec, char* out,
                              size_t out_cap) {
        char escaped_path[256] = {};
        char escaped_mime[64] = {};
        char escaped_status[64] = {};
        char escaped_event[64] = {};
        jsonEscape(rec.path, escaped_path, sizeof(escaped_path));
        jsonEscape(rec.mime, escaped_mime, sizeof(escaped_mime));
        jsonEscape(rec.status, escaped_status, sizeof(escaped_status));
        jsonEscape(rec.event_id, escaped_event, sizeof(escaped_event));

        std::snprintf(out, out_cap,
                      "{\"schema_version\":1,\"media_id\":\"%s\","
                      "\"type\":%d,\"category\":%d,"
                      "\"path\":\"%s\",\"mime\":\"%s\","
                      "\"size\":%u,\"created_at\":\"%s\","
                      "\"event_id\":\"%s\",\"duration_ms\":%u,"
                      "\"status\":\"%s\"}",
                      rec.media_id,
                      static_cast<int>(rec.type),
                      static_cast<int>(rec.category),
                      escaped_path,
                      escaped_mime,
                      static_cast<unsigned int>(rec.size),
                      rec.created_at,
                      escaped_event,
                      static_cast<unsigned int>(rec.duration_ms),
                      escaped_status);
    }

    // Parse a single JSONL media record line.
    esp_err_t parseMediaRecord(const char* line, size_t len,
                               MediaRecord* out) {
        out->type = MediaType::Snapshot;
        out->category = MediaCategory::Manual;
        out->size = 0;
        out->duration_ms = 0;
        std::strncpy(out->status, "ready", sizeof(out->status) - 1);

        extractJsonString(line, "media_id", out->media_id,
                          sizeof(out->media_id));
        extractJsonString(line, "path", out->path, sizeof(out->path));
        extractJsonString(line, "mime", out->mime, sizeof(out->mime));
        extractJsonString(line, "created_at", out->created_at,
                          sizeof(out->created_at));
        extractJsonString(line, "event_id", out->event_id,
                          sizeof(out->event_id));
        extractJsonString(line, "status", out->status,
                          sizeof(out->status));

        int type_val = 0;
        extractJsonInt(line, "type", &type_val);
        out->type = static_cast<MediaType>(type_val);

        int cat_val = 0;
        extractJsonInt(line, "category", &cat_val);
        out->category = static_cast<MediaCategory>(cat_val);

        extractJsonUInt(line, "size", &out->size);
        extractJsonUInt(line, "duration_ms", &out->duration_ms);

        return ESP_OK;
    }

    // Parse a single JSONL event record line.
    esp_err_t parseEventRecord(const char* line, size_t len,
                               EventRecord* out) {
        out->event_type[0] = '\0';
        out->created_at[0] = '\0';
        out->ended_at[0] = '\0';
        out->snapshot_id[0] = '\0';
        out->recording_id[0] = '\0';
        std::strncpy(out->notification_status, "queued",
                     sizeof(out->notification_status) - 1);

        extractJsonString(line, "event_id", out->event_id,
                          sizeof(out->event_id));
        extractJsonString(line, "event_type", out->event_type,
                          sizeof(out->event_type));
        extractJsonString(line, "created_at", out->created_at,
                          sizeof(out->created_at));
        extractJsonString(line, "ended_at", out->ended_at,
                          sizeof(out->ended_at));
        extractJsonString(line, "snapshot_id", out->snapshot_id,
                          sizeof(out->snapshot_id));
        extractJsonString(line, "recording_id", out->recording_id,
                          sizeof(out->recording_id));
        extractJsonString(line, "notification_status", out->notification_status,
                          sizeof(out->notification_status));
        return ESP_OK;
    }

    IStorageBackend* backend_;
    struct {
        uint32_t next_media = 1;
        uint32_t next_event = 1;
    } id_tracker_;
    RetentionPolicy retention_{};
};

// ========================================================================
// Public API (thin wrappers around the impl)
// ========================================================================

namespace {

StorageManager* g_mgr = nullptr;

}  // namespace

esp_err_t storageManagerInit(IStorageBackend* backend) {
    if (g_mgr != nullptr) return ESP_ERR_INVALID_STATE;
    g_mgr = new StorageManager(backend);
    if (g_mgr == nullptr) return ESP_ERR_NO_MEM;
    return g_mgr->init();
}

bool storageManagerAvailable() {
    return g_mgr != nullptr && g_mgr->available();
}

const char* storageMountPoint() {
    return g_mgr != nullptr ? g_mgr->mountPoint() : "";
}

// ---- ID allocation -------------------------------------------------------
esp_err_t storageAllocateMediaId(MediaType type, char* out_id,
                                 size_t out_cap) {
    if (g_mgr == nullptr) return ESP_ERR_INVALID_STATE;
    return g_mgr->allocateMediaId(type, out_id, out_cap);
}

esp_err_t storageAllocateEventId(char* out_id, size_t out_cap) {
    if (g_mgr == nullptr) return ESP_ERR_INVALID_STATE;
    return g_mgr->allocateEventId(out_id, out_cap);
}

// ---- Snapshot recording --------------------------------------------------
esp_err_t storageRecordSnapshot(MediaCategory category,
                                const char* event_id,
                                const char* path,
                                uint32_t size,
                                MediaRecord* out_record) {
    if (g_mgr == nullptr) return ESP_ERR_INVALID_STATE;
    return g_mgr->recordSnapshot(category, event_id, path, size, out_record);
}

esp_err_t storageRecordEvent(const char* event_type,
                             const char* snapshot_id,
                             const char* recording_id,
                             EventRecord* out_record) {
    if (g_mgr == nullptr) return ESP_ERR_INVALID_STATE;
    return ESP_ERR_NOT_FOUND;
}

// ---- Lookup --------------------------------------------------------------
esp_err_t storageLookupMedia(const char* media_id, MediaRecord* out) {
    if (g_mgr == nullptr) return ESP_ERR_INVALID_STATE;
    return g_mgr->lookupMedia(media_id, out);
}

esp_err_t storageLookupEvent(const char* event_id, EventRecord* out) {
    if (g_mgr == nullptr) return ESP_ERR_INVALID_STATE;
    return g_mgr->lookupEvent(event_id, out);
}

// ---- List ----------------------------------------------------------------
esp_err_t storageListMedia(MediaCategory category_filter,
                           uint32_t offset, uint32_t limit,
                           MediaRecord* out, size_t out_cap,
                           size_t* out_count, bool* out_has_more) {
    if (g_mgr == nullptr) return ESP_ERR_INVALID_STATE;
    return g_mgr->listMedia(category_filter, offset, limit, out, out_cap,
                            out_count, out_has_more);
}

esp_err_t storageListEvents(uint32_t offset, uint32_t limit,
                            EventRecord* out, size_t out_cap,
                            size_t* out_count, bool* out_has_more) {
    if (g_mgr == nullptr) return ESP_ERR_INVALID_STATE;
    return g_mgr->listEvents(offset, limit, out, out_cap, out_count,
                             out_has_more);
}

// ---- Status --------------------------------------------------------------
esp_err_t storageGetStatus(StorageStatus* out) {
    if (g_mgr == nullptr) return ESP_ERR_INVALID_STATE;
    return g_mgr->getStatus(out);
}

// ---- Retention -----------------------------------------------------------
esp_err_t storageGetRetention(RetentionPolicy* out) {
    if (g_mgr == nullptr) return ESP_ERR_INVALID_STATE;
    return g_mgr->getRetention(out);
}

esp_err_t storageSetRetention(const RetentionPolicy* pol) {
    if (g_mgr == nullptr) return ESP_ERR_INVALID_STATE;
    return g_mgr->setRetention(pol);
}

esp_err_t storagePreviewDeletions(DeletionCandidate* out,
                                  size_t max_candidates,
                                  size_t* out_count) {
    if (g_mgr == nullptr) return ESP_ERR_INVALID_STATE;
    return g_mgr->previewDeletions(out, max_candidates, out_count);
}

esp_err_t storageRunRetention(uint32_t* out_deleted) {
    if (g_mgr == nullptr) return ESP_ERR_INVALID_STATE;
    return g_mgr->runRetention(out_deleted);
}

// ---- Index rebuild -------------------------------------------------------
esp_err_t storageRebuildIndex() {
    if (g_mgr == nullptr) return ESP_ERR_INVALID_STATE;
    return g_mgr->rebuildIndex();
}

// ---- Delete --------------------------------------------------------------
esp_err_t storageDeleteMedia(const char* media_id) {
    if (g_mgr == nullptr) return ESP_ERR_INVALID_STATE;
    return g_mgr->deleteMedia(media_id);
}

// ---- Path helpers --------------------------------------------------------
bool storagePathIsUnderSd(const char* path) {
    if (g_mgr == nullptr) return false;
    return g_mgr->pathIsUnderSd(path);
}

esp_err_t storageResolvePath(const char* media_id, char* out_path,
                             size_t out_cap) {
    if (g_mgr == nullptr) return ESP_ERR_INVALID_STATE;
    return g_mgr->resolvePath(media_id, out_path, out_cap);
}

}  // namespace visioncam