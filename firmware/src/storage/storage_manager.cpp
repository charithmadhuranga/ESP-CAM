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
#include <cstdlib>
#include <cstring>
#include <ctime>
#include <dirent.h>
#include <sys/stat.h>
#include <vector>

#include "esp_err.h"
#include "esp_heap_caps.h"
#include "esp_log.h"
#include "esp_system.h"
#include "esp_timer.h"
#include "nvs.h"

#include "ring_log.h"
#include "wall_clock.h"

namespace visioncam {
namespace {

const char* TAG = "vc_storage";

// Index file paths (relative to /sdcard mount).
constexpr char kMediaIndexPath[] = "/sdcard/system/index/media-index.jsonl";
constexpr char kEventIndexPath[] = "/sdcard/system/index/event-index.jsonl";

// Directory layout roots (SPEC §4.8), relative to /sdcard.
constexpr char kDirSnapshots[] = "/sdcard/snapshots";

// Max length of a single JSONL line, INCLUDING its '\n' terminator.
// Derived from the record's own structural maximum, never from observed
// data (AGENTS rule 13): fixed text ~130 B + media_id 24 + two ints ~6 +
// path (jsonEscape can double it: path[128] -> 256) + mime 64 + size 10 +
// created_at 20 + event_id 24 + duration 10 + status 64 + '\n' ≈ 610 B.
// 768 leaves headroom. The previous 512 could truncate a long path, and a
// truncated line has no '\n' — the next append would then concatenate onto
// it and silently lose a record from the index.
constexpr size_t kIndexLineMax = 768;

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

// Wall-clock seconds for media/event timestamps — see wall_clock.h for why
// this is not a bare time(nullptr) (pre-NTP the clock is uptime and would
// stamp records as 1970, breaking retention age maths — SPEC 4.8).
// Single definition shared with snapshot_manager.cpp.

// Format ISO-8601 timestamp from seconds since epoch.
void formatIso8601(uint32_t epoch, char* out, size_t out_cap) {
    if (epoch == 0) epoch = wallClockSeconds();
    // time_t is 64-bit on ESP-IDF: pass a real time_t, never reinterpret a
    // 4-byte uint32_t (the high half would be stack garbage → year 85818208).
    const time_t t = static_cast<time_t>(epoch);
    std::tm tm{};
    gmtime_r(&t, &tm);
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
    // formatIso8601() writes UTC ("Z"). This firmware never sets a TZ, so
    // newlib's mktime() treats the fields as UTC too and the round trip is
    // exact. If a TZ is ever configured, this needs timegm() (not in IDF).
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

        // One-shot migration from the index format written before records
        // carried their JSONL separator: that file is one physical line of
        // concatenated records, so loadIndexes() reads only the first (on
        // this board: 2154 B of index reported as media=1). Rebuilding from
        // the files on the card rewrites every record correctly.
        if (indexFileIsLegacy(kMediaIndexPath) ||
            indexFileIsLegacy(kEventIndexPath)) {
            ESP_LOGW(TAG,
                     "init: index has no record separators (pre-newline "
                     "format) — migrating by rebuilding from the card");
            return rebuildIndex();
        }

        // Boot-time proof that the per-line validation really rejects what
        // it claims to (acceptance "corrupt trailing JSONL entry is ignored
        // on load"). Runs on string literals only — never blocks boot.
        const bool st = selfTestIndexParsing();
        ESP_LOGI(TAG, "index parse self-test: %s", st ? "OK" : "FAILED");

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

    esp_err_t renameFile(const char* src, const char* dst) {
        if (backend_ == nullptr) return ESP_ERR_INVALID_STATE;
        return backend_->renameFile(src, dst);
    }

    esp_err_t removeFile(const char* path) {
        if (backend_ == nullptr) return ESP_ERR_INVALID_STATE;
        return backend_->removeFile(path);
    }

    esp_err_t statFile(const char* path, uint32_t* out_size,
                       uint32_t* out_mtime) {
        if (backend_ == nullptr) return ESP_ERR_INVALID_STATE;
        return backend_->statFile(path, out_size, out_mtime);
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
    // media_id: if non-empty, used as-is; otherwise a new ID is allocated.
    esp_err_t recordSnapshot(const char* media_id,
                             MediaCategory category,
                             const char* event_id,
                             const char* path,
                             uint32_t size,
                             MediaRecord* out_record) {
        if (!available()) return ESP_ERR_INVALID_STATE;
        if (path == nullptr || out_record == nullptr) return ESP_ERR_INVALID_ARG;

        MediaRecord rec{};
        if (media_id != nullptr && media_id[0] != '\0') {
            std::strncpy(rec.media_id, media_id, sizeof(rec.media_id) - 1);
            rec.media_id[sizeof(rec.media_id) - 1] = '\0';
        } else {
            const esp_err_t iderr = allocateMediaId(MediaType::Snapshot,
                                                    rec.media_id, sizeof(rec.media_id));
            if (iderr != ESP_OK) return iderr;
        }

        const uint32_t now = wallClockSeconds();
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
        if (!appendMediaIndexLine(rec, line, sizeof(line))) {
            // Unreachable now that kIndexLineMax exceeds the record's
            // structural maximum, but a truncated line would carry no '\n'
            // and would swallow the next append — so refuse rather than
            // corrupt the index. The file is already on disk; a rebuild
            // will index it.
            ESP_LOGE(TAG,
                     "recordSnapshot: index line for %s exceeds %u B — not "
                     "indexed (POST /storage?action=rebuild recovers it)",
                     rec.media_id, static_cast<unsigned>(kIndexLineMax));
            return ESP_ERR_INVALID_SIZE;
        }
        const esp_err_t perr = backend_->appendFile(kMediaIndexPath,
            reinterpret_cast<const uint8_t*>(line), std::strlen(line));
        if (perr != ESP_OK) {
            ESP_LOGE(TAG, "recordSnapshot: append to media index failed: %s",
                     esp_err_to_name(perr));
            return perr;
        }

        // Keep the in-memory cache in step with the file, or lookupMedia()
        // would 404 on a record that was just written (bounded: AGENTS 14).
        if (media_records_.size() < kMaxMediaRecords) {
            media_records_.push_back(rec);
        } else {
            ESP_LOGW(TAG, "recordSnapshot: media cache full (%u) — record "
                          "written to index only, not cached",
                     static_cast<unsigned>(kMaxMediaRecords));
        }

        if (out_record != nullptr) *out_record = rec;
        return ESP_OK;
    }

    // ---- Event recording ---------------------------------------------------
    esp_err_t recordEvent(const char* event_type,
                          const char* snapshot_id,
                          const char* recording_id,
                          EventRecord* out_record) {
        if (!available()) return ESP_ERR_INVALID_STATE;
        if (event_type == nullptr || out_record == nullptr)
            return ESP_ERR_INVALID_ARG;

        EventRecord rec{};
        const esp_err_t iderr = allocateEventId(rec.event_id,
                                                sizeof(rec.event_id));
        if (iderr != ESP_OK) return iderr;

        const uint32_t now = wallClockSeconds();
        formatIso8601(now, rec.created_at, sizeof(rec.created_at));
        std::strncpy(rec.event_type, event_type, sizeof(rec.event_type) - 1);
        rec.event_type[sizeof(rec.event_type) - 1] = '\0';
        if (snapshot_id != nullptr) {
            std::strncpy(rec.snapshot_id, snapshot_id,
                         sizeof(rec.snapshot_id) - 1);
            rec.snapshot_id[sizeof(rec.snapshot_id) - 1] = '\0';
        }
        if (recording_id != nullptr) {
            std::strncpy(rec.recording_id, recording_id,
                         sizeof(rec.recording_id) - 1);
            rec.recording_id[sizeof(rec.recording_id) - 1] = '\0';
        }

        // Append to event index.
        char line[kIndexLineMax] = {};
        if (!appendEventIndexLine(rec, line, sizeof(line))) {
            ESP_LOGE(TAG,
                     "recordEvent: index line for %s exceeds %u B — not "
                     "indexed (POST /storage?action=rebuild recovers it)",
                     rec.event_id, static_cast<unsigned>(kIndexLineMax));
            return ESP_ERR_INVALID_SIZE;
        }
        const esp_err_t perr = backend_->appendFile(kEventIndexPath,
            reinterpret_cast<const uint8_t*>(line), std::strlen(line));
        if (perr != ESP_OK) {
            ESP_LOGE(TAG, "recordEvent: append to event index failed: %s",
                     esp_err_to_name(perr));
            return perr;
        }

        event_records_.push_back(rec);
        if (out_record != nullptr) *out_record = rec;
        return ESP_OK;
    }

    // ---- Lookup -----------------------------------------------------------
    esp_err_t lookupMedia(const char* media_id, MediaRecord* out) {
        if (media_id == nullptr || out == nullptr) return ESP_ERR_INVALID_ARG;
        for (const auto& rec : media_records_) {
            if (std::strcmp(rec.media_id, media_id) == 0) {
                *out = rec;
                return ESP_OK;
            }
        }
        return ESP_ERR_NOT_FOUND;
    }

    esp_err_t lookupEvent(const char* event_id, EventRecord* out) {
        if (event_id == nullptr || out == nullptr) return ESP_ERR_INVALID_ARG;
        for (const auto& rec : event_records_) {
            if (std::strcmp(rec.event_id, event_id) == 0) {
                *out = rec;
                return ESP_OK;
            }
        }
        return ESP_ERR_NOT_FOUND;
    }

    // ---- List -------------------------------------------------------------
    esp_err_t listMedia(MediaCategory category_filter,
                        uint32_t offset, uint32_t limit,
                        MediaRecord* out, size_t out_cap,
                        size_t* out_count, bool* out_has_more) {
        if (out == nullptr || out_count == nullptr || out_has_more == nullptr)
            return ESP_ERR_INVALID_ARG;
        *out_count = 0;
        *out_has_more = false;

        size_t total = 0;
        size_t emitted = 0;
        for (const auto& rec : media_records_) {
            if (category_filter != static_cast<MediaCategory>(-1) &&
                rec.category != category_filter)
                continue;
            total++;
            if (total > offset && emitted < limit && emitted < out_cap) {
                out[emitted] = rec;
                emitted++;
            } else if (total > offset) {
                *out_has_more = true;
            }
        }
        *out_count = emitted;
        return ESP_OK;
    }

    esp_err_t listEvents(uint32_t offset, uint32_t limit,
                         EventRecord* out, size_t out_cap,
                         size_t* out_count, bool* out_has_more) {
        if (out == nullptr || out_count == nullptr || out_has_more == nullptr)
            return ESP_ERR_INVALID_ARG;

        // Read the whole event index from a bounded HEAP buffer, not the
        // stack: this runs on the HTTP task, and the previous 512-byte
        // stack buffer silently truncated the index to its first line
        // (any event index over 511 bytes could not be listed).
        char* buf = nullptr;
        size_t len = 0;
        const esp_err_t rerr = allocAndReadIndex(kEventIndexPath, &buf, &len);
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

            // Isolate this line so strstr()-based field extraction cannot
            // read into the next record (see loadIndexes).
            buf[pos + line_len] = '\0';
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
        std::free(buf);

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
        out->media_count = static_cast<uint32_t>(media_records_.size());
        out->event_count = static_cast<uint32_t>(event_records_.size());

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
        if (out == nullptr || max_candidates == 0) return ESP_OK;

        const uint32_t now = wallClockSeconds();
        const uint32_t now_days = now / 86400u;

        // Per-category retention days.
        uint32_t days_by_cat[4] = {
            retention_.retention_days_manual,
            retention_.retention_days_motion,
            retention_.retention_days_detection,
            retention_.retention_days_timelapse,
        };

        size_t count = 0;
        for (const auto& rec : media_records_) {
            if (count >= max_candidates) break;
            if (std::strcmp(rec.status, "ready") != 0) continue;

            uint32_t cat = static_cast<uint32_t>(rec.category);
            if (cat >= 4) continue;
            uint32_t ttl_days = days_by_cat[cat];
            if (ttl_days == 0) continue; // 0 means keep forever

            uint32_t created = parseIso8601(rec.created_at);
            if (created == 0) continue;
            uint32_t created_days = created / 86400u;
            if (now_days <= created_days + ttl_days) continue;

            DeletionCandidate& c = out[count++];
            std::strncpy(c.media_id, rec.media_id, sizeof(c.media_id) - 1);
            c.media_id[sizeof(c.media_id) - 1] = '\0';
            c.category = rec.category;
            std::strncpy(c.path, rec.path, sizeof(c.path) - 1);
            c.path[sizeof(c.path) - 1] = '\0';
            c.size = rec.size;
            c.age_days = (now_days > created_days) ? (now_days - created_days) : 0;
        }
        *out_count = count;
        return ESP_OK;
    }

    esp_err_t runRetention(uint32_t* out_deleted) {
        if (out_deleted) *out_deleted = 0;
        if (!retention_.enabled) return ESP_OK;

        DeletionCandidate cands[16] = {};
        size_t cand_count = 0;
        const esp_err_t perr = previewDeletions(cands, 16, &cand_count);
        if (perr != ESP_OK) return perr;

        uint32_t deleted = 0;
        for (size_t i = 0; i < cand_count; ++i) {
            const esp_err_t derr = deleteMedia(cands[i].media_id);
            if (derr == ESP_OK) {
                deleted++;
                ESP_LOGI(TAG, "retention deleted %s (age=%u days, category=%u)",
                         cands[i].media_id,
                         static_cast<unsigned>(cands[i].age_days),
                         static_cast<unsigned>(cands[i].category));
            } else if (derr == ESP_ERR_NOT_FOUND) {
                continue; // already gone
            } else {
                ESP_LOGW(TAG, "retention delete %s failed: %s",
                         cands[i].media_id, esp_err_to_name(derr));
            }
        }
        if (out_deleted) *out_deleted = deleted;
        return ESP_OK;
    }

    // ---- Index rebuild ----------------------------------------------------
    esp_err_t rebuildIndex() {
        // Keep the CURRENT path -> media_id mapping so a rebuild PRESERVES
        // existing IDs. Without this the scan re-allocates IDs from 1 in
        // directory order, and an ID a client had bookmarked could point at
        // a different file afterwards — opaque IDs must be stable (AGENTS
        // rule 15, SPEC 6.5).
        std::vector<MediaRecord> previous = std::move(media_records_);

        media_records_.clear();
        event_records_.clear();
        id_tracker_.next_media = 1;
        id_tracker_.next_event = 1;

        // Remove old index files so we rebuild from scratch.
        backend_->removeFile(kMediaIndexPath);
        backend_->removeFile(kEventIndexPath);

        // Scan /sdcard/snapshots recursively for media files.
        const esp_err_t err = scanDirForMedia(kDirSnapshots, previous);
        if (err != ESP_OK) return err;

        // Reload indexes from the freshly written files.
        return loadIndexes();
    }

    // ---- Delete -----------------------------------------------------------
    esp_err_t deleteMedia(const char* media_id) {
        MediaRecord rec{};
        const esp_err_t lerr = lookupMedia(media_id, &rec);
        if (lerr != ESP_OK) return lerr;

        const esp_err_t rerr = backend_->removeFile(rec.path);
        if (rerr != ESP_OK && rerr != ESP_ERR_NOT_FOUND) return rerr;

        // Remove from in-memory index.
        for (auto it = media_records_.begin(); it != media_records_.end(); ++it) {
            if (std::strcmp(it->media_id, media_id) == 0) {
                media_records_.erase(it);
                break;
            }
        }

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

    // Write bytes to a file via the backend (temp + rename, parent dirs
    // created automatically). Path must be under /sdcard.
    esp_err_t writeFile(const char* path, const uint8_t* data,
                        size_t len) {
        if (!pathIsUnderSd(path)) return ESP_ERR_INVALID_ARG;
        return backend_->writeFile(path, data, len);
    }

    // Read bytes from a file via the backend. Path must be under /sdcard.
    esp_err_t readFile(const char* path, uint8_t* data, size_t len,
                       size_t* out_len) {
        if (!pathIsUnderSd(path)) return ESP_ERR_INVALID_ARG;
        return backend_->readFile(path, data, len, out_len);
    }

    // Recursively scan a directory for .jpg media files, create index
    // records, and append them to the media index JSONL file.
    //
    // Iterative BFS, NOT recursion: the old recursive version carried
    // DirEntry[16] (~1.2 KB) + full_path + index line + MediaRecord in every
    // frame, so depth 4-6 overflowed the 4 KB HTTP task stack — observed as
    // "Guru Meditation: StoreProhibited" on POST /storage?action=rebuild.
    // Scratch lives on the heap (bounded) so stack use stays ~1 KB regardless
    // of tree depth.
    esp_err_t scanDirForMedia(const char* root,
                              const std::vector<MediaRecord>& previous) {
        // Bounded BFS queue of directories still to visit. Overflow policy:
        // skip the subtree and log — never block, never grow (AGENTS rule 12).
        constexpr size_t kMaxQueue = 64;
        constexpr size_t kPathMax = 160;   // rec.path is 128; leave headroom
        constexpr size_t kMaxEntries = 16;

        using PathSlot = char[kPathMax];
        auto* queue = static_cast<PathSlot*>(
            heap_caps_malloc(kMaxQueue * sizeof(PathSlot), MALLOC_CAP_DEFAULT));
        auto* entries = static_cast<DirEntry*>(
            heap_caps_malloc(kMaxEntries * sizeof(DirEntry), MALLOC_CAP_DEFAULT));
        if (queue == nullptr || entries == nullptr) {
            free(queue);
            free(entries);
            ESP_LOGE(TAG, "scanDirForMedia: scratch alloc failed");
            return ESP_ERR_NO_MEM;
        }

        esp_err_t result = ESP_OK;
        size_t q_head = 0, q_tail = 0, skipped = 0;
        std::snprintf(queue[q_tail], kPathMax, "%s", root);
        ++q_tail;

        while (q_head < q_tail) {
            const char* dir_path = queue[q_head++];

            size_t count = 0;
            const esp_err_t err = backend_->listDir(dir_path, entries,
                                                    kMaxEntries, &count);
            if (err != ESP_OK) {
                // A missing directory is normal (category never used yet).
                if (err != ESP_ERR_NOT_FOUND) {
                    ESP_LOGW(TAG, "scanDirForMedia: listDir(%s): %s",
                             dir_path, esp_err_to_name(err));
                }
                continue;
            }
            // Rebuild is a user-invoked, infrequent operation — one line
            // per directory is what makes a wrong scan diagnosable from a
            // serial capture instead of a guess (index has fewer records
            // than the card has files).
            ESP_LOGI(TAG, "rebuild scan: %s -> %u entr%s", dir_path,
                     static_cast<unsigned>(count), (count == 1) ? "y" : "ies");

            for (size_t i = 0; i < count; ++i) {
                char full_path[kPathMax] = {};
                const int n = std::snprintf(full_path, sizeof(full_path),
                                            "%s/%s", dir_path, entries[i].name);
                if (n <= 0 || static_cast<size_t>(n) >= sizeof(full_path)) {
                    ++skipped;
                    continue;
                }

                if (entries[i].is_dir) {
                    if (q_tail >= kMaxQueue) {
                        ++skipped;
                        continue;   // bounded: drop, don't grow
                    }
                    std::snprintf(queue[q_tail], kPathMax, "%s", full_path);
                    ++q_tail;
                    continue;
                }

                const char* ext = std::strrchr(entries[i].name, '.');
                if (ext == nullptr || std::strcmp(ext, ".jpg") != 0) continue;
                if (media_records_.size() >= kMaxMediaRecords) continue;

                uint32_t fsize = 0, ftime = 0;
                const esp_err_t serr =
                    backend_->statFile(full_path, &fsize, &ftime);
                ESP_LOGI(TAG, "rebuild scan: %s stat=%s size=%u", full_path,
                         esp_err_to_name(serr), static_cast<unsigned>(fsize));
                if (serr != ESP_OK) {
                    // Indexing a file we cannot stat would publish a record
                    // with size 0 that 404s on fetch — worse than leaving it
                    // out. The line above says which one was skipped.
                    ++skipped;
                    continue;
                }
                if (fsize == 0) {
                    // An empty file is an interrupted write, not a snapshot:
                    // it has no JPEG in it and fetching it would 404. Seen on
                    // this board as /sdcard/snapshots/1970/01/01/manual/*.jpg
                    // from captures made before the clock was set. Left on
                    // the card (rebuild never deletes) but never indexed.
                    ESP_LOGW(TAG,
                             "rebuild scan: %s is 0 B — empty file, not "
                             "indexed (safe to delete by hand)",
                             full_path);
                    ++skipped;
                    continue;
                }

                MediaRecord rec{};
                // Reuse the ID this path already had, if the previous index
                // knew it — a rebuild must not silently renumber. Only files
                // the old index never saw get a fresh ID.
                const char* prior_id = nullptr;
                for (const auto& prev : previous) {
                    if (std::strcmp(prev.path, full_path) == 0) {
                        prior_id = prev.media_id;
                        break;
                    }
                }
                if (prior_id != nullptr) {
                    std::strncpy(rec.media_id, prior_id,
                                 sizeof(rec.media_id) - 1);
                    // Keep the allocator past every reused ID so a later
                    // capture cannot hand out an ID already in use.
                    const uint32_t reused = static_cast<uint32_t>(
                        std::atoi(prior_id + 4));  // skip "med-"
                    if (reused >= id_tracker_.next_media)
                        id_tracker_.next_media = reused + 1;
                } else if (allocateMediaId(MediaType::Snapshot, rec.media_id,
                                           sizeof(rec.media_id)) != ESP_OK) {
                    continue;
                }

                const char* base = std::strrchr(dir_path, '/');
                const char* cat_name = (base != nullptr) ? base + 1 : dir_path;
                if (std::strcmp(cat_name, "motion") == 0)
                    rec.category = MediaCategory::Motion;
                else if (std::strcmp(cat_name, "detection") == 0)
                    rec.category = MediaCategory::Detection;
                else if (std::strcmp(cat_name, "timelapse") == 0)
                    rec.category = MediaCategory::Timelapse;
                else
                    rec.category = MediaCategory::Manual;

                // Use the FILE's mtime, not "now". Rebuild runs on demand,
                // so stamping every record with the rebuild moment would
                // make retention's age maths treat an old recording as
                // brand new and nothing would ever age out (SPEC 4.9).
                // A file written before the clock was set has an mtime near
                // the epoch — fall back to the wall clock for those.
                const uint32_t stamp =
                    (ftime >= static_cast<uint32_t>(kWallClockEraThreshold))
                        ? ftime
                        : wallClockSeconds();
                formatIso8601(stamp, rec.created_at, sizeof(rec.created_at));
                rec.type = MediaType::Snapshot;
                rec.size = fsize;
                std::strncpy(rec.path, full_path, sizeof(rec.path) - 1);
                rec.path[sizeof(rec.path) - 1] = '\0';
                std::strncpy(rec.mime, "image/jpeg", sizeof(rec.mime) - 1);
                std::strncpy(rec.status, "ready", sizeof(rec.status) - 1);

                char line[kIndexLineMax] = {};
                if (!appendMediaIndexLine(rec, line, sizeof(line))) {
                    // One over-long record must not abort the whole scan.
                    ESP_LOGW(TAG,
                             "rebuild: index line for %s too long — skipped",
                             rec.media_id);
                    ++skipped;
                    continue;
                }
                const esp_err_t perr = backend_->appendFile(
                    kMediaIndexPath,
                    reinterpret_cast<const uint8_t*>(line), std::strlen(line));
                if (perr != ESP_OK) {
                    ESP_LOGE(TAG, "scanDirForMedia: append index failed: %s",
                             esp_err_to_name(perr));
                    result = perr;
                    break;
                }

                media_records_.push_back(rec);
                const char* num = rec.media_id + 4;
                uint32_t id = static_cast<uint32_t>(std::atoi(num));
                if (id >= id_tracker_.next_media)
                    id_tracker_.next_media = id + 1;
            }
            if (result != ESP_OK) break;
        }

        if (skipped > 0) {
            ESP_LOGW(TAG,
                     "scanDirForMedia: %u file(s) not indexed (0 B empty, "
                     "stat failed, directory queue full, or path too long) "
                     "— see the rebuild scan lines above",
                     static_cast<unsigned>(skipped));
        }
        free(queue);
        free(entries);
        return result;
    }

private:
    // True when an index file holds records written WITHOUT the JSONL
    // record separator (a build from before the trailing '\n' was added).
    // Such a file is one physical line of concatenated records, so
    // loadIndexes()'s strchr('\n') splitter reads only the first — observed
    // on hardware as 2154 B of index reporting media=1.
    // `}{` cannot occur in a well-formed JSONL file (a record ends with `}`
    // then `\n`), so its presence is a definitive and bounded test: one
    // strstr over a buffer already capped by kIndexLoadMaxBytes.
    bool indexFileIsLegacy(const char* path) {
        char* buf = nullptr;
        size_t len = 0;
        if (allocAndReadIndex(path, &buf, &len) != ESP_OK) return false;
        // allocAndReadIndex() returns ESP_OK with buf == nullptr when there
        // is no index file yet (the normal first-boot case for the event
        // index). strstr(NULL, ...) panics with LoadProhibited at EXCVADDR 0
        // — seen as a crash loop on hardware, so the guard is not optional.
        if (buf == nullptr || len == 0) {
            std::free(buf);  // no-op for nullptr
            return false;
        }
        const bool legacy = (std::strstr(buf, "}{") != nullptr);
        std::free(buf);
        return legacy;
    }

    // Reads one JSONL index file into a HEAP buffer (PSRAM first, then the
    // default heap) bounded by kIndexLoadMaxBytes.
    //
    // This must never be a stack buffer: loadIndexes() runs on the HTTP
    // task as well as on main (rebuildIndex() <- POST
    // /storage?action=rebuild), and a 4 KB frame on the HTTP task's old
    // 4 KB stack is the overflow that zeroed a FreeRTOS task's
    // TCB.pxEndOfStack and panicked the board in Phase 2 (tasks.h
    // VC_STACK_HTTPD carries the full analysis).
    //
    // On return *out_buf is NUL-terminated with *out_len bytes, or is
    // nullptr/0 when the index does not exist yet (ESP_OK). Callers must
    // std::free() the buffer on ESP_OK.
    esp_err_t allocAndReadIndex(const char* path, char** out_buf,
                                size_t* out_len) {
        *out_buf = nullptr;
        *out_len = 0;

        uint32_t file_size = 0;
        const esp_err_t serr = backend_->statFile(path, &file_size, nullptr);
        if (serr == ESP_ERR_NOT_FOUND) {           // no index yet
            ESP_LOGI(TAG, "%s: no index file yet", path);
            return ESP_OK;
        }
        if (serr != ESP_OK) {
            // Stat failed for some reason other than "missing": read as much
            // as the ceiling allows rather than reporting the index empty.
            ESP_LOGW(TAG, "%s: stat failed (%s) — reading up to %u B",
                     path, esp_err_to_name(serr),
                     static_cast<unsigned>(kIndexLoadMaxBytes));
            file_size = kIndexLoadMaxBytes - 1;
        }
        if (file_size == 0) {
            ESP_LOGI(TAG, "%s: index file is empty", path);
            return ESP_OK;
        }

        size_t want = static_cast<size_t>(file_size) + 1;  // + NUL
        if (want > kIndexLoadMaxBytes) {
            ESP_LOGW(TAG,
                     "%s is %u B, over the %u B index load ceiling — records "
                     "beyond it will be missing until "
                     "POST /storage?action=rebuild regenerates the index",
                     path, static_cast<unsigned>(file_size),
                     static_cast<unsigned>(kIndexLoadMaxBytes));
            want = kIndexLoadMaxBytes;
        }

        char* buf = static_cast<char*>(
            heap_caps_malloc(want, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT));
        if (buf == nullptr) buf = static_cast<char*>(std::malloc(want));
        if (buf == nullptr) return ESP_ERR_NO_MEM;

        size_t got = 0;
        const esp_err_t rerr = backend_->readFile(
            path, reinterpret_cast<uint8_t*>(buf), want - 1, &got);
        if (rerr != ESP_OK) {
            std::free(buf);
            if (rerr == ESP_ERR_NOT_FOUND) return ESP_OK;
            return rerr;
        }
        buf[got] = '\0';
        ESP_LOGI(TAG, "%s: %u B read (buffer %u B)", path,
                 static_cast<unsigned>(got), static_cast<unsigned>(want));
        *out_buf = buf;
        *out_len = got;
        return ESP_OK;
    }

    esp_err_t loadIndexes() {
        media_records_.clear();
        event_records_.clear();
        id_tracker_.next_media = 1;
        id_tracker_.next_event = 1;

        // --- media index ---
        char* buf = nullptr;
        size_t len = 0;
        esp_err_t err = allocAndReadIndex(kMediaIndexPath, &buf, &len);
        if (err != ESP_OK) return err;

        size_t pos = 0;
        while (pos < len) {
            const char* nl = std::strchr(buf + pos, '\n');
            size_t line_len = (nl != nullptr)
                ? static_cast<size_t>(nl - (buf + pos))
                : std::strlen(buf + pos);
            if (line_len > 0 && media_records_.size() < kMaxMediaRecords) {
                // Terminate THIS line before parsing. The field extractors
                // use strstr(), which would otherwise run past the
                // separator and inherit a value from the NEXT record — a
                // truncated line with no media_id of its own would come back
                // holding its neighbour's. pos already advances by
                // line_len + 1, so overwriting the '\n' loses nothing
                // (buf[pos + line_len] is either that '\n' or the buffer's
                // existing NUL on the final line).
                buf[pos + line_len] = '\0';
                MediaRecord rec{};
                if (parseMediaRecord(buf + pos, line_len, &rec) == ESP_OK &&
                    rec.media_id[0] != '\0') {
                    media_records_.push_back(rec);
                    const char* num = rec.media_id + 4; // skip "med-"
                    uint32_t id = static_cast<uint32_t>(std::atoi(num));
                    if (id >= id_tracker_.next_media)
                        id_tracker_.next_media = id + 1;
                }
            }
            pos += line_len + 1;
        }
        std::free(buf);

        // --- event index ---
        buf = nullptr;
        len = 0;
        err = allocAndReadIndex(kEventIndexPath, &buf, &len);
        if (err != ESP_OK) return err;

        pos = 0;
        while (pos < len) {
            const char* nl = std::strchr(buf + pos, '\n');
            size_t line_len = (nl != nullptr)
                ? static_cast<size_t>(nl - (buf + pos))
                : std::strlen(buf + pos);
            if (line_len > 0 && event_records_.size() < kMaxEventRecords) {
                buf[pos + line_len] = '\0';  // isolate this line (see above)
                EventRecord rec{};
                if (parseEventRecord(buf + pos, line_len, &rec) == ESP_OK &&
                    rec.event_id[0] != '\0') {
                    event_records_.push_back(rec);
                    const char* num = rec.event_id + 4; // skip "evt-"
                    uint32_t id = static_cast<uint32_t>(std::atoi(num));
                    if (id >= id_tracker_.next_event)
                        id_tracker_.next_event = id + 1;
                }
            }
            pos += line_len + 1;
        }
        std::free(buf);

        ESP_LOGI(TAG, "loadIndexes: media=%u event=%u next_media=%u next_event=%u",
                 static_cast<unsigned>(media_records_.size()),
                 static_cast<unsigned>(event_records_.size()),
                 static_cast<unsigned>(id_tracker_.next_media),
                 static_cast<unsigned>(id_tracker_.next_event));
        return ESP_OK;
    }

    // NOTE the trailing '\n' in every format string below. This is the
    // record separator of the JSONL index: without it every append lands
    // against the previous line and the whole index becomes ONE physical
    // line, so loadIndexes()'s strchr('\n') splitter can read only the
    // first record and reports media=1 no matter how many records exist
    // (observed on hardware: 2154 B of index, 1 record).
    // Returns false when the record does not fit out_cap. The caller must
    // NOT append a truncated line: it would carry no '\n', so the next
    // append would concatenate onto it and both records would be lost.
    bool appendMediaIndexLine(const MediaRecord& rec, char* out,
                              size_t out_cap) {
        char escaped_path[256] = {};
        char escaped_mime[64] = {};
        char escaped_status[64] = {};
        char escaped_event[64] = {};
        jsonEscape(rec.path, escaped_path, sizeof(escaped_path));
        jsonEscape(rec.mime, escaped_mime, sizeof(escaped_mime));
        jsonEscape(rec.status, escaped_status, sizeof(escaped_status));
        jsonEscape(rec.event_id, escaped_event, sizeof(escaped_event));

        const int n = std::snprintf(out, out_cap,
                      "{\"schema_version\":1,\"media_id\":\"%s\","
                      "\"type\":%d,\"category\":%d,"
                      "\"path\":\"%s\",\"mime\":\"%s\","
                      "\"size\":%u,\"created_at\":\"%s\","
                      "\"event_id\":\"%s\",\"duration_ms\":%u,"
                      "\"status\":\"%s\"}\n",
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
        return (n > 0 && static_cast<size_t>(n) < out_cap);
    }

    bool appendEventIndexLine(const EventRecord& rec, char* out,
                              size_t out_cap) {
        char escaped_type[64] = {};
        char escaped_created[64] = {};
        char escaped_ended[64] = {};
        char escaped_snapshot[64] = {};
        char escaped_recording[64] = {};
        char escaped_notif[64] = {};
        jsonEscape(rec.event_type, escaped_type, sizeof(escaped_type));
        jsonEscape(rec.created_at, escaped_created, sizeof(escaped_created));
        jsonEscape(rec.ended_at, escaped_ended, sizeof(escaped_ended));
        jsonEscape(rec.snapshot_id, escaped_snapshot, sizeof(escaped_snapshot));
        jsonEscape(rec.recording_id, escaped_recording, sizeof(escaped_recording));
        jsonEscape(rec.notification_status, escaped_notif, sizeof(escaped_notif));

        const int n = std::snprintf(out, out_cap,
                      "{\"schema_version\":1,\"event_id\":\"%s\","
                      "\"event_type\":\"%s\",\"created_at\":\"%s\","
                      "\"ended_at\":\"%s\",\"snapshot_id\":\"%s\","
                      "\"recording_id\":\"%s\","
                      "\"notification_status\":\"%s\"}\n",
                      rec.event_id,
                      escaped_type,
                      escaped_created,
                      escaped_ended,
                      escaped_snapshot,
                      escaped_recording,
                      escaped_notif);
        return (n > 0 && static_cast<size_t>(n) < out_cap);
    }

    // "med-" followed by one or more digits -- allocateMediaId()'s format.
    bool isMediaId(const char* s) {
        if (s == nullptr || std::strncmp(s, "med-", 4) != 0) return false;
        const char* p = s + 4;
        if (*p == '\0') return false;
        for (; *p != '\0'; ++p)
            if (*p < '0' || *p > '9') return false;
        return true;
    }

    // "evt-" followed by one or more digits -- allocateEventId()'s format.
    bool isEventId(const char* s) {
        if (s == nullptr || std::strncmp(s, "evt-", 4) != 0) return false;
        const char* p = s + 4;
        if (*p == '\0') return false;
        for (; *p != '\0'; ++p)
            if (*p < '0' || *p > '9') return false;
        return true;
    }

    // Parse a single JSONL media record line.
    esp_err_t parseMediaRecord(const char* line, size_t len,
                               MediaRecord* out) {
        // Validate BEFORE extracting (plan 02 task 2.2: "validate each line
        // independently; ignore a malformed trailing record on recovery").
        // A power loss mid-append leaves a partial line, and it must not
        // become a half-filled record that lists a file with size 0. The
        // caller NUL-terminates `line` at `len`, so every read here is
        // bounded by the line, never by the rest of the file.
        if (line == nullptr || len < 3) return ESP_ERR_INVALID_ARG;
        if (line[0] != '{' || line[len - 1] != '}') return ESP_ERR_INVALID_ARG;

        out->type = MediaType::Snapshot;
        out->category = MediaCategory::Manual;
        out->size = 0;
        out->duration_ms = 0;
        std::strncpy(out->status, "ready", sizeof(out->status) - 1);

        extractJsonString(line, "media_id", out->media_id,
                          sizeof(out->media_id));
        // media_id is the record's identity: without the exact "med-<digits>"
        // form it can neither be addressed nor deleted safely (AGENTS 15).
        if (!isMediaId(out->media_id)) return ESP_ERR_INVALID_ARG;

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
        // Same per-line validation as parseMediaRecord: a corrupt trailing
        // record is ignored rather than half-parsed (plan 02 task 2.2).
        if (line == nullptr || len < 3) return ESP_ERR_INVALID_ARG;
        if (line[0] != '{' || line[len - 1] != '}') return ESP_ERR_INVALID_ARG;

        out->event_type[0] = '\0';
        out->created_at[0] = '\0';
        out->ended_at[0] = '\0';
        out->snapshot_id[0] = '\0';
        out->recording_id[0] = '\0';
        std::strncpy(out->notification_status, "queued",
                     sizeof(out->notification_status) - 1);

        extractJsonString(line, "event_id", out->event_id,
                          sizeof(out->event_id));
        if (!isEventId(out->event_id)) return ESP_ERR_INVALID_ARG;
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

    // Proves the per-line validation actually rejects what it claims to
    // (plan 02 task 2.2 / acceptance "corrupt trailing JSONL entry is
    // ignored on load"). There is no unit-test harness for firmware in this
    // repo (firmware/test/ is empty, platformio.ini has no test_env), so
    // the same convention as the SD self-test and the forced camera-failure
    // test applies: run the check on the device at boot and log the verdict.
    //
    // Runs entirely on string literals — no file I/O, no allocation, so it
    // cannot fail for an unrelated reason and cannot mask a real defect.
    // NEVER blocks boot: a failure is logged, init() continues.
public:
    // Public because the C wrapper is the API used at boot (and by tests).
    //
    // Proves the per-line validation really rejects what it claims to (plan
    // 02 task 2.2 / acceptance item "corrupt trailing JSONL entry is ignored
    // on load"). This repo has no firmware unit-test harness (firmware/test/
    // is empty, platformio.ini has no test_env), so the convention already
    // used by the SD self-test and the forced camera-failure test applies:
    // run the check on the device at boot and log the verdict.
    //
    // Runs entirely on string literals — no file I/O, no allocation — so it
    // cannot fail for an unrelated reason and cannot mask a real defect.
    // NEVER blocks boot: a failure is logged and init() continues.
    bool selfTestIndexParsing() {
        // Every case below is NUL-terminated exactly as loadIndexes() leaves
        // a line: buf[pos + line_len] = '\0' before parsing.
        struct Case {
            const char* line;
            bool expect_ok;
            const char* expect_id;   // nullptr = do not check the ID
            uint32_t expect_size;    // media only
        };
        static const Case kMediaCases[] = {
            // 1. A well-formed record must parse AND keep its identity.
            {"{\"schema_version\":1,\"media_id\":\"med-000042\","
             "\"type\":1,\"category\":0,"
             "\"path\":\"/sdcard/snapshots/2026/11/19/manual/med-000042.jpg\","
             "\"mime\":\"image/jpeg\",\"size\":12345,"
             "\"created_at\":\"2026-11-19T05:28:02Z\",\"event_id\":\"\","
             "\"duration_ms\":0,\"status\":\"ready\"}",
             true, "med-000042", 12345},
            // 2. Power loss mid-append: the closing brace never landed. Must
            //    be ignored, not half-parsed into a size-0 record.
            {"{\"schema_version\":1,\"media_id\":\"med-000043\","
             "\"type\":1,\"category\":0,\"path\":\"/sdcard/snapshots/x.jpg\"",
             false, nullptr, 0},
            // 3. Structurally valid JSON with no media_id: cannot be
            //    addressed or deleted, so reject (AGENTS rule 15).
            {"{\"schema_version\":1,\"size\":5}", false, nullptr, 0},
            // 4. Garbage / a non-JSON line in the middle of the file.
            {"not json at all", false, nullptr, 0},
            // 5. Wrong ID shape: "med-" but not digits — a torn numeric.
            {"{\"schema_version\":1,\"media_id\":\"med-00x\","
             "\"size\":10}", false, nullptr, 0},
            // 6. Empty line (the splitter never yields one, but be safe).
            {"", false, nullptr, 0},
        };

        for (const auto& c : kMediaCases) {
            MediaRecord rec{};
            const size_t len = std::strlen(c.line);
            const bool ok = (parseMediaRecord(c.line, len, &rec) == ESP_OK);
            if (ok != c.expect_ok) {
                ESP_LOGE(TAG,
                         "index parse self-test: media case '%.40s' "
                         "expected %s, got %s",
                         c.line, c.expect_ok ? "OK" : "reject",
                         ok ? "OK" : "reject");
                return false;
            }
            if (ok && c.expect_id != nullptr &&
                std::strcmp(rec.media_id, c.expect_id) != 0) {
                ESP_LOGE(TAG,
                         "index parse self-test: media id '%s' != '%s'",
                         rec.media_id, c.expect_id);
                return false;
            }
            if (ok && rec.size != c.expect_size) {
                ESP_LOGE(TAG,
                         "index parse self-test: media size %u != %u",
                         static_cast<unsigned>(rec.size),
                         static_cast<unsigned>(c.expect_size));
                return false;
            }
        }

        struct ECase {
            const char* line;
            bool expect_ok;
            const char* expect_id;
        };
        static const ECase kEventCases[] = {
            {"{\"schema_version\":1,\"event_id\":\"evt-000007\","
             "\"event_type\":\"motion\","
             "\"created_at\":\"2026-11-19T05:28:02Z\",\"ended_at\":\"\","
             "\"snapshot_id\":\"\",\"recording_id\":\"\","
             "\"notification_status\":\"queued\"}",
             true, "evt-000007"},
            // Missing event_id — same rejection rule as media.
            {"{\"schema_version\":1,\"event_type\":\"motion\"}",
             false, nullptr},
            // Torn trailing record.
            {"{\"schema_version\":1,\"event_id\":\"evt-000008\",",
             false, nullptr},
        };
        for (const auto& c : kEventCases) {
            EventRecord rec{};
            const size_t len = std::strlen(c.line);
            const bool ok = (parseEventRecord(c.line, len, &rec) == ESP_OK);
            if (ok != c.expect_ok) {
                ESP_LOGE(TAG,
                         "index parse self-test: event case '%.40s' "
                         "expected %s, got %s",
                         c.line, c.expect_ok ? "OK" : "reject",
                         ok ? "OK" : "reject");
                return false;
            }
            if (ok && c.expect_id != nullptr &&
                std::strcmp(rec.event_id, c.expect_id) != 0) {
                ESP_LOGE(TAG,
                         "index parse self-test: event id '%s' != '%s'",
                         rec.event_id, c.expect_id);
                return false;
            }
        }
        return true;
    }

private:
    IStorageBackend* backend_;
    struct {
        uint32_t next_media = 1;
        uint32_t next_event = 1;
    } id_tracker_;
    RetentionPolicy retention_{};

    // Bounded in-memory index caches (never load whole library into RAM;
    // cap keeps memory predictable — AGENTS rule 14).
    static constexpr size_t kMaxMediaRecords = 512;
    static constexpr size_t kMaxEventRecords = 512;
    // Ceiling for ONE in-memory copy of a JSONL index file (AGENTS rule 13).
    // Derived from the structural maximum of the index — never from the
    // file's own size, the request, or any media metadata — so the load
    // buffer is bounded whatever is on the card. 512 records x 768 B = 384
    // KB, allocated from PSRAM first (the index is transient and freed as
    // soon as the records are parsed; internal DRAM must stay available for
    // DMA). Steady state is the 512-record vector, not this buffer.
    static constexpr size_t kIndexLoadMaxBytes = kMaxMediaRecords * kIndexLineMax;
    std::vector<MediaRecord> media_records_;
    std::vector<EventRecord> event_records_;
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

// ---- Index parsing self-test ---------------------------------------------
bool storageSelfTestIndexParsing() {
    if (g_mgr == nullptr) return false;
    return g_mgr->selfTestIndexParsing();
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
esp_err_t storageRecordSnapshot(const char* media_id,
                                MediaCategory category,
                                const char* event_id,
                                const char* path,
                                uint32_t size,
                                MediaRecord* out_record) {
    if (g_mgr == nullptr) return ESP_ERR_INVALID_STATE;
    return g_mgr->recordSnapshot(media_id, category, event_id, path, size,
                                 out_record);
}

esp_err_t storageRecordEvent(const char* event_type,
                             const char* snapshot_id,
                             const char* recording_id,
                             EventRecord* out_record) {
    if (g_mgr == nullptr) return ESP_ERR_INVALID_STATE;
    return g_mgr->recordEvent(event_type, snapshot_id, recording_id,
                              out_record);
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

esp_err_t storageWriteFile(const char* path, const uint8_t* data,
                           size_t len) {
    if (g_mgr == nullptr) return ESP_ERR_INVALID_STATE;
    if (!g_mgr->pathIsUnderSd(path)) return ESP_ERR_INVALID_ARG;
    return g_mgr->writeFile(path, data, len);
}

esp_err_t storageReadFile(const char* path, uint8_t* data, size_t len,
                          size_t* out_len) {
    if (g_mgr == nullptr) return ESP_ERR_INVALID_STATE;
    if (!g_mgr->pathIsUnderSd(path)) return ESP_ERR_INVALID_ARG;
    return g_mgr->readFile(path, data, len, out_len);
}

esp_err_t storageRenameFile(const char* src, const char* dst) {
    if (g_mgr == nullptr) return ESP_ERR_INVALID_STATE;
    if (!g_mgr->pathIsUnderSd(src) || !g_mgr->pathIsUnderSd(dst))
        return ESP_ERR_INVALID_ARG;
    return g_mgr->renameFile(src, dst);
}

esp_err_t storageRemoveFile(const char* path) {
    if (g_mgr == nullptr) return ESP_ERR_INVALID_STATE;
    if (!g_mgr->pathIsUnderSd(path)) return ESP_ERR_INVALID_ARG;
    return g_mgr->removeFile(path);
}

esp_err_t storageStatFile(const char* path, uint32_t* out_size,
                          uint32_t* out_mtime) {
    if (g_mgr == nullptr) return ESP_ERR_INVALID_STATE;
    if (!g_mgr->pathIsUnderSd(path)) return ESP_ERR_INVALID_ARG;
    return g_mgr->statFile(path, out_size, out_mtime);
}

}  // namespace visioncam