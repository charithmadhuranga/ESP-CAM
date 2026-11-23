// rtsp_server.h - RTSP (RFC 2326) server streaming MJPEG over RTP (RFC 2435).
//
// Owner decision D7 (2026-11-23): RTSP is brought into scope so Home
// Assistant (Generic Camera / go2rtc) and VLC can pull the live camera.
// SPEC 1A had deferred R1's rtsp.cpp; nothing is taken from R1 (AGPL) -
// this is written from RFC 2326, RFC 2435 and RFC 2617 only.
//
// What it is, and what it is not:
//   - video only, codec MJPEG (RTP payload type 26). The ESP32 has no H.264
//     encoder (AGENTS rule 4); clients that need H.264 must transcode
//     (Home Assistant's go2rtc can, see docs/HARDWARE.md 7.2);
//   - RTP is interleaved on the RTSP TCP connection ("RTP/AVP/TCP"). A UDP
//     SETUP gets 461 Unsupported Transport, which makes ffmpeg/VLC retry on
//     TCP automatically; go2rtc uses TCP by default;
//   - authentication is HTTP Digest (RFC 2617, MD5) with a dedicated RTSP
//     username/password. Only HA1 = MD5(user:realm:password) is stored, the
//     password never is and never crosses the network (AGENTS rules 16-17;
//     Basic auth is not offered, rule 35);
//   - disabled until credentials are configured (secure default).
//
// Bounds (AGENTS rules 12-13): kRtspMaxClients sessions, each a normal
// frame-manager subscriber (lease -> copy -> release; drop-oldest queue,
// so a slow RTSP client never stalls capture - rules 9, 11), a request
// buffer of kRtspRequestMax bytes and a frame copy buffer capped at
// kRtspFrameMax. Session buffers live in PSRAM.

#pragma once

#include <cstddef>
#include <cstdint>

#include "esp_err.h"

namespace visioncam {

// Connections vs. streams: Home Assistant opens several short RTSP
// connections at once (go2rtc's transcoder, its own stream worker, config-
// flow previews, still-image probes). With a limit of 2 the extra ones got
// "503 Service Unavailable" (seen in HA, 2026-11-23). Connections that only
// probe (OPTIONS/DESCRIBE) cost a small task; only PLAYing sessions take a
// frame-manager slot and bandwidth, so the two are bounded separately.
// 3 playing = the frame-manager slots left after smoke, recorder, motion,
// live view and a transient snapshot (VC_FRAME_SUBSCRIBERS = 8).
constexpr uint32_t kRtspMaxClients = 4;
constexpr uint32_t kRtspMaxPlaying = 3;
constexpr size_t kRtspRequestMax = 2048;
constexpr size_t kRtspFrameMax = 400 * 1024;
constexpr uint16_t kRtspDefaultPort = 554;
constexpr uint32_t kRtspConfigSchema = 1;
constexpr char kRtspRealm[] = "VisionCam";

struct RtspConfig {
    bool enabled = false;
    uint16_t port = kRtspDefaultPort;
    char username[33] = {};
    bool password_set = false;  // HA1 stored
};

struct RtspStatus {
    bool running = false;          // listener up
    uint32_t clients = 0;          // sessions currently connected
    uint32_t sessions_total = 0;   // since boot
    uint32_t auth_failures = 0;
    uint32_t frames_sent = 0;
    uint32_t frames_dropped = 0;   // send failures / oversize
    uint32_t playing = 0;          // sessions currently streaming
    uint32_t busy_rejects = 0;     // connections refused (limit reached)
    uint32_t stack_min_free = 0;   // worst session stack headroom (bytes)
    char last_error[48] = {};
};

// Loads the config from NVS and starts the listener if enabled.
// Failure is not fatal: every other subsystem works without RTSP.
esp_err_t rtspServerInit();

esp_err_t rtspServerGetConfig(RtspConfig* out);

// Validates and persists. `password` may be nullptr/"" to keep the stored
// HA1. username 1..32 of [A-Za-z0-9._-]; password 8..63 printable ASCII
// without ':' ; port 1..65535; enabling requires credentials. Restarts the
// listener (existing sessions are closed) when anything changed.
esp_err_t rtspServerSetConfig(bool enabled, uint16_t port,
                              const char* username, const char* password);

esp_err_t rtspServerStatus(RtspStatus* out);

}  // namespace visioncam
