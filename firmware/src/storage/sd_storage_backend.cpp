// sd_storage_backend.cpp — IStorageBackend over the onboard microSD
// (plan 02 task 2.1; SPEC sections 4.8, 4.9).
//
// Wraps the ESP-IDF FATFS VFS mount at /sdcard established by
// firmware/src/sd_bringup.cpp (esp_vfs_fat_sdmmc_mount). All file I/O goes
// through C stdio (fopen/fwrite/fclose/etc.) — this module is the only
// place that does so (AGENTS rule 10: storage owns filesystem access).
//
// Paths are relative to /sdcard. Opaque media IDs map to files under
// /sdcard/snapshots/YYYY/MM/DD/{category}/med-XXXXXXXX.jpg etc.
//
// Write pattern (SPEC §4.8): write to a temp/in-progress name, fsync,
// then rename to final. Never delete open files.

#include "sd_storage_backend.h"

#include <algorithm>
#include <cstdio>
#include <cstring>
#include <ctime>
#include <dirent.h>
#include <sys/stat.h>

#include "esp_err.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "esp_vfs_fat.h"
#include "ff.h"

#include "storage_manager.h"

namespace visioncam {
namespace {

const char* TAG = "vc_sd_backend";

// Mount point must match sd_bringup.cpp's kSdMountPoint ("/sdcard").
constexpr char kMountPoint[] = "/sdcard";

// Directory layout roots (SPEC §4.8), relative to /sdcard.
constexpr char kDirSystem[]    = "/sdcard/system";
constexpr char kDirIndex[]     = "/sdcard/system/index";
constexpr char kDirSnapshots[] = "/sdcard/snapshots";
constexpr char kDirRecordings[] = "/sdcard/recordings";
constexpr char kDirTimelapse[]  = "/sdcard/timelapse";
constexpr char kDirThumbnails[] = "/sdcard/thumbnails";
constexpr char kDirModels[]    = "/sdcard/models";
constexpr char kDirLogs[]      = "/sdcard/logs";

// Index files.
constexpr char kMediaIndex[]  = "/sdcard/system/index/media-index.jsonl";
constexpr char kEventIndex[]  = "/sdcard/system/index/event-index.jsonl";

// Sector-aligned write staging size (SPEC §4.8 rule 5: 8 KB sector
// multiples, confirmed by IDF FATFS ssize on this card).
constexpr size_t kSectorBytes = 8 * 1024;

// Ensure a directory exists (recursive mkdir). FATFS mkdir fails if any
// parent is missing, so we create each level in turn. The path may be a
// directory or a file — the final component is treated as a directory
// only when it has no extension; otherwise parent dirs are created.
esp_err_t ensureDir(const char* path) {
    char tmp[128] = {};
    size_t len = std::strlen(path);
    if (len >= sizeof(tmp)) return ESP_ERR_NO_MEM;
    std::memcpy(tmp, path, len);
    if (tmp[len - 1] == '/') tmp[len - 1] = '\0';

    // If the path looks like a file (has an extension after the last /),
    // truncate to the parent directory.
    char* last_slash = std::strrchr(tmp, '/');
    if (last_slash != nullptr) {
        const char* after_slash = last_slash + 1;
        if (std::strchr(after_slash, '.') != nullptr) {
            *last_slash = '\0';
            if (tmp[0] == '\0') return ESP_OK;
        }
    }

    // Skip the leading "/sdcard" prefix — it already exists.
    char* p = tmp;
    if (std::strncmp(p, "/sdcard", 7) == 0) p += 7;

    while (*p == '/') ++p;
    for (; *p; ++p) {
        if (*p == '/') {
            *p = '\0';
            if (::mkdir(tmp, 0777) != 0 && errno != EEXIST) {
                ESP_LOGW(TAG, "ensureDir: mkdir(%s) failed: %s", tmp,
                         std::strerror(errno));
                return ESP_FAIL;
            }
            *p = '/';
        }
    }
    // Final component.
    if (::mkdir(tmp, 0777) != 0 && errno != EEXIST) {
        ESP_LOGW(TAG, "ensureDir: mkdir(%s) failed: %s", tmp,
                 std::strerror(errno));
        return ESP_FAIL;
    }
    return ESP_OK;
}

}  // namespace

// ========================================================================
// SdStorageBackend
// ========================================================================

esp_err_t SdStorageBackend::begin() {
    // The SD card is already mounted by sd_bringup.cpp at /sdcard.
    // Just verify the directory layout exists.
    ESP_LOGI(TAG, "SdStorageBackend: ensuring directory layout at %s",
             kMountPoint);
    for (const char* d : {kDirSystem, kDirIndex, kDirSnapshots,
                          kDirRecordings, kDirTimelapse, kDirThumbnails,
                          kDirModels, kDirLogs}) {
        const esp_err_t err = ensureDir(d);
        if (err != ESP_OK) {
            ESP_LOGE(TAG, "SdStorageBackend: ensureDir(%s) failed", d);
            return err;
        }
    }
    ESP_LOGI(TAG, "SdStorageBackend: directory layout ready");
    return ESP_OK;
}

bool SdStorageBackend::isAvailable() const {
    // FATFS is mounted if the mount point is stat-able.
    struct stat st{};
    return ::stat(kMountPoint, &st) == 0;
}

esp_err_t SdStorageBackend::writeFile(const char* path, const uint8_t* data,
                                      size_t len) {
    // Create parent directories if needed.
    const esp_err_t derr = ensureDir(path);
    if (derr != ESP_OK) return derr;

    // Stage to a temp name, fsync, rename to final (SPEC §4.8).
    char tmp_path[140] = {};
    std::snprintf(tmp_path, sizeof(tmp_path), "%s.tmp", path);

    FILE* f = std::fopen(tmp_path, "w");
    if (f == nullptr) {
        ESP_LOGE(TAG, "writeFile: fopen(w) failed for %s: %s",
                 tmp_path, std::strerror(errno));
        return ESP_FAIL;
    }
    // Sector-aligned staging: write in kSectorBytes chunks.
    size_t written = 0;
    while (written < len) {
        const size_t chunk = std::min(len - written, kSectorBytes);
        const size_t n = std::fwrite(data + written, 1, chunk, f);
        if (n == 0) {
            ESP_LOGE(TAG, "writeFile: fwrite failed at %u/%u",
                     static_cast<unsigned>(written),
                     static_cast<unsigned>(len));
            std::fclose(f);
            std::remove(tmp_path);
            return ESP_FAIL;
        }
        written += n;
    }
    if (std::fflush(f) != 0) {
        ESP_LOGE(TAG, "writeFile: fflush failed for %s", tmp_path);
        std::fclose(f);
        std::remove(tmp_path);
        return ESP_FAIL;
    }
    if (std::fclose(f) != 0) {
        ESP_LOGE(TAG, "writeFile: fclose failed for %s", tmp_path);
        std::remove(tmp_path);
        return ESP_FAIL;
    }
    // FATFS (unlike POSIX) fails the rename with EEXIST when the target is
    // already there, so a retry after a partial capture could never
    // overwrite the stale file. Drop the old target first: the bytes are
    // already safely staged and fsynced in tmp_path, so the window between
    // remove and rename is a missing file, never a torn one.
    if (std::remove(path) != 0 && errno != ENOENT) {
        ESP_LOGW(TAG, "writeFile: remove(existing %s) failed: %s", path,
                 std::strerror(errno));
        // Fall through — rename may still succeed if it was a race.
    }
    if (std::rename(tmp_path, path) != 0) {
        ESP_LOGE(TAG, "writeFile: rename(%s → %s) failed: %s",
                 tmp_path, path, std::strerror(errno));
        std::remove(tmp_path);
        return ESP_FAIL;
    }
    return ESP_OK;
}

esp_err_t SdStorageBackend::appendFile(const char* path, const uint8_t* data,
                                       size_t len) {
    // Append to an existing file (JSONL index lines). Create parent dirs.
    const esp_err_t derr = ensureDir(path);
    if (derr != ESP_OK) return derr;

    FILE* f = std::fopen(path, "a");
    if (f == nullptr) {
        ESP_LOGE(TAG, "appendFile: fopen(a) failed for %s: %s",
                 path, std::strerror(errno));
        return ESP_FAIL;
    }
    const size_t written = std::fwrite(data, 1, len, f);
    const int fflush_err = std::fflush(f);
    const int fclose_err = std::fclose(f);
    if (written != len || fflush_err != 0 || fclose_err != 0) {
        ESP_LOGE(TAG, "appendFile: write/flush/close failed for %s", path);
        return ESP_FAIL;
    }
    return ESP_OK;
}

esp_err_t SdStorageBackend::readFile(const char* path, uint8_t* data,
                                     size_t len, size_t* out_len) {
    FILE* f = std::fopen(path, "r");
    if (f == nullptr) {
        if (errno == ENOENT) return ESP_ERR_NOT_FOUND;
        ESP_LOGE(TAG, "readFile: fopen(r) failed for %s: %s", path,
                 std::strerror(errno));
        return ESP_FAIL;
    }
    const size_t got = std::fread(data, 1, len, f);
    std::fclose(f);
    *out_len = got;
    return ESP_OK;
}

esp_err_t SdStorageBackend::removeFile(const char* path) {
    if (::remove(path) != 0) {
        if (errno == ENOENT) return ESP_ERR_NOT_FOUND;
        ESP_LOGE(TAG, "removeFile: remove(%s) failed: %s", path,
                 std::strerror(errno));
        return ESP_FAIL;
    }
    return ESP_OK;
}

esp_err_t SdStorageBackend::renameFile(const char* src, const char* dst) {
    if (::rename(src, dst) != 0) {
        ESP_LOGE(TAG, "renameFile: rename(%s → %s) failed: %s", src, dst,
                 std::strerror(errno));
        return ESP_FAIL;
    }
    return ESP_OK;
}

esp_err_t SdStorageBackend::statFile(const char* path, uint32_t* out_size,
                                     uint32_t* out_mtime) {
    struct stat st{};
    if (::stat(path, &st) != 0) {
        if (errno == ENOENT) return ESP_ERR_NOT_FOUND;
        return ESP_FAIL;
    }
    if (out_size) *out_size = static_cast<uint32_t>(st.st_size);
    if (out_mtime) *out_mtime = static_cast<uint32_t>(st.st_mtime);
    return ESP_OK;
}

esp_err_t SdStorageBackend::listDir(const char* dir, DirEntry* entries,
                                    size_t max_entries, size_t* out_count) {
    DIR* dp = ::opendir(dir);
    if (dp == nullptr) {
        if (errno == ENOENT) {
            *out_count = 0;
            return ESP_OK;
        }
        ESP_LOGE(TAG, "listDir: opendir(%s) failed: %s", dir,
                 std::strerror(errno));
        return ESP_FAIL;
    }
    size_t count = 0;
    struct dirent* de;
    while ((de = ::readdir(dp)) != nullptr && count < max_entries) {
        if (std::strcmp(de->d_name, ".") == 0 ||
            std::strcmp(de->d_name, "..") == 0) continue;
        DirEntry& e = entries[count++];
        std::strncpy(e.name, de->d_name, sizeof(e.name) - 1);
        e.name[sizeof(e.name) - 1] = '\0';
        e.is_dir = (de->d_type == DT_DIR);
        e.size = 0;
        e.mtime = 0;
        // Stat for size/mtime (bounded: one stat per entry).
        char full[512] = {};
        std::snprintf(full, sizeof(full), "%s/%s", dir, de->d_name);
        struct stat st{};
        if (::stat(full, &st) == 0) {
            e.size = static_cast<uint32_t>(st.st_size);
            e.mtime = static_cast<uint32_t>(st.st_mtime);
        }
    }
    ::closedir(dp);
    *out_count = count;
    return ESP_OK;
}

esp_err_t SdStorageBackend::freeSpace(FreeSpace* out) {
    // FATFS f_getfree on the mount point.
    FATFS* fatfs = nullptr;
    DWORD free_clusters = 0;
    const FRESULT fr = f_getfree("0:", &free_clusters, &fatfs);
    if (fr != FR_OK || fatfs == nullptr) {
        ESP_LOGE(TAG, "freeSpace: f_getfree failed with FRESULT %d",
                 static_cast<int>(fr));
        return ESP_FAIL;
    }
    const uint64_t cluster_bytes =
        static_cast<uint64_t>(fatfs->csize) * fatfs->ssize;
    const uint64_t total = cluster_bytes * (fatfs->n_fatent - 2);
    const uint64_t free = cluster_bytes * free_clusters;
    const uint64_t used = total - free;
    out->total_bytes = total;
    out->used_bytes = used;
    out->free_bytes = free;
    return ESP_OK;
}

const char* SdStorageBackend::mountPoint() const { return kMountPoint; }

}  // namespace visioncam