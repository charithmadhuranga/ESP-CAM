// wall_clock.h — one definition of "now" for timestamps written to disk.
//
// Why this exists: SNTP (started by the network manager on GOT_IP) sets the
// system clock. Before that, time(nullptr) is seconds-since-boot, which
// stamps media records as 1970-01-01 once gmtime_r interprets them as an
// epoch — retention age maths then deletes (or keeps) the wrong files.
//
// Every module that writes a timestamp to SD or to an index MUST use this
// instead of esp_timer_get_time() (uptime) or a bare time(nullptr).
// Once NTP has synced the value is real and monotonic enough for retention.
#pragma once

#include <cstdint>
#include <ctime>

#include "esp_timer.h"

namespace visioncam {

// 2020-09-13: anything below this cannot be a plausible wall clock, so the
// system clock has not been set yet. Never regresses to 1970 after sync.
inline constexpr time_t kWallClockEraThreshold = 1600000000;

// Wall-clock seconds. Falls back to uptime (still ordered within a session)
// when the clock has not been set, so records sort correctly either way.
inline uint32_t wallClockSeconds() {
    const time_t now = std::time(nullptr);
    if (now >= kWallClockEraThreshold) return static_cast<uint32_t>(now);
    return static_cast<uint32_t>(esp_timer_get_time() / 1000000ULL);
}

}  // namespace visioncam
