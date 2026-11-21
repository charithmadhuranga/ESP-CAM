// storage_backend.h — IStorageBackend interface and SD implementation
// (plan 02 task 2.1; SPEC sections 4.8, 4.9).
//
// Every public API takes opaque media IDs (med-…, rec-…, evt-…), never
// filesystem paths (AGENTS rule 15, SPEC 6.0). Path traversal is rejected
// even though FAT has no symlinks — defence in depth.
//
// StorageManager is the sole filesystem owner (AGENTS rule 10): nothing
// else calls fopen/fwrite/fclose/remove/rename/mkdir. All file I/O goes
// through IStorageBackend implementations.
//
// Backend contract:
//   - begin()        initialise the backend (mount, scan, open index…)
//   - isAvailable()  true when media can be written/read
//   - writeFile()    write bytes to an opaque-path file (create/overwrite)
//   - appendFile()   append bytes to an opaque-path file (for JSONL index)
//   - readFile()     read bytes from an opaque-path file
//   - open/read/seek/closeReadFile() bounded sequential file reads
//   - removeFile()   delete an opaque-path file
//   - renameFile()   atomically rename (temp → final)
//   - statFile()     get size/modtime
//   - listDir()      bounded directory listing
//   - freeSpace()    total/used/free bytes
//
// StorageManager builds on top: opaque ID allocation, JSONL indexes,
// snapshot lifecycle, retention, and the /storage API surface.

#pragma once

#include <cstddef>
#include <cstdint>

#include "esp_err.h"

namespace visioncam {

// Opaque media ID prefix (SPEC 6.0, §4.8 index design).
constexpr char kMediaIdPrefix[] = "med-";
constexpr char kEventIdPrefix[] = "evt-";
constexpr char kRecordingIdPrefix[] = "rec-";

// Media types (matches JSONL index "type" field).
enum class MediaType : uint8_t {
    Snapshot = 0,
    Recording,
    Thumbnail,
    TimelapseFrame,
    TimelapseOutput,
    Model,
    Log,
};

// Media category (SPEC 6.5 filter values).
enum class MediaCategory : uint8_t {
    Manual = 0,
    Motion,
    Detection,
    Timelapse,
};

// Storage backends.
enum class StorageBackendType : uint8_t {
    Sd = 0,   // onboard microSD via FATFS VFS
    Network,  // optional network storage (Phase 8)
};

// Result of a directory entry read.
struct DirEntry {
    char name[64];       // filename only, NUL-terminated
    bool is_dir = false;
    uint32_t size = 0;   // bytes, 0 for directories
    uint32_t mtime = 0;  // seconds since epoch (FAT timestamp converted)
};

// Result of a free space query.
struct FreeSpace {
    uint64_t total_bytes = 0;
    uint64_t used_bytes = 0;
    uint64_t free_bytes = 0;
};

// IStorageBackend — one implementation per storage medium (SPEC 4.9).
class IStorageBackend {
public:
    virtual ~IStorageBackend() = default;

    // Initialise the backend (mount, scan, open indexes…). Call once.
    virtual esp_err_t begin() = 0;

    // True when media can be written/read.
    virtual bool isAvailable() const = 0;

    // Write len bytes to path. Creates parent dirs as needed. Overwrites.
    virtual esp_err_t writeFile(const char* path, const uint8_t* data,
                                size_t len) = 0;

    // Append len bytes to path. Creates parent dirs as needed. Used for
    // JSONL index lines.
    virtual esp_err_t appendFile(const char* path, const uint8_t* data,
                                 size_t len) = 0;

    // Read up to len bytes from path into data. Returns actual bytes read.
    virtual esp_err_t readFile(const char* path, uint8_t* data,
                               size_t len, size_t* out_len) = 0;

    // Open a file for bounded sequential reads. The returned handle is owned
    // by the backend and must be released with closeReadFile().
    virtual esp_err_t openReadFile(const char* path, void** out_handle) = 0;
    virtual esp_err_t readOpenFile(void* handle, uint8_t* data, size_t len,
                                   size_t* out_len) = 0;
    virtual esp_err_t seekReadFile(void* handle, uint32_t offset) = 0;
    virtual esp_err_t closeReadFile(void* handle) = 0;

    // Delete path. Returns ESP_ERR_NOT_FOUND if absent.
    virtual esp_err_t removeFile(const char* path) = 0;

    // Atomically rename src → dst (used to finalise temp files).
    virtual esp_err_t renameFile(const char* src, const char* dst) = 0;

    // Stat path: size and modification time.
    virtual esp_err_t statFile(const char* path, uint32_t* out_size,
                               uint32_t* out_mtime) = 0;

    // List directory entries, bounded by max_entries. Returns count filled.
    virtual esp_err_t listDir(const char* dir, DirEntry* entries,
                              size_t max_entries, size_t* out_count) = 0;

    // Total/used/free bytes on the backing volume.
    virtual esp_err_t freeSpace(FreeSpace* out) = 0;

    // Mount point prefix (e.g. "/sdcard").
    virtual const char* mountPoint() const = 0;
};

}  // namespace visioncam