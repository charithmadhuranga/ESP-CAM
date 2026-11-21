// frame_manager.h — the frame manager (plan 02 task 1.1.5; SPEC 4.1.1
// rule 9, SPEC 8.2, AGENTS rule 9).
//
// THE ONLY module that touches camera frame buffers: one capture task
// owns every esp_camera_fb_get()/esp_camera_fb_return() call in the
// firmware (AGENTS rule 9). Everything else talks to leases.
//
// Design, and why it is shaped this way:
//
//   capture task  --lease--> consumer          (fast path, no copy)
//        |
//        +-- copy out into the consumer's own PSRAM buffer, then release
//
// A LEASE is a reference-counted loan of a camera frame buffer: only the
// memory address is shared, refcount in a bounded pool record. The
// contract is short: a lease holder must NOT hold a lease across blocking
// I/O (SD write, socket send, MQTT publish — SPEC 4.1.1 rule 9). A slow
// consumer copies the frame into its OWN bounded buffer with
// frameLeaseCopyTo() and releases the lease immediately — that is the
// "per-consumer PSRAM copy for slow consumers" the SPEC asks for, and it
// is what makes a slow stream client unable to stall capture.
//
// Bounded everywhere (AGENTS rule 12), each stated where it is created:
//   - VC_FRAME_SUBSCRIBERS registered consumers (fixed registry)
//   - VC_FRAME_QUEUE_DEPTH descriptors per consumer (FreeRTOS queue,
//     drop-oldest when the consumer is behind, counted, never blocking
//     the capture task)
//   - VC_FRAME_POOL refcounted records total: when every record is still
//     referenced by a lease or a queue the NEW frame is dropped and
//     counted — the pool is the back-pressure, and it is why no consumer
//     can wedge capture
//
// Drop policy for a slow consumer (SPEC 4.1.1 rule 9 / R1's pattern):
// the frame that consumer has not taken yet is REPLACED by the newer
// one and counted — fresh frames matter more than a backlog for live
// view, motion and snapshots alike. Frame data is never logged (AGENTS
// coding standards); only lengths, ids and counters are.
//
// Subscriber registration is serialized against capture distribution.
// The capture task uses a try-lock and accounts a dropped distribution
// instead of blocking on a consumer lifecycle change.

#pragma once

#include <cstddef>
#include <cstdint>

#include "esp_err.h"
#include "sensor.h"

namespace visioncam {

// Fixed bounds (AGENTS rule 12 — see the header comment).
// Slot budget (2026-11-21, after the live stream landed and AI/timelapse
// were descoped):
//   persistent — smoke (boot test, never released), recorder, motion = 3
//   transient  — live stream (one per open browser, released on every
//                exit path via RAII) and snapshot capture (subscribes
//                per capture, unsubscribes on every exit path)  = 5 spare
// Eight slots costs ~800 B of depth-2 descriptor queues, not frame buffers.
// The budget stays explicit so a registry-full failure can't mask a leak.
#define VC_FRAME_SUBSCRIBERS   8   // registered consumers
#define VC_FRAME_QUEUE_DEPTH   2   // descriptors per consumer
#define VC_FRAME_POOL          4   // refcounted frame records

// Consumer handle from frameManagerSubscribe(). opaque; 0 is never valid.
using FrameSubscriberId = uint8_t;

// A lease on one camera frame. Copy it by value; the record pointer
// inside is what frameRelease() returns to the pool/buffers. A lease is
// single-use: frameRelease() zeroes it so a double release is a no-op.
struct FrameLease {
    bool     valid = false;
    framesize_t framesize = FRAMESIZE_VGA;
    uint32_t id = 0;        // monotonic frame id (wraps, documented)
    uint64_t ts_us = 0;     // esp_timer microseconds at acquisition
    uint16_t width = 0;
    uint16_t height = 0;
    uint32_t len = 0;       // JPEG length in bytes (0 => malformed)
    const uint8_t* data = nullptr;

  private:
    void* record = nullptr;  // internal pool record; never dereferenced here
    friend void frameRelease(FrameLease& lease);
    friend bool frameAcquire(FrameSubscriberId sub, FrameLease& lease,
                             uint32_t timeout_ms);
};

// Creates the capture task (parked) and the subscriber registry. The
// camera must already be initialised by camera/ before frameManagerStart().
// Idempotent: a second call returns ESP_ERR_INVALID_STATE.
esp_err_t frameManagerInit();

// Registers a bounded per-consumer queue. name is for logs/stats only
// (copied, max 15 chars). Returns ESP_ERR_NO_MEM when the registry is full.
esp_err_t frameManagerSubscribe(FrameSubscriberId* out_id, const char* name);

// Unregisters: queued (unleased) descriptors are released. Outstanding
// leases remain valid until their frameRelease().
esp_err_t frameManagerUnsubscribe(FrameSubscriberId id);

// Starts frame acquisition. Requires an initialised camera (camera/),
// otherwise ESP_ERR_INVALID_STATE. Safe to call again while running.
esp_err_t frameManagerStart();

// Stops acquisition without disturbing outstanding leases (they stay
// valid until released). The capture task parks, so a later
// frameManagerStart() resumes with no re-init.
esp_err_t frameManagerStop();

// Takes the next frame lease for this consumer, waiting up to
// timeout_ms (0 = poll). Returns false on timeout. The lease MUST be
// released with frameRelease(); holding it across blocking I/O is a
// SPEC 4.1.1 rule 9 violation.
bool frameAcquire(FrameSubscriberId sub, FrameLease& lease,
                  uint32_t timeout_ms);

// Returns the lease: refcount-- and, at zero, the camera buffer goes
// back to the driver. A zero/invalid lease is a no-op (double-release
// safe). After the call the lease is zeroed.
void frameRelease(FrameLease& lease);

// Copies the leased JPEG into the caller's own buffer, then the caller
// is expected to release the lease immediately — that is the
// per-consumer PSRAM copy pattern for slow consumers (SPEC 4.1.1
// rule 9). Bounded by cap; a frame that does not fit is an error
// (never truncated silently).
esp_err_t frameLeaseCopyTo(const FrameLease& lease, uint8_t* dst,
                           size_t cap);

struct FrameManagerStats {
    bool     running = false;          // capture task acquiring
    bool     camera_gone = false;      // ladder raised CameraFailed; task stopped
    uint32_t frames_captured = 0;      // valid frames offered to consumers
    uint32_t invalid_dropped = 0;      // SPEC 4.1.1 rule 7 (len == 0 / > capacity)
    uint32_t pool_dropped = 0;         // no free record: new frame dropped
    uint32_t queue_dropped = 0;        // drop-oldest, all consumers summed
                                       // (incl. frames dropped early by
                                       //  evictForCapture before acquire)
    uint32_t backpressure_waits = 0;   // fb_get skipped: all driver buffers
                                       // held by in-flight leases (never a
                                       //  camera-failure ladder attempt)
    uint32_t leases_taken = 0;
    uint32_t leases_released = 0;
    uint32_t subscribers = 0;
    uint32_t last_frame_len = 0;
    uint64_t last_frame_ts_us = 0;
};

FrameManagerStats frameManagerGetStats();

// Rule-7 capacity the capture task validates every frame against
// (delegates to the camera manager: jpeg_buffer_size if set, else the
// driver's width*height/5 of the current frame size).
size_t frameJpegCapacity();

}  // namespace visioncam