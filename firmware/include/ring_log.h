// ring_log.h — RTC RAM ring log + crash-loop detection (SPEC section 8.4.1).
//
// A bounded log kept in RTC slow memory with the no-init attribute, so it
// survives a software reset and is still readable after a crash — the
// evidence you need is the evidence a crash destroys first. On power-on the
// RTC contents are undefined, so the log self-identifies with a magic value
// and is re-initialised when the magic does not match.
//
// Size: VC_RING_LOG_BYTES of line data (R1 keeps ~6.6 KB in the same
// place; ours is smaller and wraps by dropping the oldest whole line).
//
// Call context: ringLogInit()/ringLogDump()/ringLogMarkHealthy() from the
// boot path only; ringLogWrite() from any TASK (it takes a spinlock and
// formats on the stack — never from an ISR, never on a hot per-frame path:
// frame data is not logged, AGENTS coding standards).

#pragma once

#include <cstdint>

#include "esp_err.h"
#include "esp_system.h"

namespace visioncam {

// Ring data capacity in bytes (the header lives outside it).
#define VC_RING_LOG_BYTES       4096
// Longest single formatted line, including the prefix and newline.
#define VC_RING_LOG_LINE_MAX    192

enum class RingLevel : uint8_t { Debug = 0, Info, Warn, Error };

// MUST be the first thing app_main does. Reads the previous session out of
// RTC memory, reports crash-loop state (SPEC 8.4.1: consecutive abnormal
// resets, with a brownout/power hint from the reset reason), then starts
// this session's log with a boot marker.
void ringLogInit();

// Appends one formatted line. Bounded by VC_RING_LOG_LINE_MAX; overflow of
// the ring drops the oldest whole line (drop-oldest, counted in the boot
// marker as it wraps). Never blocks on I/O.
void ringLogWrite(RingLevel level, const char* tag, const char* fmt, ...)
    __attribute__((format(printf, 3, 4)));

// Prints the retained log (oldest surviving line first) to the console.
// Called once from the boot path before other tasks exist, so lines cannot
// interleave with ESP_LOG output.
void ringLogDump();

// Declares this boot healthy: clears the abnormal-boot counter. Called when
// the device reaches its health point — camera running and HTTP serving
// (Wi-Fi is optional; local operation is the product promise).
void ringLogMarkHealthy();

// Consecutive boots that ended before ringLogMarkHealthy() (0 = healthy).
// Exposed by the resource monitor for /status and /metrics.
uint32_t ringLogAbnormalBoots();

// Total boots seen in RTC memory (survives soft resets; resets on power-on
// when the magic is rebuilt). Diagnostics only.
uint32_t ringLogBootCount();

// Convenience macros: level, tag, printf-style message.
#define VC_RING_LOG(level, tag, ...) \
    ::visioncam::ringLogWrite((level), (tag), __VA_ARGS__)
#define VC_RING_INFO(tag, ...)  VC_RING_LOG(::visioncam::RingLevel::Info,  tag, __VA_ARGS__)
#define VC_RING_WARN(tag, ...)  VC_RING_LOG(::visioncam::RingLevel::Warn,  tag, __VA_ARGS__)
#define VC_RING_ERROR(tag, ...) VC_RING_LOG(::visioncam::RingLevel::Error, tag, __VA_ARGS__)

}  // namespace visioncam
