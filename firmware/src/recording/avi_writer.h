// avi_writer.h - RIFF/AVI segment writer (Phase 3 task 3.1; SPEC section 4.6).
//
// Builds a RIFF/AVI file from JPEG frames, written fresh from the public
// AVI/RIFF specification - never from R1's 310-byte header byte table
// (clean-room licence rule, AGENTS rules 31-36).
//
// On-disk layout (all multi-byte integers little-endian):
//
//   RIFF "AVI " container
//     LIST "hdrl"
//       avih      main AVI header (dwMicroSecPerFrame, dwTotalFrames, ...)
//       LIST "strl"
//         strh    video stream header (fccType="vids", fccHandler="MJPG", ...)
//         strf    BITMAPINFOHEADER (width, height, biCompression="MJPG", ...)
//     LIST "movi"
//       00dc [size] [JPEG bytes] [0-3 pad bytes to 4-byte boundary] x N
//     idx1 [4-byte size]  [4-byte id "00dc", 4-byte flags, 4-byte offset,
//                          4-byte size] x N
//
// Design notes:
//
//   * Index is built in a PSRAM buffer sized (maxFrames + 2) * 16 bytes,
//     bounded by the per-segment frame cap - the segment rotates instead
//     of rebooting (SPEC 4.6 item 2).
//   * Frames are staged in an 8 KB (SD sector multiple) RAM buffer and
//     written to SD in whole buffers only - never many small writes
//     (SPEC 4.6 item 3).
//   * A provisional header is written at segment start; the final header
//     + idx1 are patched at close (SPEC 4.6 item 8).
//   * Header stores MEASURED FPS (frames / elapsed), not requested FPS
//     (SPEC 4.6 item 5).
//
// All file I/O goes through IStorageBackend (AGENTS rule 10). The writer
// task owns the file; avi_writer itself is a pure function library with
// no file handles - the task orchestrates open/write/close/rename.
//
// Bounds (AGENTS rules 12-13):
//   kAviIndexMaxBytes  - PSRAM index ceiling, derived from maxFrames, not
//                        from any request or media size.
//   kAviStageBytes     - fixed 8192 staging buffer, never sized from input.

#pragma once

#include <cstddef>
#include <cstdint>

#include "esp_err.h"

