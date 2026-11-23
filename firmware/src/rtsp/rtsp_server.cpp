// rtsp_server.cpp - RTSP/RTP MJPEG server (see rtsp_server.h).
//
// Clean-room from RFC 2326 (RTSP 1.0), RFC 2435 (RTP payload for JPEG),
// RFC 2617 (Digest) - owner decision D7. Nothing is taken from R1.

#include "rtsp_server.h"

#include <algorithm>
#include <atomic>
#include <cctype>
#include <cstdio>
#include <cstring>
#include <new>

#include "esp_heap_caps.h"
#include "esp_log.h"
#include "esp_random.h"
#include "esp_rom_md5.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "freertos/task.h"
#include "lwip/sockets.h"
#include "nvs.h"

#include "frame_manager.h"
#include "tasks.h"

namespace visioncam {
namespace {

const char* TAG = "vc_rtsp";

// ---- NVS ------------------------------------------------------------------
constexpr char kNvsNs[] = "rtsp";
constexpr char kKeyVer[] = "ver";
constexpr char kKeyEnabled[] = "en";
constexpr char kKeyPort[] = "port";
constexpr char kKeyUser[] = "user";
constexpr char kKeyHa1[] = "ha1";

// ---- RTP / JPEG -----------------------------------------------------------
constexpr uint8_t kRtpPayloadJpeg = 26;
constexpr size_t kRtpChunk = 1400;  // JPEG scan bytes per RTP packet
// '$' framing (4) + RTP (12) + JPEG header (8) + restart header (4) +
// quantization header (4) + two 8-bit tables (128) + chunk.
constexpr size_t kPacketMax = 4 + 12 + 8 + 4 + 4 + 128 + kRtpChunk;
// A send that cannot make progress for this long closes the session. Short
// stalls are normal on a busy Wi-Fi link (hardware test: RTSP + web live
// view over a phone hotspot); they only lower the frame rate, since each
// subscriber always gets the newest frame (drop-oldest queue).
constexpr uint32_t kIoTimeoutMs = 10000;
// A connection that never starts playing is a probe (HA config-flow
// preview, still-image fetch, health check); free its slot quickly.
constexpr uint32_t kSetupIdleMs = 15000;

// ---- state ------------------------------------------------------------------
SemaphoreHandle_t g_lock = nullptr;
RtspConfig g_cfg{};
char g_ha1[33] = {};
TaskHandle_t g_listener = nullptr;
std::atomic<uint32_t> g_generation{0};  // bumped on config change
std::atomic<uint32_t> g_clients{0};
std::atomic<bool> g_running{false};
std::atomic<uint32_t> g_sessions_total{0};
std::atomic<uint32_t> g_auth_failures{0};
std::atomic<uint32_t> g_frames_sent{0};
std::atomic<uint32_t> g_frames_dropped{0};
std::atomic<uint32_t> g_playing{0};
std::atomic<uint32_t> g_busy_rejects{0};
std::atomic<uint32_t> g_stack_min_free{UINT32_MAX};
char g_last_error[48] = {};

struct Lock {
    Lock() { xSemaphoreTake(g_lock, portMAX_DELAY); }
    ~Lock() { xSemaphoreGive(g_lock); }
};

void setError(const char* msg) {
    Lock l;
    std::snprintf(g_last_error, sizeof(g_last_error), "%s", msg);
}

// ---- helpers ----------------------------------------------------------------

void md5Hex(const char* const* parts, size_t n, char out[33]) {
    md5_context_t ctx;
    esp_rom_md5_init(&ctx);
    for (size_t i = 0; i < n; ++i) {
        if (i > 0) esp_rom_md5_update(&ctx, reinterpret_cast<const uint8_t*>(":"), 1);
        esp_rom_md5_update(&ctx, reinterpret_cast<const uint8_t*>(parts[i]),
                           std::strlen(parts[i]));
    }
    uint8_t d[16];
    esp_rom_md5_final(d, &ctx);
    for (int i = 0; i < 16; ++i) std::snprintf(out + i * 2, 3, "%02x", d[i]);
}

bool constTimeEq(const char* a, const char* b, size_t n) {
    uint8_t diff = 0;
    for (size_t i = 0; i < n; ++i) diff |= static_cast<uint8_t>(a[i] ^ b[i]);
    return diff == 0;
}

bool validUsername(const char* u) {
    const size_t n = std::strlen(u);
    if (n < 1 || n > 32) return false;
    for (size_t i = 0; i < n; ++i) {
        const char c = u[i];
        if (!(std::isalnum(static_cast<unsigned char>(c)) || c == '.' ||
              c == '_' || c == '-'))
            return false;
    }
    return true;
}

bool validPassword(const char* p) {
    const size_t n = std::strlen(p);
    if (n < 8 || n > 63) return false;
    for (size_t i = 0; i < n; ++i) {
        if (p[i] < 0x20 || p[i] > 0x7e) return false;
    }
    return true;
}

void putBe16(uint8_t* p, uint16_t v) {
    p[0] = static_cast<uint8_t>(v >> 8);
    p[1] = static_cast<uint8_t>(v);
}
void putBe32(uint8_t* p, uint32_t v) {
    p[0] = static_cast<uint8_t>(v >> 24);
    p[1] = static_cast<uint8_t>(v >> 16);
    p[2] = static_cast<uint8_t>(v >> 8);
    p[3] = static_cast<uint8_t>(v);
}

// Case-insensitive header lookup in a request block. Copies the trimmed
// value (bounded). Returns false if absent.
bool header(const char* req, const char* name, char* out, size_t cap) {
    const size_t nlen = std::strlen(name);
    const char* line = std::strstr(req, "\r\n");
    while (line != nullptr && line[2] != '\0') {
        line += 2;
        if (strncasecmp(line, name, nlen) == 0 && line[nlen] == ':') {
            const char* v = line + nlen + 1;
            while (*v == ' ' || *v == '\t') ++v;
            const char* end = std::strstr(v, "\r\n");
            size_t len = end ? static_cast<size_t>(end - v) : std::strlen(v);
            if (len >= cap) len = cap - 1;
            std::memcpy(out, v, len);
            out[len] = '\0';
            return true;
        }
        line = std::strstr(line, "\r\n");
    }
    return false;
}

// Digest parameter (quoted or bare) from an Authorization value.
bool digestParam(const char* auth, const char* key, char* out, size_t cap) {
    const size_t klen = std::strlen(key);
    const char* p = auth;
    while ((p = std::strstr(p, key)) != nullptr) {
        const bool at_start = (p == auth) || p[-1] == ' ' || p[-1] == ',';
        if (at_start && p[klen] == '=') {
            const char* v = p + klen + 1;
            size_t len = 0;
            if (*v == '"') {
                ++v;
                const char* e = std::strchr(v, '"');
                if (e == nullptr) return false;
                len = static_cast<size_t>(e - v);
            } else {
                while (v[len] && v[len] != ',' && v[len] != ' ') ++len;
            }
            if (len >= cap) return false;
            std::memcpy(out, v, len);
            out[len] = '\0';
            return true;
        }
        p += klen;
    }
    return false;
}

// ---- JPEG parsing (RFC 2435 section 3.1 inputs) ------------------------------

struct JpegInfo {
    uint8_t type = 0;          // 0 = 4:2:2, 1 = 4:2:0 (+64 with restart)
    uint16_t width = 0;
    uint16_t height = 0;
    uint16_t dri = 0;
    const uint8_t* qt[2] = {nullptr, nullptr};  // tables used by Y, then C
    const uint8_t* scan = nullptr;
    size_t scan_len = 0;
};

bool parseJpeg(const uint8_t* d, size_t n, JpegInfo& j) {
    if (n < 4 || d[0] != 0xFF || d[1] != 0xD8) return false;
    const uint8_t* tables[4] = {};
    uint8_t comp_tq[3] = {0, 1, 1};
    uint8_t ncomp = 0;
    bool sof = false;
    size_t i = 2;
    while (i + 4 <= n) {
        if (d[i] != 0xFF) return false;
        const uint8_t m = d[i + 1];
        if (m == 0xFF) { ++i; continue; }               // fill byte
        if (m == 0xD8 || m == 0x01 || (m >= 0xD0 && m <= 0xD7)) {
            i += 2;
            continue;
        }
        const size_t seg = (static_cast<size_t>(d[i + 2]) << 8) | d[i + 3];
        if (seg < 2 || i + 2 + seg > n) return false;
        const uint8_t* s = d + i + 4;
        const size_t slen = seg - 2;
        if (m == 0xDB) {                                // DQT
            size_t k = 0;
            while (k < slen) {
                const uint8_t pq = s[k] >> 4, tq = s[k] & 0x0F;
                if (pq != 0 || tq > 3 || k + 65 > slen) return false;
                tables[tq] = s + k + 1;
                k += 65;
            }
        } else if (m == 0xC0) {                         // SOF0 baseline
            if (slen < 6) return false;
            j.height = static_cast<uint16_t>((s[1] << 8) | s[2]);
            j.width = static_cast<uint16_t>((s[3] << 8) | s[4]);
            ncomp = s[5];
            if (ncomp != 3 || slen < 6u + 3u * ncomp) return false;
            const uint8_t y = s[7];
            if (y == 0x21) j.type = 0;
            else if (y == 0x22) j.type = 1;
            else return false;
            if (s[10] != 0x11 || s[13] != 0x11) return false;
            for (int c = 0; c < 3; ++c) comp_tq[c] = s[8 + c * 3] & 0x03;
            sof = true;
        } else if ((m >= 0xC1 && m <= 0xCF) && m != 0xC4 && m != 0xC8 &&
                   m != 0xCC) {
            return false;                               // not baseline
        } else if (m == 0xDD) {                         // DRI
            if (slen < 2) return false;
            j.dri = static_cast<uint16_t>((s[0] << 8) | s[1]);
        } else if (m == 0xDA) {                         // SOS
            if (!sof) return false;
            j.scan = d + i + 2 + seg;
            size_t end = n;
            while (end >= 2 && !(d[end - 2] == 0xFF && d[end - 1] == 0xD9)) --end;
            if (end < 2 || d + end - 2 < j.scan) return false;
            j.scan_len = static_cast<size_t>((d + end - 2) - j.scan);
            j.qt[0] = tables[comp_tq[0]];
            j.qt[1] = tables[comp_tq[1]];
            if (j.qt[0] == nullptr || j.qt[1] == nullptr) return false;
            if (j.width == 0 || j.height == 0 || j.width > 2040 ||
                j.height > 2040)
                return false;
            if (j.dri) j.type = static_cast<uint8_t>(j.type + 64);
            return j.scan_len > 0;
        }
        i += 2 + seg;
    }
    return false;
}

// ---- session ----------------------------------------------------------------

struct Session {
    int fd = -1;
    uint32_t generation = 0;
    char peer[16] = {};
    char req[kRtspRequestMax + 1] = {};
    size_t req_len = 0;
    char nonce[33] = {};
    bool authed = false;
    bool setup = false;
    bool playing = false;
    uint8_t channel = 0;
    uint32_t session_id = 0;
    uint16_t seq = 0;
    uint32_t ssrc = 0;
    FrameSubscriberId sub = 0;
    bool subscribed = false;
    uint8_t* frame = nullptr;
    size_t frame_cap = 0;
    uint8_t pkt[kPacketMax] = {};
    char resp[1024] = {};
    // Scratch kept in the (PSRAM) session instead of on the 3 KB task stack.
    char auth[512] = {};
    char uri[256] = {};
    char extra[320] = {};
    char sdp[320] = {};
};

bool waitReadable(int fd, uint32_t ms) {
    fd_set r;
    FD_ZERO(&r);
    FD_SET(fd, &r);
    timeval tv{static_cast<time_t>(ms / 1000),
               static_cast<suseconds_t>((ms % 1000) * 1000)};
    return select(fd + 1, &r, nullptr, nullptr, &tv) > 0;
}

bool sendAll(int fd, const void* data, size_t len) {
    const uint8_t* p = static_cast<const uint8_t*>(data);
    while (len > 0) {
        const int w = send(fd, p, len, 0);   // SO_SNDTIMEO bounds each call
        if (w <= 0) return false;
        p += w;
        len -= static_cast<size_t>(w);
    }
    return true;
}

bool reply(Session& s, int code, const char* reason, const char* cseq,
           const char* extra, const char* body) {
    const size_t blen = body ? std::strlen(body) : 0;
    int n = std::snprintf(s.resp, sizeof(s.resp),
                          "RTSP/1.0 %d %s\r\nCSeq: %s\r\nServer: VisionCam\r\n%s",
                          code, reason, cseq, extra ? extra : "");
    if (n < 0 || static_cast<size_t>(n) >= sizeof(s.resp)) return false;
    size_t len = static_cast<size_t>(n);
    n = blen ? std::snprintf(s.resp + len, sizeof(s.resp) - len,
                             "Content-Length: %u\r\n\r\n",
                             static_cast<unsigned>(blen))
             : std::snprintf(s.resp + len, sizeof(s.resp) - len, "\r\n");
    if (n < 0 || len + static_cast<size_t>(n) >= sizeof(s.resp)) return false;
    len += static_cast<size_t>(n);
    return sendAll(s.fd, s.resp, len) && (!blen || sendAll(s.fd, body, blen));
}

bool checkAuth(Session& s, const char* method, const char* cseq) {
    if (s.authed) return true;
    char* auth = s.auth;
    char* uri = s.uri;
    auth[0] = uri[0] = '\0';
    bool ok = false;
    if (header(s.req, "Authorization", auth, sizeof(s.auth)) &&
        strncasecmp(auth, "Digest ", 7) == 0) {
        char user[40] = {}, nonce[40] = {}, resp[40] = {};
        char qop[16] = {}, nc[16] = {}, cnonce[72] = {};
        const char* a = auth + 7;
        char ha1[33];
        char cfg_user[33];
        {
            Lock l;
            std::memcpy(ha1, g_ha1, sizeof(ha1));
            std::memcpy(cfg_user, g_cfg.username, sizeof(cfg_user));
        }
        if (digestParam(a, "username", user, sizeof(user)) &&
            digestParam(a, "nonce", nonce, sizeof(nonce)) &&
            digestParam(a, "uri", uri, sizeof(s.uri)) &&
            digestParam(a, "response", resp, sizeof(resp)) &&
            std::strlen(resp) == 32 && std::strcmp(user, cfg_user) == 0 &&
            std::strcmp(nonce, s.nonce) == 0 && ha1[0] != '\0') {
            char ha2[33], expect[33];
            const char* p2[] = {method, uri};
            md5Hex(p2, 2, ha2);
            if (digestParam(a, "qop", qop, sizeof(qop)) &&
                digestParam(a, "nc", nc, sizeof(nc)) &&
                digestParam(a, "cnonce", cnonce, sizeof(cnonce))) {
                const char* p3[] = {ha1, nonce, nc, cnonce, qop, ha2};
                md5Hex(p3, 6, expect);
            } else {
                const char* p3[] = {ha1, nonce, ha2};
                md5Hex(p3, 3, expect);
            }
            ok = constTimeEq(expect, resp, 32);
        }
        if (!ok) {
            g_auth_failures.fetch_add(1);
            ESP_LOGW(TAG, "auth failed from %s", s.peer);
        }
    }
    if (ok) {
        s.authed = true;
        return true;
    }
    char* extra = s.extra;
    std::snprintf(extra, sizeof(s.extra),
                  "WWW-Authenticate: Digest realm=\"%s\", nonce=\"%s\", "
                  "algorithm=\"MD5\"\r\n",
                  kRtspRealm, s.nonce);
    reply(s, 401, "Unauthorized", cseq, extra, nullptr);
    return false;
}

bool frameReserve(Session& s, size_t need) {
    if (need <= s.frame_cap) return true;
    if (need > kRtspFrameMax) return false;
    const size_t cap = std::min(kRtspFrameMax, (need + 16383u) & ~size_t{16383});
    auto* p = static_cast<uint8_t*>(
        heap_caps_malloc(cap, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT));
    if (p == nullptr) return false;
    heap_caps_free(s.frame);
    s.frame = p;
    s.frame_cap = cap;
    return true;
}

// Sends one JPEG as RFC 2435 fragments, interleaved on the TCP connection.
bool sendJpeg(Session& s, const uint8_t* jpeg, size_t len, uint64_t ts_us) {
    JpegInfo j;
    if (!parseJpeg(jpeg, len, j)) return true;  // skip undecodable frame
    const uint32_t ts = static_cast<uint32_t>(ts_us * 9u / 100u);  // 90 kHz
    size_t off = 0;
    while (off < j.scan_len) {
        uint8_t* p = s.pkt;
        uint8_t* rtp = p + 4;
        uint8_t* q = rtp + 12;
        // JPEG main header
        q[0] = 0;
        q[1] = static_cast<uint8_t>(off >> 16);
        q[2] = static_cast<uint8_t>(off >> 8);
        q[3] = static_cast<uint8_t>(off);
        q[4] = j.type;
        q[5] = 255;  // Q >= 128: tables are carried in-band
        q[6] = static_cast<uint8_t>(j.width / 8);
        q[7] = static_cast<uint8_t>(j.height / 8);
        q += 8;
        if (j.dri) {
            putBe16(q, j.dri);
            q[2] = 0xFF;  // F=1 L=1, restart count 0x3FFF
            q[3] = 0xFF;
            q += 4;
        }
        if (off == 0) {
            q[0] = 0;  // MBZ
            q[1] = 0;  // precision: both tables 8-bit
            putBe16(q + 2, 128);
            std::memcpy(q + 4, j.qt[0], 64);
            std::memcpy(q + 68, j.qt[1], 64);
            q += 4 + 128;
        }
        const size_t chunk = std::min(kRtpChunk, j.scan_len - off);
        std::memcpy(q, j.scan + off, chunk);
        q += chunk;
        off += chunk;
        const bool last = off >= j.scan_len;

        rtp[0] = 0x80;
        rtp[1] = static_cast<uint8_t>((last ? 0x80 : 0) | kRtpPayloadJpeg);
        putBe16(rtp + 2, s.seq++);
        putBe32(rtp + 4, ts);
        putBe32(rtp + 8, s.ssrc);
        const size_t rtp_len = static_cast<size_t>(q - rtp);
        p[0] = '$';
        p[1] = s.channel;
        putBe16(p + 2, static_cast<uint16_t>(rtp_len));
        if (!sendAll(s.fd, p, rtp_len + 4)) return false;
    }
    return true;
}

// Handles one complete request in s.req. Returns false to close.
bool handleRequest(Session& s) {
    char method[16] = {}, url[256] = {}, cseq[16] = "0";
    if (std::sscanf(s.req, "%15s %255s", method, url) != 2) return false;
    header(s.req, "CSeq", cseq, sizeof(cseq));
    char* extra = s.extra;
    constexpr size_t kExtra = sizeof(Session::extra);

    if (std::strcmp(method, "OPTIONS") == 0) {
        return reply(s, 200, "OK", cseq,
                     "Public: OPTIONS, DESCRIBE, SETUP, PLAY, PAUSE, "
                     "TEARDOWN, GET_PARAMETER\r\n",
                     nullptr);
    }
    if (std::strcmp(method, "TEARDOWN") == 0) {
        reply(s, 200, "OK", cseq, nullptr, nullptr);
        return false;
    }
    if (!checkAuth(s, method, cseq)) return true;  // 401 sent; client retries

    if (std::strcmp(method, "DESCRIBE") == 0) {
        char* sdp = s.sdp;
        std::snprintf(sdp, sizeof(s.sdp),
                      "v=0\r\no=- %u 1 IN IP4 0.0.0.0\r\ns=VisionCam Pro\r\n"
                      "c=IN IP4 0.0.0.0\r\nt=0 0\r\na=control:*\r\n"
                      "m=video 0 RTP/AVP 26\r\na=rtpmap:26 JPEG/90000\r\n"
                      "a=control:track1\r\n",
                      static_cast<unsigned>(s.session_id));
        const size_t ulen = std::strlen(url);
        std::snprintf(extra, kExtra,
                      "Content-Base: %s%s\r\nContent-Type: application/sdp\r\n",
                      url, (ulen && url[ulen - 1] == '/') ? "" : "/");
        return reply(s, 200, "OK", cseq, extra, sdp);
    }
    if (std::strcmp(method, "SETUP") == 0) {
        char tr[160] = {};
        header(s.req, "Transport", tr, sizeof(tr));
        if (std::strstr(tr, "TCP") == nullptr && std::strstr(tr, "interleaved") == nullptr) {
            return reply(s, 461, "Unsupported Transport", cseq, nullptr, nullptr);
        }
        unsigned a = 0, b = 1;
        const char* il = std::strstr(tr, "interleaved=");
        if (il) std::sscanf(il + 12, "%u-%u", &a, &b);
        if (a > 254) a = 0;
        s.channel = static_cast<uint8_t>(a);
        s.setup = true;
        std::snprintf(extra, kExtra,
                      "Transport: RTP/AVP/TCP;unicast;interleaved=%u-%u;ssrc=%08X\r\n"
                      "Session: %08X;timeout=60\r\n",
                      a, a + 1, static_cast<unsigned>(s.ssrc),
                      static_cast<unsigned>(s.session_id));
        return reply(s, 200, "OK", cseq, extra, nullptr);
    }
    if (std::strcmp(method, "PLAY") == 0) {
        if (!s.setup) {
            return reply(s, 455, "Method Not Valid in This State", cseq, nullptr, nullptr);
        }
        if (!s.subscribed) {
            if (g_playing.fetch_add(1) >= kRtspMaxPlaying) {
                g_playing.fetch_sub(1);
                g_busy_rejects.fetch_add(1);
                return reply(s, 453, "Not Enough Bandwidth", cseq, nullptr, nullptr);
            }
            if (frameManagerSubscribe(&s.sub, "rtsp") != ESP_OK) {
                g_playing.fetch_sub(1);
                g_busy_rejects.fetch_add(1);
                return reply(s, 453, "Not Enough Bandwidth", cseq, nullptr, nullptr);
            }
            s.subscribed = true;
        }
        s.playing = true;
        std::snprintf(extra, kExtra,
                      "Session: %08X\r\nRange: npt=0.000-\r\n",
                      static_cast<unsigned>(s.session_id));
        ESP_LOGI(TAG, "PLAY from %s", s.peer);
        return reply(s, 200, "OK", cseq, extra, nullptr);
    }
    if (std::strcmp(method, "PAUSE") == 0) {
        s.playing = false;
        std::snprintf(extra, kExtra, "Session: %08X\r\n",
                      static_cast<unsigned>(s.session_id));
        return reply(s, 200, "OK", cseq, extra, nullptr);
    }
    if (std::strcmp(method, "GET_PARAMETER") == 0 ||
        std::strcmp(method, "SET_PARAMETER") == 0) {
        std::snprintf(extra, kExtra, "Session: %08X\r\n",
                      static_cast<unsigned>(s.session_id));
        return reply(s, 200, "OK", cseq, extra, nullptr);
    }
    return reply(s, 501, "Not Implemented", cseq, nullptr, nullptr);
}

// Reads what is available (waiting up to wait_ms) and processes complete
// requests and interleaved client packets (RTCP receiver reports).
bool pumpInput(Session& s, uint32_t wait_ms) {
    if (!waitReadable(s.fd, wait_ms)) return true;
    const size_t room = kRtspRequestMax - s.req_len;
    if (room == 0) return false;  // request larger than the bound
    const int r = recv(s.fd, s.req + s.req_len, room, 0);
    if (r <= 0) return false;      // closed or error
    s.req_len += static_cast<size_t>(r);
    s.req[s.req_len] = '\0';

    for (;;) {
        if (s.req_len == 0) return true;
        size_t consumed = 0;
        if (s.req[0] == '$') {
            if (s.req_len < 4) return true;
            const size_t n = 4 + ((static_cast<uint8_t>(s.req[2]) << 8) |
                                  static_cast<uint8_t>(s.req[3]));
            if (n > kRtspRequestMax) return false;
            if (s.req_len < n) return true;
            consumed = n;
        } else {
            const char* end = std::strstr(s.req, "\r\n\r\n");
            if (end == nullptr) return true;
            size_t hdr_len = static_cast<size_t>(end - s.req) + 4;
            char clen[12] = {};
            size_t body = 0;
            if (header(s.req, "Content-Length", clen, sizeof(clen)))
                body = static_cast<size_t>(std::strtoul(clen, nullptr, 10));
            if (hdr_len + body > kRtspRequestMax) return false;
            if (s.req_len < hdr_len + body) return true;
            const char saved = s.req[hdr_len];
            s.req[hdr_len] = '\0';
            const bool keep = handleRequest(s);
            s.req[hdr_len] = saved;
            if (!keep) return false;
            consumed = hdr_len + body;
        }
        std::memmove(s.req, s.req + consumed, s.req_len - consumed);
        s.req_len -= consumed;
        s.req[s.req_len] = '\0';
    }
}

void sessionRun(Session& s) {
    const int64_t opened = esp_timer_get_time();
    while (s.generation == g_generation.load()) {
        if (!s.playing) {
            if (!pumpInput(s, 1000)) return;
            if (!s.playing &&
                esp_timer_get_time() - opened > int64_t{kSetupIdleMs} * 1000) {
                return;  // never started playing
            }
            continue;
        }
        if (!pumpInput(s, 0)) return;
        if (!s.playing) continue;

        FrameLease lease{};
        if (!frameAcquire(s.sub, lease, 1000)) continue;
        const size_t len = lease.len;
        const uint64_t ts = lease.ts_us;
        bool have = false;
        if (len > 0 && frameReserve(s, len)) {
            have = frameLeaseCopyTo(lease, s.frame, s.frame_cap) == ESP_OK;
        }
        frameRelease(lease);  // never hold a lease across network I/O
        if (!have) {
            g_frames_dropped.fetch_add(1);
            continue;
        }
        if (!sendJpeg(s, s.frame, len, ts)) {
            g_frames_dropped.fetch_add(1);
            return;  // client too slow or gone
        }
        g_frames_sent.fetch_add(1);
    }
}

void sessionTask(void* arg) {
    auto* s = static_cast<Session*>(arg);
    sessionRun(*s);
    if (s->subscribed) {
        frameManagerUnsubscribe(s->sub);
        g_playing.fetch_sub(1);
    }
    // Record the worst stack headroom any session reached (sizing evidence).
    const uint32_t hwm = uxTaskGetStackHighWaterMark(nullptr);
    uint32_t prev = g_stack_min_free.load();
    while (hwm < prev && !g_stack_min_free.compare_exchange_weak(prev, hwm)) {}
    shutdown(s->fd, SHUT_RDWR);
    close(s->fd);
    ESP_LOGI(TAG, "session from %s closed", s->peer);
    heap_caps_free(s->frame);
    heap_caps_free(s);
    g_clients.fetch_sub(1);
    vTaskDelete(nullptr);
}

void startSession(int fd, const sockaddr_in& peer) {
    if (g_clients.fetch_add(1) >= kRtspMaxClients) {
        g_clients.fetch_sub(1);
        g_busy_rejects.fetch_add(1);
        const char busy[] = "RTSP/1.0 503 Service Unavailable\r\nCSeq: 0\r\n\r\n";
        send(fd, busy, sizeof(busy) - 1, 0);
        close(fd);
        return;
    }
    void* mem = heap_caps_calloc(1, sizeof(Session), MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (mem == nullptr) {
        g_clients.fetch_sub(1);
        close(fd);
        setError("out of memory");
        return;
    }
    auto* s = new (mem) Session();
    s->fd = fd;
    s->generation = g_generation.load();
    inet_ntoa_r(peer.sin_addr, s->peer, sizeof(s->peer));
    uint8_t rnd[16];
    esp_fill_random(rnd, sizeof(rnd));
    for (int i = 0; i < 16; ++i) std::snprintf(s->nonce + i * 2, 3, "%02x", rnd[i]);
    s->session_id = esp_random();
    s->ssrc = esp_random();
    s->seq = static_cast<uint16_t>(esp_random());

    timeval tv{kIoTimeoutMs / 1000, 0};
    setsockopt(fd, SOL_SOCKET, SO_SNDTIMEO, &tv, sizeof(tv));
    int one = 1;
    setsockopt(fd, IPPROTO_TCP, TCP_NODELAY, &one, sizeof(one));

    if (xTaskCreate(sessionTask, "vc_rtsp_s", VC_STACK_RTSP_SESSION, s,
                    VC_PRIO_RTSP, nullptr) != pdPASS) {
        heap_caps_free(s);
        close(fd);
        g_clients.fetch_sub(1);
        setError("session task create failed");
        return;
    }
    g_sessions_total.fetch_add(1);
    ESP_LOGI(TAG, "session from %s", s->peer);
}

int openListener(uint16_t port) {
    const int fd = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
    if (fd < 0) return -1;
    int one = 1;
    setsockopt(fd, SOL_SOCKET, SO_REUSEADDR, &one, sizeof(one));
    sockaddr_in a{};
    a.sin_family = AF_INET;
    a.sin_port = htons(port);
    a.sin_addr.s_addr = htonl(INADDR_ANY);
    if (bind(fd, reinterpret_cast<sockaddr*>(&a), sizeof(a)) != 0 ||
        listen(fd, 2) != 0) {
        close(fd);
        return -1;
    }
    return fd;
}

void listenerTask(void*) {
    for (;;) {
        bool enabled;
        uint16_t port;
        {
            Lock l;
            enabled = g_cfg.enabled && g_cfg.password_set && g_cfg.username[0];
            port = g_cfg.port;
        }
        const uint32_t gen = g_generation.load();
        if (!enabled) {
            g_running = false;
            ulTaskNotifyTake(pdTRUE, portMAX_DELAY);  // wait for a config change
            continue;
        }
        const int lfd = openListener(port);
        if (lfd < 0) {
            g_running = false;
            setError("cannot listen on port");
            ESP_LOGE(TAG, "cannot listen on port %u", static_cast<unsigned>(port));
            ulTaskNotifyTake(pdTRUE, pdMS_TO_TICKS(10000));
            continue;
        }
        g_running = true;
        setError("");
        ESP_LOGI(TAG, "RTSP listening on port %u", static_cast<unsigned>(port));
        while (gen == g_generation.load()) {
            if (!waitReadable(lfd, 1000)) continue;
            sockaddr_in peer{};
            socklen_t plen = sizeof(peer);
            const int cfd = accept(lfd, reinterpret_cast<sockaddr*>(&peer), &plen);
            if (cfd >= 0) startSession(cfd, peer);
        }
        close(lfd);
        g_running = false;
    }
}

esp_err_t loadConfig() {
    nvs_handle_t h;
    esp_err_t err = nvs_open(kNvsNs, NVS_READONLY, &h);
    if (err == ESP_ERR_NVS_NOT_FOUND) return ESP_OK;  // defaults: disabled
    if (err != ESP_OK) return err;
    uint32_t ver = 0;
    if (nvs_get_u32(h, kKeyVer, &ver) == ESP_OK && ver == kRtspConfigSchema) {
        uint8_t en = 0;
        nvs_get_u8(h, kKeyEnabled, &en);
        uint16_t port = kRtspDefaultPort;
        nvs_get_u16(h, kKeyPort, &port);
        size_t ulen = sizeof(g_cfg.username);
        if (nvs_get_str(h, kKeyUser, g_cfg.username, &ulen) != ESP_OK)
            g_cfg.username[0] = '\0';
        size_t hlen = sizeof(g_ha1);
        if (nvs_get_str(h, kKeyHa1, g_ha1, &hlen) != ESP_OK || hlen != 33)
            g_ha1[0] = '\0';
        g_cfg.enabled = en != 0;
        g_cfg.port = port ? port : kRtspDefaultPort;
        g_cfg.password_set = g_ha1[0] != '\0';
    }
    nvs_close(h);
    return ESP_OK;
}

}  // namespace

esp_err_t rtspServerInit() {
    if (g_lock == nullptr) g_lock = xSemaphoreCreateMutex();
    if (g_lock == nullptr) return ESP_ERR_NO_MEM;
    const esp_err_t err = loadConfig();
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "config load failed: %s - RTSP disabled", esp_err_to_name(err));
    }
    if (g_listener == nullptr &&
        xTaskCreate(listenerTask, "vc_rtsp", VC_STACK_RTSP_LISTEN, nullptr,
                    VC_PRIO_RTSP, &g_listener) != pdPASS) {
        g_listener = nullptr;
        return ESP_ERR_NO_MEM;
    }
    return ESP_OK;
}

