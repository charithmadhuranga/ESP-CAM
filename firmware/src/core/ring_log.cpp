// ring_log.cpp — see ring_log.h.
//
// Layout in RTC slow memory (all RTC_NOINIT_ATTR — no initialiser, so the
// linker places them in .rtc_noinit and the ROM/bootloader leaves them
// alone across a software reset; on power-on they read as garbage, which
// the magic check turns into a clean re-init):
//
//   RingHeader  magic, version, boot/abnormal counters, seq, cursor/count
//   uint8_t[]   VC_RING_LOG_BYTES of newline-terminated lines
//
// Overflow policy (AGENTS rule 12, stated here where the storage is
// created): the byte ring is fixed at VC_RING_LOG_BYTES; when a line does
// not fit, the oldest whole line is dropped until it does. Nothing is ever
// reallocated and no line is refused for lack of space.

#include "ring_log.h"

#include <cstdarg>
#include <cstdio>
#include <cstring>

#include "esp_attr.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#include "resource_monitor.h"   // resetReasonName()

namespace visioncam {
namespace {

constexpr uint32_t kMagic = 0x4C524356;   // "VCRL" little-endian
constexpr uint16_t kVersion = 1;
constexpr uint16_t kFlagHealthy = 1u << 0;

struct RingHeader {
    uint32_t magic;
    uint16_t version;
    uint16_t flags;            // bit0: this session reached health
    uint32_t boot_count;       // valid boots seen since power-on
    uint32_t abnormal_boots;   // consecutive boots that ended pre-health
    uint32_t seq;              // line sequence number
    uint32_t start;            // offset of the oldest byte
    uint32_t count;            // bytes currently held (<= VC_RING_LOG_BYTES)
};

RTC_NOINIT_ATTR RingHeader s_hdr;
RTC_NOINIT_ATTR uint8_t s_ring[VC_RING_LOG_BYTES];

// Serialises ring/cursor updates across tasks. Formatting happens on the
// caller's stack OUTSIDE this lock; the lock only covers the byte copy.
// Task context only — never from an ISR (AGENTS: keep ISR code minimal).
portMUX_TYPE s_mux = portMUX_INITIALIZER_UNLOCKED;

// Guards writes that happen before ringLogInit() (a constructor or an early
// failure) against using garbage cursors from uninitialised RTC memory.
bool s_ready = false;

// Drops the oldest whole line until `need` bytes fit. Caller holds s_mux.
// Returns false only if `need` can never fit (larger than the ring).
bool makeRoomLocked(size_t need) {
    if (need > VC_RING_LOG_BYTES) return false;
    while (s_hdr.count + need > VC_RING_LOG_BYTES) {
        // Advance start past the first newline; if the ring holds no
        // complete line, start over from the beginning.
        uint32_t i = s_hdr.start;
        bool found = false;
        for (uint32_t n = 0; n < s_hdr.count; ++n) {
            if (s_ring[i] == '\n') {
                const uint32_t dropped = (i + 1 >= s_hdr.start)
                                             ? (i + 1 - s_hdr.start)
                                             : (VC_RING_LOG_BYTES - s_hdr.start + i + 1);
                s_hdr.start = (i + 1) % VC_RING_LOG_BYTES;
                s_hdr.count -= dropped;
                found = true;
                break;
            }
            i = (i + 1) % VC_RING_LOG_BYTES;
        }
        if (!found) {
            s_hdr.start = 0;
            s_hdr.count = 0;
        }
    }
    return true;
}

void writeLine(RingLevel level, const char* tag, const char* fmt, ...)
    __attribute__((format(printf, 3, 4)));

void writeLine(RingLevel level, const char* tag, const char* fmt, ...) {
    if (!s_ready) return;

    // Compose the whole line on the stack first: the lock must never cover
    // vsnprintf (variadic, unbounded time) — only the byte copy.
    char line[VC_RING_LOG_LINE_MAX];
    char msg[VC_RING_LOG_LINE_MAX - 48];  // room for "#seq ms L tag: " + \n
    va_list args;
    va_start(args, fmt);
    const int m = vsnprintf(msg, sizeof(msg), fmt, args);
    va_end(args);
    if (m < 0) return;

    const char lvl = "DIWE"[static_cast<unsigned>(level) > 3
                                ? 0
                                : static_cast<unsigned>(level)];
    const uint32_t ms = static_cast<uint32_t>(esp_timer_get_time() / 1000);

    portENTER_CRITICAL(&s_mux);
    // Prefix without the newline, then force one on: every line in the ring
    // is newline-terminated, including a truncated long message.
    int n = snprintf(line, sizeof(line) - 1, "#%u %u %c %s: %s",
                     static_cast<unsigned>(++s_hdr.seq),
                     static_cast<unsigned>(ms), lvl, tag, msg);
    if (n < 0) {
        portEXIT_CRITICAL(&s_mux);
        return;
    }
    if (n > static_cast<int>(sizeof(line) - 2)) n = sizeof(line) - 2;
    line[n++] = '\n';

    if (makeRoomLocked(static_cast<size_t>(n))) {
        for (int i = 0; i < n; ++i) {
            s_ring[(s_hdr.start + s_hdr.count) % VC_RING_LOG_BYTES] =
                static_cast<uint8_t>(line[i]);
            s_hdr.count++;
        }
    }
    portEXIT_CRITICAL(&s_mux);
}

}  // namespace

void ringLogInit() {
    const esp_reset_reason_t reason = esp_reset_reason();

    if (s_hdr.magic == kMagic && s_hdr.version == kVersion) {
        // Previous session survived in RTC memory: this is a software
        // reset. A boot that never reached health counts as abnormal.
        //
        // The ring CONTENT is deliberately preserved: after a crash the
        // next boot's dump is the only remaining record of what the dying
        // session was doing (SPEC 8.4.1). start/count/seq stay as they
        // were — new lines append after the old ones and the drop-oldest
        // policy makes room (the boot marker below says which session a
        // line belongs to).
        if ((s_hdr.flags & kFlagHealthy) != 0) {
            s_hdr.abnormal_boots = 0;
        } else if (s_hdr.abnormal_boots < 0xFFFFu) {
            s_hdr.abnormal_boots++;
        }
        s_hdr.boot_count++;
    } else {
        // Power-on (or RTC contents unreadable): start fresh. A power cut
        // is not a crash loop, so the abnormal counter begins at zero.
        memset(&s_hdr, 0, sizeof(s_hdr));
        s_hdr.magic = kMagic;
        s_hdr.version = kVersion;
        s_hdr.boot_count = 1;
    }
    s_hdr.flags = 0;  // this session is not healthy yet
    s_ready = true;

    // Boot marker: reset reason + crash-loop state for this boot. Written
    // while only this task exists, so it lands before any other line.
    writeLine(RingLevel::Info, "boot", "reset=%s boot=%u abnormal=%u",
              resetReasonName(reason),
              static_cast<unsigned>(s_hdr.boot_count),
              static_cast<unsigned>(s_hdr.abnormal_boots));

    if (s_hdr.abnormal_boots >= 3) {
        // R1's lesson (SPEC 8.4.1): repeated abnormal resets are almost
        // always the power supply — say so, prominently.
        writeLine(RingLevel::Error, "boot",
                  "CRASH LOOP: %u consecutive boots ended before health — "
                  "check the 5 V power supply first%s",
                  static_cast<unsigned>(s_hdr.abnormal_boots),
                  reason == ESP_RST_BROWNOUT ? " (brownout reset seen)" : "");
    } else if (reason == ESP_RST_BROWNOUT) {
        writeLine(RingLevel::Warn, "boot",
                  "brownout reset on previous boot — check the 5 V supply "
                  "under Wi-Fi TX load");
    }
}

void ringLogWrite(RingLevel level, const char* tag, const char* fmt, ...) {
    if (!s_ready) return;
    char msg[VC_RING_LOG_LINE_MAX - 48];
    va_list args;
    va_start(args, fmt);
    const int n = vsnprintf(msg, sizeof(msg), fmt, args);
    va_end(args);
    if (n < 0) return;
    writeLine(level, tag, "%s", msg);
}

void ringLogDump() {
    if (!s_ready) return;

    printf("---- RTC ring log (boot %u, %u byte(s) retained) ----\n",
           static_cast<unsigned>(s_hdr.boot_count),
           static_cast<unsigned>(s_hdr.count));

    // One line per lock: the lock must never span printf (AGENTS: avoid
    // long critical sections). Dump runs from the boot path, but the
    // per-line discipline keeps it correct if that ever changes.
    uint32_t i = s_hdr.start;
    const uint32_t end = (s_hdr.start + s_hdr.count) % VC_RING_LOG_BYTES;
    for (;;) {
        char line[VC_RING_LOG_LINE_MAX];
        size_t n = 0;
        bool complete = false;
        portENTER_CRITICAL(&s_mux);
        if (s_hdr.start > i ||  // a writer dropped past our position
            ((i - s_hdr.start + VC_RING_LOG_BYTES) % VC_RING_LOG_BYTES) >
                s_hdr.count) {
            portEXIT_CRITICAL(&s_mux);
            break;  // ring was reset or wrapped under us — stop cleanly
        }
        while (i != end && n < sizeof(line) - 1) {
            const char c = static_cast<char>(s_ring[i]);
            i = (i + 1) % VC_RING_LOG_BYTES;
            if (c == '\n') {
                complete = true;
                break;
            }
            line[n++] = c;
        }
        portEXIT_CRITICAL(&s_mux);
        if (n == 0 && !complete) break;   // nothing left
        if (complete) {
            line[n] = '\0';
            printf("%s\n", line);
        }
        if (i == end) break;
    }
    printf("---- end of retained log ----\n");
}

void ringLogMarkHealthy() {
    if (!s_ready) return;
    portENTER_CRITICAL(&s_mux);
    s_hdr.flags |= kFlagHealthy;
    s_hdr.abnormal_boots = 0;
    portEXIT_CRITICAL(&s_mux);
    writeLine(RingLevel::Info, "boot",
              "health reached — abnormal boots cleared");
}

uint32_t ringLogAbnormalBoots() { return s_ready ? s_hdr.abnormal_boots : 0; }

uint32_t ringLogBootCount() { return s_ready ? s_hdr.boot_count : 0; }

}  // namespace visioncam
