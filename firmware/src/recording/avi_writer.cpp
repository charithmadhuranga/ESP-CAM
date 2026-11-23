// avi_writer.cpp - RIFF/AVI (MJPEG) segment builder (see avi_writer.h).

#include "avi_writer.h"

#include <cstring>

namespace visioncam {

namespace {

// Sequential little-endian writer over a caller buffer whose size has
// already been checked.
struct Out {
    uint8_t* p;
    void u32(uint32_t v) {
        p[0] = static_cast<uint8_t>(v);
        p[1] = static_cast<uint8_t>(v >> 8);
        p[2] = static_cast<uint8_t>(v >> 16);
        p[3] = static_cast<uint8_t>(v >> 24);
        p += 4;
    }
    void u16(uint16_t v) {
        p[0] = static_cast<uint8_t>(v);
        p[1] = static_cast<uint8_t>(v >> 8);
        p += 2;
    }
    void cc(const char (&s)[5]) {
        std::memcpy(p, s, 4);
        p += 4;
    }
};

constexpr uint32_t kAvihBytes = 56;
constexpr uint32_t kStrhBytes = 56;
constexpr uint32_t kStrfBytes = 40;
// LIST sizes count the form type plus contents.
constexpr uint32_t kStrlListSize =
    4 + (kChunkHeaderBytes + kStrhBytes) + (kChunkHeaderBytes + kStrfBytes);
constexpr uint32_t kHdrlListSize =
    4 + (kChunkHeaderBytes + kAvihBytes) + (kChunkHeaderBytes + kStrlListSize);

static_assert(kStrlListSize == 116, "strl layout");
static_assert(kHdrlListSize == 192, "hdrl layout");
static_assert(kListHeaderBytes + kChunkHeaderBytes + kHdrlListSize +
                  kListHeaderBytes == kAviHeaderBytes,
              "header layout");
static_assert(kAviMoviFourccOffset == kAviHeaderBytes - 4, "movi offset");

}  // namespace

size_t aviBuildHeader(const AviStreamInfo& in, uint8_t* out, size_t out_cap) {
    if (out == nullptr || out_cap < kAviHeaderBytes) return 0;

    // Frame rate as the rational dwRate/dwScale. With a measured duration
    // the rate is exact (frames per duration); otherwise nominal fps.
    uint32_t rate = in.nominal_fps > 0 ? in.nominal_fps : 1;
    uint32_t scale = 1;
    if (in.frames > 0 && in.duration_ms > 0) {
        rate = in.frames * 1000u;  // <= 7.2e6, fits
        scale = in.duration_ms;
    }
    const uint32_t us_per_frame = static_cast<uint32_t>(
        (static_cast<uint64_t>(scale) * 1000000u) / rate);
    const uint32_t max_bytes_per_sec = static_cast<uint32_t>(
        (static_cast<uint64_t>(in.max_frame_bytes) * rate) / scale);

    const uint32_t riff_size =
        aviFileBytes(in.movi_payload_bytes, in.frames) - kChunkHeaderBytes;

    Out o{out};
    o.cc("RIFF"); o.u32(riff_size); o.cc("AVI ");

    o.cc("LIST"); o.u32(kHdrlListSize); o.cc("hdrl");

    o.cc("avih"); o.u32(kAvihBytes);
    o.u32(us_per_frame);           // dwMicroSecPerFrame
    o.u32(max_bytes_per_sec);      // dwMaxBytesPerSec
    o.u32(0);                      // dwPaddingGranularity
    o.u32(kAviIdxFlagsKeyframe);   // dwFlags = AVIF_HASINDEX (0x10)
    o.u32(in.frames);              // dwTotalFrames
    o.u32(0);                      // dwInitialFrames
    o.u32(1);                      // dwStreams
    o.u32(in.max_frame_bytes);     // dwSuggestedBufferSize
    o.u32(in.width);               // dwWidth
    o.u32(in.height);              // dwHeight
    o.u32(0); o.u32(0); o.u32(0); o.u32(0);  // dwReserved[4]

    o.cc("LIST"); o.u32(kStrlListSize); o.cc("strl");

    o.cc("strh"); o.u32(kStrhBytes);
    o.cc("vids");                  // fccType
    o.cc("MJPG");                  // fccHandler
    o.u32(0);                      // dwFlags
    o.u16(0); o.u16(0);            // wPriority, wLanguage
    o.u32(0);                      // dwInitialFrames
    o.u32(scale);                  // dwScale
    o.u32(rate);                   // dwRate
    o.u32(0);                      // dwStart
    o.u32(in.frames);              // dwLength
    o.u32(in.max_frame_bytes);     // dwSuggestedBufferSize
    o.u32(0xffffffffu);            // dwQuality (-1 = default)
    o.u32(0);                      // dwSampleSize (variable)
    o.u16(0); o.u16(0);            // rcFrame.left, top
    o.u16(static_cast<uint16_t>(in.width));
    o.u16(static_cast<uint16_t>(in.height));

    o.cc("strf"); o.u32(kStrfBytes);
    o.u32(kStrfBytes);             // biSize
    o.u32(in.width);               // biWidth
    o.u32(in.height);              // biHeight
    o.u16(1);                      // biPlanes
    o.u16(24);                     // biBitCount
    o.cc("MJPG");                  // biCompression
    o.u32(in.width * in.height * 3);  // biSizeImage
    o.u32(0); o.u32(0);            // biX/YPelsPerMeter
    o.u32(0); o.u32(0);            // biClrUsed, biClrImportant

    o.cc("LIST"); o.u32(4 + in.movi_payload_bytes); o.cc("movi");

    return static_cast<size_t>(o.p - out);  // == kAviHeaderBytes (asserted)
}

void aviBuildFrameChunkHeader(uint32_t jpeg_len,
                              uint8_t out[kChunkHeaderBytes]) {
    Out o{out};
    o.cc("00dc");
    o.u32(jpeg_len);  // unpadded length; the pad byte is not counted
}

void aviBuildIdxEntry(uint32_t offset_from_movi, uint32_t jpeg_len,
                      uint8_t out[kIdxEntryBytes]) {
    Out o{out};
    o.cc("00dc");
    o.u32(kAviIdxFlagsKeyframe);
    o.u32(offset_from_movi);
    o.u32(jpeg_len);
}

void aviBuildIdx1Header(uint32_t frames, uint8_t out[kChunkHeaderBytes]) {
    Out o{out};
    o.cc("idx1");
    o.u32(frames * kIdxEntryBytes);
}

}  // namespace visioncam
