// sd_storage_backend.h — IStorageBackend over the onboard microSD
// (plan 02 task 2.1; SPEC sections 4.8, 4.9).
//
// Wraps the ESP-IDF FATFS VFS mount at /sdcard established by
// firmware/src/sd_bringup.cpp. All file I/O goes through C stdio here —
// this module (storage/) is the sole filesystem owner (AGENTS rule 10).

#pragma once

#include "storage_backend.h"

namespace visioncam {

// FATFS-backed implementation of IStorageBackend for the onboard microSD.
// Use storage_managerInit() with an instance of this class.
class SdStorageBackend : public IStorageBackend {
public:
    esp_err_t begin() override;
    bool isAvailable() const override;

    esp_err_t writeFile(const char* path, const uint8_t* data,
                        size_t len) override;
    esp_err_t appendFile(const char* path, const uint8_t* data,
                         size_t len) override;
    esp_err_t readFile(const char* path, uint8_t* data, size_t len,
                       size_t* out_len) override;
    esp_err_t removeFile(const char* path) override;
    esp_err_t renameFile(const char* src, const char* dst) override;
    esp_err_t statFile(const char* path, uint32_t* out_size,
                       uint32_t* out_mtime) override;
    esp_err_t listDir(const char* dir, DirEntry* entries,
                      size_t max_entries, size_t* out_count) override;
    esp_err_t freeSpace(FreeSpace* out) override;
    const char* mountPoint() const override;
};

}  // namespace visioncam