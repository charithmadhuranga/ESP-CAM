// frame_manager.cpp — see frame_manager.h.
//
// The single place in the firmware that calls esp_camera_fb_get() and
// esp_camera_fb_return() (AGENTS rule 9). The capture task runs one loop:
//
//   acquire -> validate (SPEC 4.1.1 rule 7) -> record into the pool ->
//   push to every subscriber's bounded queue (drop-oldest per slow
//   consumer) -> drop our own reference -> repeat
//
// and the ONLY long-lived buffer ownership is the pool record's
// refcount, so a slow consumer can never wedge capture: at most
// VC_FRAME_POOL records exist, and when they are all referenced the new
// frame is dropped and counted instead of blocking.

#include "frame_manager.h"

#include <atomic>
#include <cstring>

#include "esp_camera.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"
#include "freertos/task.h"

#include "camera_manager.h"
#include "camera_recovery.h"
#include "ring_log.h"
#include "tasks.h"

namespace visioncam {
namespace {

const char* TAG = "vc_frame";

constexpr uint32_t kFbGetTimeoutMs = 200;   // keeps NULL handling & stops responsive
constexpr uint32_t kParkPollMs     = 50;    // wait granularity while stopped

// --- the bounded pool (VC_FRAME_POOL records) ----------------------------
// refs counts: 1 per subscriber queue holding the record + 1 per
// outstanding lease + 1 while the capture task still owns it. The camera
// buffer goes back to the driver exactly when refs hits 0.
struct Record {
    camera_fb_t*     fb = nullptr;
    std::atomic<uint32_t> refs{0};
    uint32_t         id = 0;
    uint64_t         ts_us = 0;
    framesize_t      framesize = FRAMESIZE_VGA;   // sampled at capture time
};

Record s_pool[VC_FRAME_POOL];

struct Subscriber {
    bool          used = false;
    char          name[16] = {};
    QueueHandle_t queue = nullptr;
    uint32_t      dropped = 0;
};

Subscriber s_subs[VC_FRAME_SUBSCRIBERS];

std::atomic<bool>     s_running{false};
std::atomic<bool>     s_camera_gone{false};
std::atomic<bool>     s_task_alive{false};
std::atomic<bool>     s_task_parked{false};   // set when the task observes s_running==false
std::atomic<uint32_t> s_next_id{1};
std::atomic<uint32_t> s_frames_captured{0};
std::atomic<uint32_t> s_invalid_dropped{0};
std::atomic<uint32_t> s_pool_dropped{0};
std::atomic<uint32_t> s_leases_taken{0};
std::atomic<uint32_t> s_leases_released{0};
std::atomic<uint32_t> s_last_len{0};
std::atomic<uint64_t> s_last_ts{0};
TaskHandle_t          s_task = nullptr;

uint64_t nowUs() { return esp_timer_get_time(); }

// Takes a record with refs == 0, or nullptr when the pool is exhausted.
// Static pool: no allocation ever happens here (AGENTS rule 13).
Record* poolTake() {
    for (unsigned i = 0; i < VC_FRAME_POOL; ++i) {
        uint32_t expected = 0;
        if (s_pool[i].refs.compare_exchange_strong(expected, 1)) {
            return &s_pool[i];
        }
    }
    return nullptr;
}

// Releases one reference; returns the camera buffer at zero.
void recordUnref(Record* rec) {
    if (rec == nullptr) return;
    if (rec->refs.fetch_sub(1) != 1) return;   // still referenced
    if (rec->fb != nullptr) {
        esp_camera_fb_return(rec->fb);        // the ONLY return() call site
        rec->fb = nullptr;
    }
}

// Pushes one frame record to every subscriber. Drop-oldest per consumer
// when that consumer is behind; the dropped record loses the queue's
// reference (a leased one survives in the consumer's hands).
void distribute(Record* rec) {
    for (unsigned i = 0; i < VC_FRAME_SUBSCRIBERS; ++i) {
        Subscriber& sub = s_subs[i];
        if (!sub.used || sub.queue == nullptr) continue;
        if (xQueueSend(sub.queue, &rec, 0) == pdPASS) {
            rec->refs.fetch_add(1);           // the queue's reference
            continue;
        }
        // Slow consumer: the queued frame is replaced by this one.
        Record* old = nullptr;
        if (xQueueReceive(sub.queue, &old, 0) == pdPASS && old != nullptr) {
            sub.dropped++;
            recordUnref(old);
            if (xQueueSend(sub.queue, &rec, 0) == pdPASS) {
                rec->refs.fetch_add(1);
                continue;
            }
        }
        // Still full (should not happen): count and move on. Never block.
        sub.dropped++;
    }
}

// SPEC 9.1 rule 4: terminal camera failure — stop acquisition cleanly.
// The task parks (a later frameManagerStart() resumes it), consumers
// stop receiving frames and their frameAcquire() calls time out; the
// ladder already published CameraFailed.
void stopOnCameraFailure() {
    s_running.store(false);
    s_camera_gone.store(true);
    ESP_LOGE(TAG, "camera failed — frame acquisition stopped (consumers "
                  "stop cleanly; leases already handed out stay valid)");
    VC_RING_ERROR("frame", "capture stopped — camera failed");
}

void captureTask(void* /*arg*/) {
    s_task_alive.store(true);
    for (;;) {
        if (!s_running.load()) {
            s_task_parked.store(true);   // handshake for frameManagerStop()
            ulTaskNotifyTake(pdTRUE, pdMS_TO_TICKS(kParkPollMs));
            s_task_parked.store(false);
            continue;
        }

        camera_fb_t* fb = esp_camera_fb_get();   // R2 blocks internally with FB_GET_TIMEOUT
        if (fb == nullptr) {
            // SPEC 9.1: the ladder runs in OUR task and yields ~10 ms on
            // every NULL (frame_manager.h: rule 1, published via
            // cameraRecoveryYieldMs()).
            const NullFrameVerdict v = cameraRecoveryNoteNullFrame();
            vTaskDelay(pdMS_TO_TICKS(cameraRecoveryYieldMs()));
            if (v == NullFrameVerdict::Failed) {
                stopOnCameraFailure();
            }
            continue;
        }
        cameraRecoveryNoteGoodFrame();   // SPEC 9.1 rule 3

        // SPEC 4.1.1 rule 7: drop and count len == 0 or > capacity.
        const size_t capacity = frameJpegCapacity();
        if (fb->len == 0 || (capacity > 0 && fb->len > capacity)) {
            s_invalid_dropped.fetch_add(1);
            ESP_LOGE(TAG, "frame dropped (rule 7): len=%u capacity=%u",
                     static_cast<unsigned>(fb->len),
                     static_cast<unsigned>(capacity));
            esp_camera_fb_return(fb);
            taskYIELD();  // don't tight-loop on rule-7 drops
            continue;
        }

        Record* rec = poolTake();
        if (rec == nullptr) {
            // Back-pressure: every record is referenced by a lease or a
            // full consumer queue. The NEW frame is dropped — capture
            // never blocks (header: bounded, drop policy documented).
            s_pool_dropped.fetch_add(1);
            esp_camera_fb_return(fb);
            taskYIELD();  // don't tight-loop when the pool is exhausted
            continue;
        }

        rec->fb = fb;
        rec->id = s_next_id.fetch_add(1);
        rec->ts_us = nowUs();
        rec->framesize = cameraManagerFramesize();
        s_frames_captured.fetch_add(1);
        s_last_len.store(fb->len);
        s_last_ts.store(rec->ts_us);

        distribute(rec);          // adds per-queue references
        recordUnref(rec);         // drops the capture task's own reference
        // R1 runs its capture task at a fixed period precisely to avoid a
        // tight loop starving the other core. Yield here so app_main and
        // equal-priority tasks on this core get CPU between frames;
        // without it a fast camera wedges the core and trips the interrupt
        // watchdog (reproduced: "Interrupt wdt timeout on CPU1").
        taskYIELD();
    }
}

}  // namespace

size_t frameJpegCapacity() { return cameraManagerJpegCapacity(); }

esp_err_t frameManagerInit() {
    if (s_task_alive.load()) return ESP_ERR_INVALID_STATE;
    s_task_parked.store(false);
    // Pinned to core 1 (the application core): R2's cam_task owns core 0
    // (SPEC 4.1.1 rule 10 — "pin deliberately and measure"), and the
    // capture task must not compete with it for the same core. Priority 6
    // keeps it above the HTTP/stream work it feeds.
    const BaseType_t ok = xTaskCreatePinnedToCore(
        captureTask, "vc_capture", VC_STACK_CAPTURE, nullptr,
        VC_PRIO_CAPTURE, &s_task, 1);
    if (ok != pdPASS) {
        ESP_LOGE(TAG, "capture task create failed — no frames can be served");
        return ESP_ERR_NO_MEM;
    }
    ESP_LOGI(TAG, "frame manager up: task prio=%d stack=%u B, pool=%u "
                  "records, %u subscriber slots",
             VC_PRIO_CAPTURE, static_cast<unsigned>(VC_STACK_CAPTURE),
             static_cast<unsigned>(VC_FRAME_POOL),
             static_cast<unsigned>(VC_FRAME_SUBSCRIBERS));
    return ESP_OK;
}

esp_err_t frameManagerSubscribe(FrameSubscriberId* out_id, const char* name) {
    if (out_id == nullptr) return ESP_ERR_INVALID_ARG;
    for (unsigned i = 0; i < VC_FRAME_SUBSCRIBERS; ++i) {
        if (s_subs[i].used) continue;
        s_subs[i].queue = xQueueCreate(VC_FRAME_QUEUE_DEPTH, sizeof(Record*));
        if (s_subs[i].queue == nullptr) return ESP_ERR_NO_MEM;
        s_subs[i].used = true;
        s_subs[i].dropped = 0;
        std::strncpy(s_subs[i].name, name != nullptr ? name : "consumer",
                     sizeof(s_subs[i].name) - 1);
        *out_id = static_cast<FrameSubscriberId>(i);
        ESP_LOGI(TAG, "subscriber %u registered (%s), queue depth %u",
                 i, s_subs[i].name, static_cast<unsigned>(VC_FRAME_QUEUE_DEPTH));
        return ESP_OK;
    }
    ESP_LOGE(TAG, "subscriber registry full (%u slots)",
             static_cast<unsigned>(VC_FRAME_SUBSCRIBERS));
    return ESP_ERR_NO_MEM;
}

esp_err_t frameManagerUnsubscribe(FrameSubscriberId id) {
    if (id >= VC_FRAME_SUBSCRIBERS || !s_subs[id].used) return ESP_ERR_INVALID_ARG;
    Subscriber& sub = s_subs[id];
    // Drain what was queued: each entry releases the queue's reference.
    Record* rec = nullptr;
    while (xQueueReceive(sub.queue, &rec, 0) == pdPASS) {
        if (rec != nullptr) recordUnref(rec);
    }
    vQueueDelete(sub.queue);
    sub.queue = nullptr;
    sub.used = false;
    sub.name[0] = '\0';
    ESP_LOGI(TAG, "subscriber %u unregistered", id);
    return ESP_OK;
}

esp_err_t frameManagerStart() {
    if (!s_task_alive.load()) return ESP_ERR_INVALID_STATE;
    s_camera_gone.store(false);
    s_running.store(true);
    xTaskNotifyGive(s_task);   // wake a parked task immediately
    ESP_LOGI(TAG, "frame acquisition started (capacity %u B)",
             static_cast<unsigned>(frameJpegCapacity()));
    return ESP_OK;
}

esp_err_t frameManagerStop() {
    s_running.store(false);
    // Wait for the capture task to observe s_running==false and park. It may
    // be blocked inside esp_camera_fb_get() (up to FB_GET_TIMEOUT), so give
    // it a few seconds. Without this handshake a later camera re-init races
    // a capture task still inside esp_camera_fb_get / the driver.
    const int64_t stop_deadline = nowUs() + 5'000'000LL;  // 5 s
    while (!s_task_parked.load() && nowUs() < stop_deadline) {
        vTaskDelay(pdMS_TO_TICKS(10));
    }
    if (!s_task_parked.load()) {
        ESP_LOGW(TAG, "frame acquisition stop: capture task did not park "
                      "within 5 s — a re-init may race it");
    }
    ESP_LOGI(TAG, "frame acquisition stopped (leases stay valid)");
    return ESP_OK;
}

bool frameAcquire(FrameSubscriberId sub, FrameLease& lease,
                  uint32_t timeout_ms) {
    lease = FrameLease{};
    if (sub >= VC_FRAME_SUBSCRIBERS || !s_subs[sub].used) return false;
    Record* rec = nullptr;
    if (xQueueReceive(s_subs[sub].queue, &rec,
                      pdMS_TO_TICKS(timeout_ms)) != pdPASS) {
        return false;
    }
    if (rec == nullptr || rec->fb == nullptr) return false;

    // The queue's reference transfers to this lease — no refcount change.
    lease.valid = true;
    lease.framesize = rec->framesize;
    lease.id = rec->id;
    lease.ts_us = rec->ts_us;
    lease.width = rec->fb->width;
    lease.height = rec->fb->height;
    lease.len = rec->fb->len;
    lease.data = rec->fb->buf;
    lease.record = rec;
    s_leases_taken.fetch_add(1);
    return true;
}

void frameRelease(FrameLease& lease) {
    if (!lease.valid || lease.record == nullptr) {
        lease = FrameLease{};
        return;
    }
    auto* rec = static_cast<Record*>(lease.record);
    lease = FrameLease{};        // zero first: a second release is a no-op
    s_leases_released.fetch_add(1);
    recordUnref(rec);
}

esp_err_t frameLeaseCopyTo(const FrameLease& lease, uint8_t* dst,
                           size_t cap) {
    if (!lease.valid || dst == nullptr) return ESP_ERR_INVALID_ARG;
    if (lease.len > cap) return ESP_ERR_INVALID_SIZE;   // never truncate
    std::memcpy(dst, lease.data, lease.len);
    return ESP_OK;
}

FrameManagerStats frameManagerGetStats() {
    FrameManagerStats st;
    st.running = s_running.load();
    st.camera_gone = s_camera_gone.load();
    st.frames_captured = s_frames_captured.load();
    st.invalid_dropped = s_invalid_dropped.load();
    st.pool_dropped = s_pool_dropped.load();
    st.leases_taken = s_leases_taken.load();
    st.leases_released = s_leases_released.load();
    st.subscribers = 0;
    for (unsigned i = 0; i < VC_FRAME_SUBSCRIBERS; ++i) {
        if (!s_subs[i].used) continue;
        st.subscribers++;
        st.queue_dropped += s_subs[i].dropped;
    }
    st.last_frame_len = s_last_len.load();
    st.last_frame_ts_us = s_last_ts.load();
    return st;
}

}  // namespace visioncam