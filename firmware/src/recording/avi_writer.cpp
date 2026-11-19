// avi_writer.cpp - RIFF/AVI segment builder (Phase 3 task 3.1; SPEC 4.6).
//
// Written fresh from the public AVI/RIFF specification - never from R1's
// 310-byte header byte table (clean-room licence rule, AGENTS rules 31-36).
// All multi-byte integers are little-endian.

#include "avi_writer.h"

#include <cstring>

namespace visioncam {

namespace {

void putLe32(uint8_t* p, uint32_t v) {
    p[0] = static_cast<uint8_t>(v & 0xff);
    p[1] = static_cast<uint8_t>((v >> 8) & 0xff);
    p[2] = static_cast<uint8_t>((v >> 16) & 0xff);
    p[3] = static_cast<uint8_t>((v >> 24) & 0xff);
}

void putLe16(uint8_t* p, uint16_t v) {
    p[0] = static_cast<uint8_t>(v & 0xff);
    p[1] = static_cast<uint8_t>((v >> 8) & 0xff);
}

constexpr uint32_t kFourccVids = 0x73646976; // "vids" little-endian
constexpr uint32_t kFourccMjpg = 0x47504a4d; // "MJPG" little-endian

// avih main AVI header (56 bytes):
//   0  dwMicroSecPerFrame   DWORD
//   4  dwMaxBytesPerSec      DWORD
//   8  dwPaddingGranularity  DWORD
//   12 dwFlags               DWORD
//   16 dwTotalFrames         DWORD
//   20 dwInitialFrames       DWORD
//   24 dwStreams             DWORD
//   28 dwSuggestedBufferSize DWORD
//   32 dwWidth               DWORD
//   36 dwHeight              DWORD
//   40 dwReserved[4]         DWORD[4]
void buildAvih(uint8_t* out, const AviHeaderFields& f) {
    putLe32(out + 0,  f.dwMicroSecPerFrame);
    putLe32(out + 4,  f.dwSuggestedBufferSize);
    putLe32(out + 8,  0);                       // padding granularity
    putLe32(out + 12, 0x10);                    // AVIF_HASINDEX
    putLe32(out + 16, f.dwTotalFrames);
    putLe32(out + 20, f.dwInitialFrames);
    putLe32(out + 24, 1);                       // one stream (video)
    putLe32(out + 28, f.dwSuggestedBufferSize);
    putLe32(out + 32, f.dwWidth);
    putLe32(out + 36, f.dwHeight);
    putLe32(out + 40, 0);
    putLe32(out + 44, 0);
    putLe32(out + 48, 0);
    putLe32(out + 52, 0);
}

// strh stream header (56 bytes)
void buildStrh(uint8_t* out, uint32_t width, uint32_t height,
               const AviHeaderFields& f) {
    putLe32(out + 0,  kFourccVids);
    putLe32(out + 4,  kFourccMjpg);
    putLe32(out + 8,  0);                       // dwFlags
    putLe32(out + 12, 0);                       // dwPriority
    putLe32(out + 16, f.dwInitialFrames);
    putLe32(out + 20, f.dwScale);
    putLe32(out + 24, f.dwRate);
    putLe32(out + 28, f.dwStart);
    putLe32(out + 32, f.dwLength);
    putLe32(out + 36, f.dwSuggestedBufferSize);
    putLe32(out + 40, f.dwQuality);
    putLe32(out + 44, f.dwSampleSize);
    putLe16(out + 48, 0);                       // rcFrame.left
    putLe16(out + 50, 0);                       // rcFrame.top
    putLe16(out + 52, static_cast<uint16_t>(width));
    putLe16(out + 54, static_cast<uint16_t>(height));
}

// strf BITMAPINFOHEADER (40 bytes)
void buildStrf(uint8_t* out, uint32_t width, uint32_t height) {
    putLe32(out + 0,  kAviBitmapInfoHeaderBytes); // biSize = 40
    putLe32(out + 4,  width);
    putLe32(out + 8,  static_cast<int32_t>(height));
    putLe16(out + 12, 1);                        // biPlanes
    putLe16(out + 14, 24);                       // biBitCount
    putLe32(out + 16, kFourccMjpg);              // biCompression = MJPG
    putLe32(out + 20, 0);                        // biSizeImage
    putLe32(out + 24, 0);                        // biXPelsPerMeter
    putLe32(out + 28, 0);                        // biYPelsPerMeter
    putLe32(out + 32, 0);                        // biClrUsed
    putLe32(out + 36, 0);                        // biClrImportant
}

}  // namespace

size_t aviBuildHeader(uint32_t width, uint32_t height, uint32_t measured_fps,
                      uint8_t* out, size_t out_cap) {
    // Required size:
    //   RIFF(12) + LIST hdrl(12) + avih(8+56) + LIST strl(12) +
    //   strh(8+56) + strf(8+40) + LIST movi(12+8)
    const size_t need =
        kRiffSizeField + kListHeaderBytes +
        kChunkHeaderBytes + kAviMainHeaderBytes +
        kListHeaderBytes +
        kChunkHeaderBytes + kAviStrhHeaderBytes +
        kChunkHeaderBytes + kAviBitmapInfoHeaderBytes +
        kListHeaderBytes + kChunkHeaderBytes;
    if (out_cap < need) return 0;

    uint8_t* p = out;

    // ---- RIFF container (size patched at close) ----
    putLe32(p + 0, kFourccRiff);
    putLe32(p + 4, 0);            // RIFF size — patched by finalize
    putLe32(p + 8, kFourccAvi);
    p += kRiffSizeField;

    // ---- LIST hdrl ----
    putLe32(p + 0, kFourccList);
    putLe32(p + 4, 0);            // LIST size — patched
    putLe32(p + 8, kFourccHdrl);
    p += kListHeaderBytes;

    // ---- avih ----
    AviHeaderFields f{};
    f.dwMicroSecPerFrame = measured_fps ? (1000000u / measured_fps) : 0;
    f.dwTotalFrames = 0;
    f.dwWidth = width;
    f.dwHeight = height;
    f.dwRate = measured_fps;
    f.dwSuggestedBufferSize = width * height;
    putLe32(p + 0, kFourccAvif);
    putLe32(p + 4, kAviMainHeaderBytes);
    buildAvih(p + kChunkHeaderBytes, f);
    p += kChunkHeaderBytes + kAviMainHeaderBytes;

    // ---- LIST strl ----
    putLe32(p + 0, kFourccList);
    putLe32(p + 4, 0);            // LIST size — patched
    putLe32(p + 8, kFourccStrl);
    p += kListHeaderBytes;

    // ---- strh ----
    putLe32(p + 0, kFourccStrh);
    putLe32(p + 4, kAviStrhHeaderBytes);
    buildStrh(p + kChunkHeaderBytes, width, height, f);
    p += kChunkHeaderBytes + kAviStrhHeaderBytes;

    // ---- strf ----
    putLe32(p + 0, kFourccStrf);
    putLe32(p + 4, kAviBitmapInfoHeaderBytes);
    buildStrf(p + kChunkHeaderBytes, width, height);
    p += kChunkHeaderBytes + kAviBitmapInfoHeaderBytes;

    // ---- LIST movi (placeholder, size patched at close) ----
    putLe32(p + 0, kFourccList);
    putLe32(p + 4, 0);            // movi size — patched
    putLe32(p + 8, kFourccMovi);
    p += kListHeaderBytes + kChunkHeaderBytes;

    return static_cast<size_t>(p - out);
}

size_t aviBuild00dcChunk(const uint8_t* jpeg, uint32_t jpeg_len,
                         uint8_t* out, size_t out_cap) {
    const uint32_t padded = aviPad4(jpeg_len);
    const size_t need = kChunkHeaderBytes + padded;
    if (out_cap < need) return 0;

    putLe32(out + 0, kFourcc00dc);
    putLe32(out + 4, jpeg_len);   // chunk size = JPEG length only
    std::memcpy(out + kChunkHeaderBytes, jpeg, jpeg_len);
    if (padded > jpeg_len) {
        std::memset(out + kChunkHeaderBytes + jpeg_len, 0, padded - jpeg_len);
    }
    return need;
}

void aviBuildIdxEntry(const uint8_t* /*jpeg*/, uint32_t offset, uint32_t size,
                      uint8_t* out) {
    putLe32(out + 0,  kFourcc00dc);
    putLe32(out + 4,  kAviIdxFlagsKeyframe);
    putLe32(out + 8,  offset);
    putLe32(out + 12, size);
}

// Offsets into the header buffer for fields that aviFinalizeHeader patches.
size_t aviFinalizeHeader(const AviHeaderFields& fields, uint32_t movi_size,
                         uint8_t* header_patch, size_t header_patch_cap,
                         uint8_t* idx1_out, size_t idx1_out_cap) {
    if (header_patch_cap < kAviHeaderMinBytes) return 0;

    // Sizes:
    const uint32_t strh_total = kChunkHeaderBytes + kAviStrhHeaderBytes;  // 64
    const uint32_t strf_total = kChunkHeaderBytes + kAviBitmapInfoHeaderBytes; // 48
    const uint32_t strl_size = strh_total + strf_total;                   // 112
    const uint32_t avih_total = kChunkHeaderBytes + kAviMainHeaderBytes;  // 64
    const uint32_t hdrl_size = avih_total + kListHeaderBytes + strl_size; // 64+12+112 = 188
    const uint32_t movi_total = kListHeaderBytes + kChunkHeaderBytes + movi_size; // 12+8+movi
    const uint32_t idx1_total = kChunkHeaderBytes +
                                fields.dwTotalFrames * kIdxEntryBytes;      // 8 + frames*16

    const uint32_t riff_size = 4 + hdrl_size + movi_total + idx1_total;

    // Patch RIFF size at offset 4 (from start of header_patch)
    putLe32(header_patch + 4, riff_size);

    // Patch LIST hdrl size at offset 16
    putLe32(header_patch + 16, hdrl_size + kChunkHeaderBytes);

    // avih fields start at offset 32 (after RIFF(12) + LIST hdrl(12) + avih fourcc(4) + avih size(4))
    uint8_t* avih = header_patch + 32;
    putLe32(avih + 0,  fields.dwMicroSecPerFrame);   // dwMicroSecPerFrame
    putLe32(avih + 16, fields.dwTotalFrames);        // dwTotalFrames
    putLe32(avih + 24, fields.dwRate);               // dwRate
    putLe32(avih + 28, fields.dwSuggestedBufferSize);// dwSuggestedBufferSize
    putLe32(avih + 32, fields.dwLength);             // dwLength

    // strh fields start at offset 32 + 8(avih hdr) + 56(avih fields) + 12(LIST strl) + 8(strh hdr) = 116
    uint8_t* strh = header_patch + 116;
    putLe32(strh + 20, fields.dwScale);              // dwScale
    putLe32(strh + 24, fields.dwRate);               // dwRate
    putLe32(strh + 32, fields.dwLength);             // dwLength
    putLe32(strh + 36, fields.dwSuggestedBufferSize);// dwSuggestedBufferSize

    // strf fields start at offset 116 + 56 + 8(strf hdr) = 180
    uint8_t* strf = header_patch + 180;
    putLe32(strf + 4,  fields.dwWidth);              // biWidth
    putLe32(strf + 8, static_cast<int32_t>(fields.dwHeight)); // biHeight

    // header_patch now holds the complete patched header.
    // Return the total header size (what the caller writes at offset 0).
    const size_t header_size =
        kRiffSizeField +
        kListHeaderBytes + hdrl_size +
        kListHeaderBytes + kChunkHeaderBytes; // movi list header

    // Build idx1 header in idx1_out: "idx1" + size + entries
    const size_t idx1_need = kChunkHeaderBytes +
                             fields.dwTotalFrames * kIdxEntryBytes;
    if (idx1_out_cap < idx1_need) return 0;

    putLe32(idx1_out + 0, kFourccIdx1);
    putLe32(idx1_out + 4, fields.dwTotalFrames * kIdxEntryBytes);
    // Entries are filled by the caller via aviBuildIdxEntry() into
    // idx1_out + 8 + i*16 as frames are recorded.

    return header_size;
}

}  // namespace visioncam