esp_err_t rtspServerGetConfig(RtspConfig* out) {
    if (out == nullptr || g_lock == nullptr) return ESP_ERR_INVALID_ARG;
    Lock l;
    *out = g_cfg;
    return ESP_OK;
}

esp_err_t rtspServerSetConfig(bool enabled, uint16_t port,
                              const char* username, const char* password) {
    if (g_lock == nullptr) return ESP_ERR_INVALID_STATE;
    if (port == 0 || username == nullptr || !validUsername(username))
        return ESP_ERR_INVALID_ARG;
    const bool new_pw = password != nullptr && password[0] != '\0';
    if (new_pw && !validPassword(password)) return ESP_ERR_INVALID_ARG;

    RtspConfig cand;
    char ha1[33];
    {
        Lock l;
        cand = g_cfg;
        std::memcpy(ha1, g_ha1, sizeof(ha1));
    }
    const bool user_changed = std::strcmp(cand.username, username) != 0;
    // HA1 binds the username: a new username needs the password again.
    if (user_changed && !new_pw) return ESP_ERR_INVALID_STATE;
    if (new_pw) {
        const char* parts[] = {username, kRtspRealm, password};
        md5Hex(parts, 3, ha1);
    }
    if (enabled && ha1[0] == '\0') return ESP_ERR_INVALID_STATE;
    cand.enabled = enabled;
    cand.port = port;
    std::snprintf(cand.username, sizeof(cand.username), "%s", username);
    cand.password_set = ha1[0] != '\0';

    nvs_handle_t h;
    esp_err_t err = nvs_open(kNvsNs, NVS_READWRITE, &h);
    if (err != ESP_OK) return err;
    err = nvs_set_u32(h, kKeyVer, kRtspConfigSchema);
    if (err == ESP_OK) err = nvs_set_u8(h, kKeyEnabled, cand.enabled ? 1 : 0);
    if (err == ESP_OK) err = nvs_set_u16(h, kKeyPort, cand.port);
    if (err == ESP_OK) err = nvs_set_str(h, kKeyUser, cand.username);
    if (err == ESP_OK && ha1[0]) err = nvs_set_str(h, kKeyHa1, ha1);
    if (err == ESP_OK) err = nvs_commit(h);
    nvs_close(h);
    if (err != ESP_OK) return err;   // last-known-good stays active

    {
        Lock l;
        g_cfg = cand;
        std::memcpy(g_ha1, ha1, sizeof(g_ha1));
    }
    g_generation.fetch_add(1);      // listener + sessions restart
    if (g_listener) xTaskNotifyGive(g_listener);
    ESP_LOGI(TAG, "config saved: enabled=%d port=%u user=%u B",
             cand.enabled ? 1 : 0, static_cast<unsigned>(cand.port),
             static_cast<unsigned>(std::strlen(cand.username)));
    return ESP_OK;
}

esp_err_t rtspServerStatus(RtspStatus* out) {
    if (out == nullptr || g_lock == nullptr) return ESP_ERR_INVALID_ARG;
    out->running = g_running.load();
    out->clients = g_clients.load();
    out->sessions_total = g_sessions_total.load();
    out->auth_failures = g_auth_failures.load();
    out->frames_sent = g_frames_sent.load();
    out->frames_dropped = g_frames_dropped.load();
    out->playing = g_playing.load();
    out->busy_rejects = g_busy_rejects.load();
    const uint32_t m = g_stack_min_free.load();
    out->stack_min_free = (m == UINT32_MAX) ? 0 : m;
    Lock l;
    std::snprintf(out->last_error, sizeof(out->last_error), "%s", g_last_error);
    return ESP_OK;
}

}  // namespace visioncam
