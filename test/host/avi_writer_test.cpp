// Host test for firmware/src/recording/avi_writer.cpp.
//
// Builds an AVI exactly the way recording_manager.cpp does (provisional
// header, 00dc chunks, rebuilt header, idx1) from JPEG files given on the
// command line, writes it to argv[1], and checks the header bytes. The
// shell driver (run_avi_test.sh) then validates the file with ffprobe and
// decodes every frame with ffmpeg.
//
// Run via test/host/run_avi_test.sh, or by hand:
//   ./avi_test out.avi 640 480 a.jpg b.jpg ...

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <vector>

#include "avi_writer.h"

using namespace visioncam;

static int g_fail = 0;
#define CHECK(c)                                                    \
    do {                                                            \
        if (!(c)) {                                                 \
            std::fprintf(stderr, "FAIL %s:%d %s\n", __FILE__, __LINE__, #c); \
            g_fail++;                                               \
        }                                                           \
    } while (0)

static uint32_t le32(const uint8_t* p) {
    return p[0] | (p[1] << 8) | (p[2] << 16) | (uint32_t(p[3]) << 24);
}

int main(int argc, char** argv) {
    if (argc < 5) {
        std::fprintf(stderr, "usage: %s out.avi w h jpeg...\n", argv[0]);
        return 2;
    }
    FILE* f = std::fopen(argv[1], "w+b");
    if (!f) return 2;

    AviStreamInfo info{};
    info.width = std::atoi(argv[2]);
    info.height = std::atoi(argv[3]);
    info.nominal_fps = 10;

    uint8_t hdr[kAviHeaderBytes];
    CHECK(aviBuildHeader(info, hdr, sizeof(hdr)) == kAviHeaderBytes);
    CHECK(aviBuildHeader(info, hdr, sizeof(hdr) - 1) == 0);
    std::fwrite(hdr, 1, sizeof(hdr), f);

    std::vector<uint8_t> idx(kAviIndexMaxBytes);
    for (int i = 4; i < argc; i++) {
        FILE* j = std::fopen(argv[i], "rb");
        if (!j) return 2;
        std::vector<uint8_t> jpg;
        int c;
        while ((c = std::fgetc(j)) != EOF) jpg.push_back(uint8_t(c));
        std::fclose(j);
        const uint32_t len = jpg.size();
        uint8_t ch[kChunkHeaderBytes];
        aviBuildFrameChunkHeader(len, ch);
        aviBuildIdxEntry(4 + info.movi_payload_bytes, len,
                         &idx[kChunkHeaderBytes + info.frames * kIdxEntryBytes]);
        std::fwrite(ch, 1, sizeof(ch), f);
        std::fwrite(jpg.data(), 1, len, f);
        const uint8_t pad = 0;
        if (aviPadEven(len) != len) std::fwrite(&pad, 1, 1, f);
        info.movi_payload_bytes += kChunkHeaderBytes + aviPadEven(len);
        info.frames++;
        if (len > info.max_frame_bytes) info.max_frame_bytes = len;
    }
    info.duration_ms = info.frames * 100;  // 10 fps

    CHECK(aviBuildHeader(info, hdr, sizeof(hdr)) == kAviHeaderBytes);
    std::fseek(f, 0, SEEK_SET);
    std::fwrite(hdr, 1, sizeof(hdr), f);
    std::fseek(f, 0, SEEK_END);
    aviBuildIdx1Header(info.frames, idx.data());
    std::fwrite(idx.data(), 1, kChunkHeaderBytes + info.frames * kIdxEntryBytes, f);
    const long size = std::ftell(f);
    std::fclose(f);

    // Structural checks.
    CHECK(size == long(aviFileBytes(info.movi_payload_bytes, info.frames)));
    CHECK(std::memcmp(hdr, "RIFF", 4) == 0);
    CHECK(le32(hdr + 4) == uint32_t(size) - 8);
    CHECK(std::memcmp(hdr + 12, "LIST", 4) == 0 && le32(hdr + 16) == 192);
    CHECK(le32(hdr + 24 + 8 + 16) == info.frames);  // dwTotalFrames
    CHECK(le32(hdr + 24 + 8 + 24) == 1);            // dwStreams
    CHECK(le32(hdr + 24 + 8 + 32) == info.width);   // dwWidth
    CHECK(std::memcmp(hdr + 88, "LIST", 4) == 0 && le32(hdr + 92) == 116);
    CHECK(std::memcmp(hdr + 100, "strh", 4) == 0);
    CHECK(std::memcmp(hdr + 108, "vids", 4) == 0);
    CHECK(std::memcmp(hdr + 112, "MJPG", 4) == 0);
    CHECK(std::memcmp(hdr + 164, "strf", 4) == 0);
    CHECK(std::memcmp(hdr + 172 + 16, "MJPG", 4) == 0);  // biCompression
    CHECK(std::memcmp(hdr + 212, "LIST", 4) == 0);
    CHECK(le32(hdr + 216) == 4 + info.movi_payload_bytes);
    CHECK(le32(hdr + kAviMoviFourccOffset) == kFourccMovi);

    std::printf("%s: %u frames, %ld bytes, %d check failure(s)\n", argv[1],
                info.frames, size, g_fail);
    return g_fail ? 1 : 0;
}