namespace visioncam {

// ---- RIFF/AVI fourcc codes (public format, written fresh) ----

constexpr uint32_t kFourccRiff   = 0x46464952; // "RIFF"
constexpr uint32_t kFourccAvi    = 0x20495641; // "AVI "
constexpr uint32_t kFourccList   = 0x5453494c; // "LIST"
constexpr uint32_t kFourccHdrl   = 0x6c726468; // "hdrl"
constexpr uint32_t kFourccAvif   = 0x68697661; // "avih"
constexpr uint32_t kFourccStrl   = 0x6c727473; // "strl"
constexpr uint32_t kFourccStrh   = 0x68727473; // "strh"
constexpr uint32_t kFourccStrf   = 0x66727473; // "strf"
constexpr uint32_t kFourccMovi   = 0x69766f6d; // "movi"
constexpr uint32_t kFourccIdx1   = 0x31786469; // "idx1"
constexpr uint32_t kFourcc00dc   = 0x63643030; // "00dc" (little-endian)
constexpr uint32_t kFourccMjpg   = 0x47504a4d; // "MJPG" (little-endian)
constexpr uint32_t kFourccDib    = 0x42494442; // "BITMAPINFOHEADER" tag

// ---- Fixed structural sizes ----

constexpr uint32_t kRiffSizeField     = 4;   // "RIFF" + size + "AVI "
constexpr uint32_t kListHeaderBytes   = 12;  // LIST + size + form type
constexpr uint32_t kChunkHeaderBytes  = 8;   // fourcc + size
constexpr uint32_t kIdxEntryBytes     = 16;  // per-frame idx1 entry
constexpr uint32_t kAviMainHeaderBytes = 56; // avih structure size
constexpr uint32_t kAviStrhHeaderBytes = 56; // strh structure size
constexpr uint32_t kAviBitmapInfoHeaderBytes = 40; // BITMAPINFOHEADER size
// Total RIFF/AVI header size (fixed for all resolutions):
//   RIFF(12) + LIST hdrl(12 + avih(8+56) + LIST strl(12 + strh(8+56) +
//   strf(8+40))) + LIST movi(12+8) = 232 bytes
constexpr uint32_t kAviHeaderBytes = 232;
constexpr uint32_t kAviHeaderMinBytes = kAviHeaderBytes;

// SD sector multiple staging buffer (SPEC 4.6 item 3).
constexpr size_t kAviStageBytes = 8192;

// PSRAM index buffer ceiling: (maxFrames + 2) * 16 (R1 formula, item 2).
// maxFrames is per-segment and bounded so this can never overflow - the
// segment rotates instead (AGENTS rule 13: never size from unbounded input).
constexpr uint32_t kAviMaxFramesPerSegment = 7200;  // 60 s @ 120 fps headroom
constexpr size_t kAviIndexMaxBytes =
    (kAviMaxFramesPerSegment + 2) * kIdxEntryBytes;

// Chunk flags for idx1 entries (AVIIF_KEYFRAME = 0x10 for every JPEG frame).
constexpr uint32_t kAviIdxFlagsKeyframe = 0x00000010;

// ---- Opaque recording ID -------------------------------------------------

// Defined in storage_backend.h as kRecordingIdPrefix ("rec-").

// ---- AVI header fields (patched at close, provisional at open) -----------

struct AviHeaderFields {
    uint32_t dwMicroSecPerFrame = 0;   // 1e6 / measured_fps
    uint32_t dwTotalFrames = 0;
    uint32_t dwWidth = 0;
    uint32_t dwHeight = 0;
    uint32_t dwInitialFrames = 0;
    uint32_t dwScale = 1;
    uint32_t dwRate = 0;               // measured_fps
    uint32_t dwStart = 0;
    uint32_t dwLength = 0;
    uint32_t dwSuggestedBufferSize = 0;
    uint32_t dwQuality = 0xffffffff;   // -1 = default
    uint32_t dwSampleSize = 0;
    uint32_t dwReserved[4] = {};
};

// ---- Builder functions ---------------------------------------------------

// Writes the RIFF/AVI container header (LIST hdrl + avih + strl + strh +
// strf) into out, returning bytes written (<= out_cap). All integers are
// little-endian. This is the provisional header written at segment start;
// aviFinalizeHeader() rewrites it at close with real measured values.
size_t aviBuildHeader(uint32_t width, uint32_t height, uint32_t measured_fps,
                      uint8_t* out, size_t out_cap);

// Appends one 00dc JPEG chunk (4-byte marker + 4-byte size + JPEG +
// 0-3 pad bytes to 4-byte boundary) into out, returning bytes written
// (<= out_cap). The caller must ensure out_cap >= kChunkHeaderBytes +
// jpeg_len + 3.
size_t aviBuild00dcChunk(const uint8_t* jpeg, uint32_t jpeg_len,
                         uint8_t* out, size_t out_cap);

// Builds one idx1 entry (16 bytes) into out.
void aviBuildIdxEntry(const uint8_t* jpeg, uint32_t offset, uint32_t size,
                      uint8_t* out);

// Finalizes the AVI header: rewrites dwTotalFrames, dwMicroSecPerFrame,
// dwRate, dwLength, dwSuggestedBufferSize and the RIFF LIST sizes with
// measured values, then writes the idx1 index after all 00dc chunks.
// Returns total bytes of header_patch (written at offset 0). The idx1
// header is written to idx1_out; entries are filled by the caller via
// aviBuildIdxEntry() into idx1_out + 8 + i*16.
size_t aviFinalizeHeader(const AviHeaderFields& fields, uint32_t movi_size,
                         uint8_t* header_patch, size_t header_patch_cap,
                         uint8_t* idx1_out, size_t idx1_out_cap);

// ---- Stage buffer helper -------------------------------------------------

// Pads a length up to a 4-byte boundary (DWORD alignment for 00dc chunks).
constexpr uint32_t aviPad4(uint32_t n) { return (n + 3u) & ~3u; }

}  // namespace visioncam
