// avi_writer.h - RIFF/AVI (MJPEG) segment builder (Phase 3 task 3.1; SPEC 4.6).
//
// Pure byte builders: no I/O, no allocation, no IDF calls, so the layout is
// unit-testable on the host (test/host/avi_writer_test.cpp, validated with
// ffprobe). Written from the public AVI/RIFF specification - never from R1
// (clean-room rule, AGENTS rules 31-36). All integers little-endian.
//
// File layout produced by the recording manager:
//
//   offset 0    RIFF <size> 'AVI '
//   offset 12     LIST <192> 'hdrl'
//   offset 24       'avih' <56> MainAVIHeader
//   offset 88       LIST <116> 'strl'
//   offset 100        'strh' <56> AVIStreamHeader     (vids / MJPG)
//   offset 164        'strf' <40> BITMAPINFOHEADER    (MJPG)
//   offset 212    LIST <4 + movi payload> 'movi'
//   offset 224      '00dc' <jpeg_len> JPEG [pad to even]   x frames
//                 'idx1' <frames*16> { '00dc', KEYFRAME, offset, len } x frames
//
// The header is written provisionally at segment open (sizes 0) and then
// REBUILT in full from final values at close - nothing is patched at
// hand-computed offsets. idx1 offsets are relative to the 'movi' fourcc
// (first chunk = 4), the convention every mainstream demuxer accepts.

#pragma once

#include <cstddef>
#include <cstdint>

namespace visioncam {

// Fixed sizes.
constexpr uint32_t kChunkHeaderBytes = 8;    // fourcc + size
constexpr uint32_t kListHeaderBytes = 12;    // 'LIST' + size + form type
constexpr uint32_t kIdxEntryBytes = 16;      // one idx1 entry
constexpr uint32_t kAviHeaderBytes = 224;    // through the 'movi' fourcc
constexpr uint32_t kAviMoviFourccOffset = 220;  // file offset of 'movi'

// Bounds (AGENTS rule 12/13).
constexpr uint32_t kAviMaxFramesPerSegment = 7200;  // 60 s @ 120 fps headroom
constexpr size_t kAviIndexMaxBytes =
    kChunkHeaderBytes + kAviMaxFramesPerSegment * kIdxEntryBytes;

constexpr uint32_t kFourccMovi = 0x69766f6d;  // "movi"
constexpr uint32_t kFourcc00dc = 0x63643030;  // "00dc"
constexpr uint32_t kAviIdxFlagsKeyframe = 0x00000010;  // AVIIF_KEYFRAME

// RIFF chunks are padded to an even length (AVI spec).
constexpr uint32_t aviPadEven(uint32_t n) { return (n + 1u) & ~1u; }

struct AviStreamInfo {
    uint32_t width = 0;
    uint32_t height = 0;
    uint32_t frames = 0;              // frames written (= idx1 entries)
    uint32_t duration_ms = 0;         // capture span; 0 => use nominal_fps
    uint32_t nominal_fps = 0;         // configured fps (provisional header)
    uint32_t max_frame_bytes = 0;     // largest JPEG (dwSuggestedBufferSize)
    uint32_t movi_payload_bytes = 0;  // sum of 8 + padded JPEG per frame
};

// Writes the complete kAviHeaderBytes header. Returns kAviHeaderBytes, or 0
// if out_cap is too small.
size_t aviBuildHeader(const AviStreamInfo& info, uint8_t* out, size_t out_cap);

// Writes the 8-byte '00dc' chunk header for a JPEG of jpeg_len bytes. The
// caller writes the JPEG, then aviPadEven(jpeg_len) - jpeg_len zero bytes.
void aviBuildFrameChunkHeader(uint32_t jpeg_len, uint8_t out[kChunkHeaderBytes]);

// Writes one 16-byte idx1 entry. offset_from_movi is the chunk header's
// offset relative to the 'movi' fourcc (the first frame is at 4).
void aviBuildIdxEntry(uint32_t offset_from_movi, uint32_t jpeg_len,
                      uint8_t out[kIdxEntryBytes]);

// Writes the 8-byte 'idx1' chunk header for `frames` entries.
void aviBuildIdx1Header(uint32_t frames, uint8_t out[kChunkHeaderBytes]);

// Total file size for a finalized segment.
constexpr uint32_t aviFileBytes(uint32_t movi_payload_bytes, uint32_t frames) {
    return kAviHeaderBytes + movi_payload_bytes + kChunkHeaderBytes +
           frames * kIdxEntryBytes;
}

}  // namespace visioncam
