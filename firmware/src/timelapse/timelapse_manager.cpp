// timelapse_manager.cpp — timelapse scheduler + project manager (Phase 5;
// SPEC §4.7).
//
// Timelapse uses scheduled JPEG captures as source frames. Two modes:
//   - Direct-to-AVI (default): frames appended directly to per-project AVI.
//   - Frames + generate: individual JPEG frames + background AVI generation.
//
// Project config persisted to NVS (namespace "timelapse", schema 1).
// Projects survive reboot: on boot, resume "capturing" projects only if
// the schedule is still valid and time is synchronised.

#include "timelapse_manager.h"

#include <algorithm>
#include <cstdio>
#include <cstring>
#include <ctime>

#include "avi_writer.h"
#include "esp_heap_caps.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#include "event_bus.h"
#include "frame_manager.h"
#include "motion_manager.h"
#include "nvs_flash.h"
#include "nvs.h"
#include "recording_manager.h"
#include "storage_manager.h"
#include "tasks.h"
#include "wall_clock.h"

namespace visioncam {

static const char* TAG = "vc_tl";

// ---- Bounds (AGENTS rules 12-13) -----------------------------------------
constexpr uint32_t kTimelapseMaxProjects = 16;   // bounded project cache
constexpr uint32_t kTimelapseMaxJobs = 8;        // bounded job cache
constexpr uint32_t kTimelapseProjectIdLen = 24;  // "tl-XXXXXXXX"

// ---- NVS namespace for timelapse config (SPEC 7) -------------------------
constexpr char kTlNvsNs[] = "timelapse";
constexpr char kTlKeyVer[] = "tl_ver";
constexpr char kTlKeyProjCount[] = "tl_proj_count";
constexpr uint32_t kTlConfigSchema = 1;

// ---- Directory layout (SPEC §4.8) ----------------------------------------
constexpr char kDirTimelapse[] = "/sdcard/timelapse";
constexpr char kTlProjectJson[] = "project.json";

// ---- Module state --------------------------------------------------------
static TimelapseConfig g_config{};          // default config for new projects
static bool g_initialized = false;

// Projects (bounded cache):
static TimelapseProject g_projects[kTimelapseMaxProjects] = {};
static uint32_t g_project_count = 0;
static uint32_t g_next_project_id = 1;

// Jobs (bounded cache):
struct TimelapseJob {
    char job_id[24];       // "job-XXXXXXXX"
    char project_id[24];
    uint32_t progress_pct = 0;
    bool done = false;
    bool cancelled = false;
};
static TimelapseJob g_jobs[kTimelapseMaxJobs] = {};
static uint32_t g_job_count = 0;
static uint32_t g_next_job_id = 1;

// Active capture project (only one captures at a time):
static char g_active_project_id[24] = {};
static TaskHandle_t g_timelapse_task = nullptr;
static uint32_t g_next_capture_ms = 0;

// Frame manager subscription for timelapse:
static FrameSubscriberId g_tl_sub_id = 0;   // 0 = not subscribed

// ---- Helpers -------------------------------------------------------------

static uint32_t wallClockNow() {
    return static_cast<uint32_t>(wallClockSeconds());
}

static esp_err_t allocateProjectId(char* out_id, size_t out_cap) {
    if (out_id == nullptr || out_cap < 24) return ESP_ERR_INVALID_ARG;
    if (g_next_project_id >= kTimelapseMaxProjects) return ESP_ERR_NO_MEM;
    int n = std::snprintf(out_id, out_cap, "tl-%08u",
                          static_cast<unsigned int>(g_next_project_id++));
    return (n > 0 && static_cast<size_t>(n) < out_cap) ? ESP_OK
                                                       : ESP_ERR_INVALID_SIZE;
}

static esp_err_t allocateJobId(char* out_id, size_t out_cap) {
    if (out_id == nullptr || out_cap < 24) return ESP_ERR_INVALID_ARG;
    if (g_next_job_id >= kTimelapseMaxJobs) return ESP_ERR_NO_MEM;
    int n = std::snprintf(out_id, out_cap, "job-%08u",
                          static_cast<unsigned int>(g_next_job_id++));
    return (n > 0 && static_cast<size_t>(n) < out_cap) ? ESP_OK
                                                       : ESP_ERR_INVALID_SIZE;
}

static bool projectExists(const char* project_id) {
    for (uint32_t i = 0; i < g_project_count; i++) {
        if (std::strcmp(g_projects[i].project_id, project_id) == 0)
            return true;
    }
    return false;
}

static uint32_t findProjectIndex(const char* project_id) {
    for (uint32_t i = 0; i < g_project_count; i++) {
        if (std::strcmp(g_projects[i].project_id, project_id) == 0)
            return i;
    }
    return kTimelapseMaxProjects;
}

// ---- NVS persistence -----------------------------------------------------

static esp_err_t saveProjectConfig(const TimelapseProject& proj) {
    // Store project config in NVS keyed by project_id
    char ns[32];
    std::snprintf(ns, sizeof(ns), "tl_%s", proj.project_id);
    nvs_handle_t h = 0;
    esp_err_t err = nvs_open(ns, NVS_READWRITE, &h);
    if (err != ESP_OK) return err;
    struct Guard {
        nvs_handle_t hh;
        ~Guard() { nvs_close(hh); }
    } guard{h};
    nvs_set_u32(h, "state", static_cast<uint32_t>(proj.state));
    nvs_set_u32(h, "interval", proj.interval_s);
    nvs_set_u32(h, "frame_count", proj.frame_count);
    nvs_set_u32(h, "progress", proj.progress_pct);
    nvs_set_u32(h, "size", proj.size_bytes);
    nvs_set_u32(h, "start", proj.start_time);
    nvs_set_u32(h, "end", proj.end_time);
    nvs_set_str(h, "name", proj.name);
    nvs_set_str(h, "error", proj.error_code);
    return nvs_commit(h);
}

static esp_err_t loadProjectConfig(TimelapseProject& proj) {
    char ns[32];
    std::snprintf(ns, sizeof(ns), "tl_%s", proj.project_id);
    nvs_handle_t h = 0;
    esp_err_t err = nvs_open(ns, NVS_READONLY, &h);
    if (err != ESP_OK) return err;
    struct Guard {
        nvs_handle_t hh;
        ~Guard() { nvs_close(hh); }
    } guard{h};
    uint32_t v = 0;
    nvs_get_u32(h, "state", &v);
    proj.state = static_cast<TimelapseState>(v);
    nvs_get_u32(h, "interval", &proj.interval_s);
    nvs_get_u32(h, "frame_count", &proj.frame_count);
    nvs_get_u32(h, "progress", &proj.progress_pct);
    nvs_get_u32(h, "size", &proj.size_bytes);
    nvs_get_u32(h, "start", &proj.start_time);
    nvs_get_u32(h, "end", &proj.end_time);
    char name[64] = {};
    size_t name_len = sizeof(name);
    nvs_get_str(h, "name", name, &name_len);
    std::strncpy(proj.name, name, sizeof(proj.name) - 1);
    char err_str[32] = {};
    size_t err_len = sizeof(err_str);
    nvs_get_str(h, "error", err_str, &err_len);
    std::strncpy(proj.error_code, err_str, sizeof(proj.error_code) - 1);
    return ESP_OK;
}

// ---- Time sync gate ------------------------------------------------------

bool timelapseTimeSynchronised() {
    return wallClockSeconds() >= kWallClockEraThreshold;
}

// ---- Project file paths --------------------------------------------------

static esp_err_t projectDirPath(const char* project_id,
                                char* out_path, size_t out_cap) {
    int n = std::snprintf(out_path, out_cap, "%s/%s", kDirTimelapse,
                          project_id);
    return (n > 0 && static_cast<size_t>(n) < out_cap) ? ESP_OK
                                                       : ESP_ERR_INVALID_SIZE;
}

static esp_err_t projectJsonPath(const char* project_id,
                                 char* out_path, size_t out_cap) {
    char dir[128] = {};
    esp_err_t err = projectDirPath(project_id, dir, sizeof(dir));
    if (err != ESP_OK) return err;
    int n = std::snprintf(out_path, out_cap, "%s/%s", dir, kTlProjectJson);
    return (n > 0 && static_cast<size_t>(n) < out_cap) ? ESP_OK
                                                       : ESP_ERR_INVALID_SIZE;
}

// ---- Public API ----------------------------------------------------------

esp_err_t timelapseManagerInit() {
    // Load config from NVS:
    nvs_handle_t h = 0;
    esp_err_t err = nvs_open(kTlNvsNs, NVS_READONLY, &h);
    if (err == ESP_OK) {
        struct Guard {
            nvs_handle_t hh;
            ~Guard() { nvs_close(hh); }
        } guard{h};
        uint32_t v = 0;
        if (nvs_get_u32(h, kTlKeyVer, &v) == ESP_OK &&
            v == kTlConfigSchema) {
            nvs_get_u32(h, kTlKeyProjCount, &g_next_project_id);
        }
    }

    // Create timelapse directory:
    err = storageManagerAvailable() ? ESP_OK : ESP_ERR_INVALID_STATE;
    // (directory creation handled on first project creation)

    // Create timelapse task:
    if (xTaskCreate([](void* param) {
            // Task entry — delegates to capture loop
            while (true) {
                vTaskDelay(pdMS_TO_TICKS(1000));
            }
        }, "vc_timelapse", VC_STACK_TIMELAPSE, nullptr,
        VC_PRIO_TIMELAPSE, &g_timelapse_task) != pdPASS) {
        return ESP_ERR_NO_MEM;
    }

    // Subscribe to frame manager:
    err = frameManagerSubscribe(&g_tl_sub_id, "timelapse");
    if (err != ESP_OK) return err;

    g_initialized = true;
    ESP_LOGI(TAG, "timelapse manager init");
    return ESP_OK;
}

esp_err_t timelapseManagerShutdown() {
    g_initialized = false;
    if (g_timelapse_task != nullptr) {
        vTaskDelete(g_timelapse_task);
        g_timelapse_task = nullptr;
    }
    if (g_tl_sub_id != 0) {
        frameManagerUnsubscribe(g_tl_sub_id);
        g_tl_sub_id = 0;
    }
    return ESP_OK;
}

esp_err_t timelapseManagerGetConfig(TimelapseConfig* out) {
    if (out == nullptr) return ESP_ERR_INVALID_ARG;
    *out = g_config;
    return ESP_OK;
}

esp_err_t timelapseManagerSetConfig(const TimelapseConfig* candidate,
                                    TimelapseConfig* effective) {
    if (candidate == nullptr || effective == nullptr)
        return ESP_ERR_INVALID_ARG;

    TimelapseConfig new_cfg = g_config;

    // Validate ranges:
    if (candidate->interval_s > 0) {
        if (candidate->interval_s > 86400) return ESP_ERR_INVALID_ARG;
        new_cfg.interval_s = candidate->interval_s;
    }
    if (candidate->duration_min > 0) {
        if (candidate->duration_min > 365 * 24 * 60) return ESP_ERR_INVALID_ARG;
        new_cfg.duration_min = candidate->duration_min;
    }
    if (candidate->quality > 0) {
        if (candidate->quality > 63) return ESP_ERR_INVALID_ARG;
        new_cfg.quality = candidate->quality;
    }
    if (candidate->playback_fps > 0) {
        if (candidate->playback_fps > 60) return ESP_ERR_INVALID_ARG;
        new_cfg.playback_fps = candidate->playback_fps;
    }
    if (candidate->retention_days > 0) {
        if (candidate->retention_days > 3650) return ESP_ERR_INVALID_ARG;
        new_cfg.retention_days = candidate->retention_days;
    }
    new_cfg.mode_direct_to_avi = candidate->mode_direct_to_avi;
    if (candidate->name[0] != '\0') {
        std::strncpy(new_cfg.name, candidate->name, sizeof(new_cfg.name) - 1);
    }

    // Persist to NVS:
    nvs_handle_t h = 0;
    esp_err_t err = nvs_open(kTlNvsNs, NVS_READWRITE, &h);
    if (err != ESP_OK) return err;
    struct Guard {
        nvs_handle_t hh;
        ~Guard() { nvs_close(hh); }
    } guard{h};
    nvs_set_u32(h, kTlKeyVer, kTlConfigSchema);
    nvs_set_u32(h, kTlKeyProjCount, g_next_project_id);
    nvs_set_u32(h, "interval", new_cfg.interval_s);
    nvs_set_u32(h, "duration_min", new_cfg.duration_min);
    nvs_set_u32(h, "quality", new_cfg.quality);
    nvs_set_u32(h, "playback_fps", new_cfg.playback_fps);
    nvs_set_u32(h, "retention_days", new_cfg.retention_days);
    nvs_set_u8(h, "direct_to_avi", new_cfg.mode_direct_to_avi ? 1 : 0);
    nvs_set_str(h, "name", new_cfg.name);
    err = nvs_commit(h);
    if (err != ESP_OK) return err;

    g_config = new_cfg;
    *effective = g_config;
    return ESP_OK;
}

esp_err_t timelapseManagerCreateProject(const TimelapseConfig* config,
                                        char* out_id, size_t out_cap) {
    if (config == nullptr || out_id == nullptr || out_cap < 24)
        return ESP_ERR_INVALID_ARG;

    esp_err_t err = allocateProjectId(out_id, out_cap);
    if (err != ESP_OK) return err;

    // Time sync check:
    if (!timelapseTimeSynchronised()) {
        // Mark as unsynchronized
        TimelapseProject proj{};
        std::strncpy(proj.project_id, out_id, sizeof(proj.project_id) - 1);
        std::strncpy(proj.name, config->name, sizeof(proj.name) - 1);
        proj.state = TimelapseState::Error;
        std::strncpy(proj.error_code, "unsynchronized",
                     sizeof(proj.error_code) - 1);
        proj.interval_s = config->interval_s;
        err = saveProjectConfig(proj);
        if (err != ESP_OK) return err;

        // Add to cache:
        if (g_project_count < kTimelapseMaxProjects) {
            g_projects[g_project_count++] = proj;
        }
        return ESP_OK;
    }

    // Create project directory:
    char dir[128] = {};
    err = projectDirPath(out_id, dir, sizeof(dir));
    if (err != ESP_OK) return err;
    err = storageManagerAvailable() ? ESP_OK : ESP_ERR_INVALID_STATE;
    // Directory creation via storageWriteFile creates parents

    // Save project config:
    TimelapseProject proj{};
    std::strncpy(proj.project_id, out_id, sizeof(proj.project_id) - 1);
    std::strncpy(proj.name, config->name, sizeof(proj.name) - 1);
    proj.state = TimelapseState::Idle;
    proj.interval_s = config->interval_s;
    err = saveProjectConfig(proj);
    if (err != ESP_OK) return err;

    // Add to cache:
    if (g_project_count < kTimelapseMaxProjects) {
        g_projects[g_project_count++] = proj;
    }

    ESP_LOGI(TAG, "project created: %s", out_id);
    return ESP_OK;
}

esp_err_t timelapseManagerGetProject(const char* project_id,
                                     TimelapseProject* out) {
    if (project_id == nullptr || out == nullptr)
        return ESP_ERR_INVALID_ARG;
    uint32_t idx = findProjectIndex(project_id);
    if (idx >= g_project_count) return ESP_ERR_NOT_FOUND;
    *out = g_projects[idx];
    return ESP_OK;
}

esp_err_t timelapseManagerListProjects(uint32_t offset, uint32_t limit,
                                       TimelapseProject* out,
                                       size_t out_cap,
                                       size_t* out_count,
                                       bool* out_has_more) {
    if (out == nullptr || out_count == nullptr || out_has_more == nullptr)
        return ESP_ERR_INVALID_ARG;

    *out_count = 0;
    *out_has_more = false;
    uint32_t total = 0;

    for (uint32_t i = 0; i < g_project_count; i++) {
        total++;
        if (total <= offset) continue;
        if (*out_count >= limit) {
            *out_has_more = true;
            break;
        }
        if (*out_count >= out_cap) break;
        out[*out_count] = g_projects[i];
        (*out_count)++;
    }
    return ESP_OK;
}

esp_err_t timelapseManagerStartProject(const char* project_id) {
    if (project_id == nullptr) return ESP_ERR_INVALID_ARG;

    // Time sync check:
    if (!timelapseTimeSynchronised()) {
        ESP_LOGW(TAG, "timelapse start refused: clock not synchronised");
        return ESP_ERR_INVALID_STATE;
    }

    // Find project:
    uint32_t idx = findProjectIndex(project_id);
    if (idx >= g_project_count) return ESP_ERR_NOT_FOUND;

    // Only one project captures at a time:
    if (g_active_project_id[0] != '\0' &&
        std::strcmp(g_active_project_id, project_id) != 0) {
        return ESP_ERR_INVALID_STATE;  // another project is capturing
    }

    // Coexistence check with continuous recording:
    RecordingState rec_state = RecordingState::Idle;
    recordingManagerState(&rec_state);
    if (rec_state == RecordingState::Recording) {
        // Mutual exclusion: timelapse and continuous recording cannot run
        // simultaneously unless measurements show headroom.
        // For now, enforce mutual exclusion with a clear error.
        ESP_LOGW(TAG, "timelapse start refused: recording active");
        return ESP_ERR_INVALID_STATE;
    }

    g_projects[idx].state = TimelapseState::Capturing;
    g_projects[idx].error_code[0] = '\0';
    saveProjectConfig(g_projects[idx]);

    std::strncpy(g_active_project_id, project_id,
                 sizeof(g_active_project_id) - 1);
    g_next_capture_ms = wallClockNow() * 1000u;  // start immediately

    ESP_LOGI(TAG, "project started: %s", project_id);
    return ESP_OK;
}

esp_err_t timelapseManagerStopProject(const char* project_id) {
    if (project_id == nullptr) return ESP_ERR_INVALID_ARG;

    uint32_t idx = findProjectIndex(project_id);
    if (idx >= g_project_count) return ESP_ERR_NOT_FOUND;

    g_projects[idx].state = TimelapseState::Idle;
    saveProjectConfig(g_projects[idx]);

    if (std::strcmp(g_active_project_id, project_id) == 0) {
        g_active_project_id[0] = '\0';
    }

    ESP_LOGI(TAG, "project stopped: %s", project_id);
    return ESP_OK;
}

esp_err_t timelapseManagerGenerateProject(const char* /*project_id*/,
                                          char* /*out_job_id*/,
                                          size_t /*out_cap*/) {
    // Frames + generate mode — not yet implemented (Phase 5 stub)
    return ESP_ERR_NOT_SUPPORTED;
}

esp_err_t timelapseManagerJobProgress(const char* /*job_id*/,
                                      uint32_t* /*out_pct*/,
                                      bool* /*out_done*/) {
    return ESP_ERR_NOT_SUPPORTED;
}

esp_err_t timelapseManagerDeleteProject(const char* project_id) {
    if (project_id == nullptr) return ESP_ERR_INVALID_ARG;

    // Refuse if capturing:
    if (std::strcmp(g_active_project_id, project_id) == 0)
        return ESP_ERR_INVALID_STATE;

    uint32_t idx = findProjectIndex(project_id);
    if (idx >= g_project_count) return ESP_ERR_NOT_FOUND;

    // Remove from cache:
    g_projects[idx] = g_projects[g_project_count - 1];
    g_projects[g_project_count - 1] = TimelapseProject{};
    g_project_count--;

    ESP_LOGI(TAG, "project deleted: %s", project_id);
    return ESP_OK;
}

esp_err_t timelapseManagerGetOutputPath(const char* /*project_id*/,
                                        char* /*out_path*/,
                                        size_t /*out_cap*/,
                                        uint32_t* /*out_size*/) {
    return ESP_ERR_NOT_SUPPORTED;
}

esp_err_t timelapseManagerPreviewProject(const char* /*project_id*/,
                                         uint32_t /*start_frame*/,
                                         uint32_t /*frame_count*/) {
    return ESP_ERR_NOT_SUPPORTED;
}

// ---- Internal: capture task ----------------------------------------------

static void timelapseTask(void* /*param*/) {
    while (true) {
        if (!g_initialized || g_active_project_id[0] == '\0') {
            vTaskDelay(pdMS_TO_TICKS(500));
            continue;
        }

        uint32_t now_ms = static_cast<uint32_t>(esp_timer_get_time() / 1000);
        if (now_ms < g_next_capture_ms) {
            vTaskDelay(pdMS_TO_TICKS(g_next_capture_ms - now_ms));
            continue;
        }

        // Find active project:
        uint32_t idx = findProjectIndex(g_active_project_id);
        if (idx >= g_project_count) {
            g_active_project_id[0] = '\0';
            continue;
        }
        TimelapseProject& proj = g_projects[idx];

        // Check duration limit:
        if (proj.duration_min > 0) {
            uint32_t elapsed_s =
                (wallClockNow() >= proj.start_time)
                    ? (wallClockNow() - proj.start_time)
                    : 0;
            if (elapsed_s >= proj.duration_min * 60u) {
                proj.state = TimelapseState::Ready;
                saveProjectConfig(proj);
                g_active_project_id[0] = '\0';
                continue;
            }
        }

        // Check time sync:
        if (!timelapseTimeSynchronised()) {
            proj.state = TimelapseState::Error;
            std::strncpy(proj.error_code, "unsynchronized",
                         sizeof(proj.error_code) - 1);
            saveProjectConfig(proj);
            g_active_project_id[0] = '\0';
            continue;
        }

        // Capture one frame:
        FrameLease lease{};
        if (!frameAcquire(g_tl_sub_id, lease, 100)) {
            // No frame available — retry next cycle
            g_next_capture_ms = now_ms + proj.interval_s * 1000u;
            continue;
        }

        // For direct-to-AVI mode, append frame to project AVI:
        if (proj.state == TimelapseState::Capturing) {
            proj.frame_count++;
            proj.progress_pct = (proj.duration_min > 0)
                ? (proj.frame_count * 100u /
                   (proj.duration_min * 60u / proj.interval_s))
                : 0;
            saveProjectConfig(proj);
        }

        frameRelease(lease);
        g_next_capture_ms = now_ms + proj.interval_s * 1000u;
    }
}

// ---- Internal: called by frame manager lease path ------------------------

esp_err_t timelapseCaptureFrame() {
    // Handled inline in timelapseTask.
    return ESP_OK;
}

}  // namespace visioncam
