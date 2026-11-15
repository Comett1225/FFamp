#include "audio_codec.h"
#ifdef ABSENT_MPG123
#include <cstring>
#include <mpg123.h>
#include <mutex>

namespace absent {

namespace {

const int kMpegRates[4][3] = {{11025, 12000, 8000}, {0, 0, 0}, {22050, 24000, 16000}, {44100, 48000, 32000}};

struct Mpg123Audio : PacketDecoder {
    mpg123_handle* handle = nullptr;

    ~Mpg123Audio() override {
        if (handle) mpg123_delete(handle);
    }

    bool open() {
        if (handle) mpg123_delete(handle);
        int problem = 0;
        handle = mpg123_new(nullptr, &problem);
        if (!handle) return false;
        mpg123_param(handle, MPG123_FLAGS, MPG123_QUIET | MPG123_NO_READAHEAD, 0);
        mpg123_param(handle, MPG123_REMOVE_FLAGS, MPG123_GAPLESS, 0);
        mpg123_format_none(handle);
        mpg123_format(handle, rate, channels == 1 ? MPG123_MONO : MPG123_STEREO, MPG123_ENC_FLOAT_32);
        return mpg123_open_feed(handle) == MPG123_OK;
    }

    void reset() override { open(); }

    void feed(const std::vector<uint8_t>& packet, std::vector<float>& out) override {
        if (!handle || packet.empty() || mpg123_feed(handle, packet.data(), packet.size()) != MPG123_OK) return;
        for (;;) {
            int64_t number = 0;
            unsigned char* audio = nullptr;
            size_t bytes = 0;
            const int status = mpg123_decode_frame64(handle, &number, &audio, &bytes);
            if (status == MPG123_NEW_FORMAT) continue;
            if (status != MPG123_OK) break;
            const size_t values = bytes / sizeof(float);
            const size_t at = out.size();
            out.resize(at + values);
            memcpy(out.data() + at, audio, values * sizeof(float));
        }
    }
};

}

std::unique_ptr<PacketDecoder> openMpg123Audio(const Track&, const std::vector<uint8_t>& firstPacket, std::string* error) {
    auto fail = [&](const char* text) {
        if (error) *error = text;
        return nullptr;
    };
    static std::once_flag started;
    std::call_once(started, [] { mpg123_init(); });
    const uint8_t* p = firstPacket.data();
    if (firstPacket.size() < 4 || p[0] != 0xFF || (p[1] & 0xE0) != 0xE0) return fail("MPEG audio header not understood");
    const int version = (p[1] >> 3) & 3, layer = 4 - ((p[1] >> 1) & 3), index = (p[2] >> 2) & 3;
    if (version == 1 || layer == 4 || index == 3) return fail("MPEG audio header not understood");
    auto decoder = std::make_unique<Mpg123Audio>();
    decoder->rate = kMpegRates[version][index];
    decoder->channels = (p[3] >> 6) == 3 ? 1 : 2;
    decoder->packetFrames = layer == 1 ? 384 : (layer == 3 && version != 3) ? 576 : 1152;
    decoder->prerollPackets = 12;
    if (!decoder->open()) return fail("the MPEG audio decoder did not start");
    return decoder;
}

}
#else

namespace absent {

std::unique_ptr<PacketDecoder> openMpg123Audio(const Track&, const std::vector<uint8_t>&, std::string* error) {
    if (error) *error = "built without an MPEG audio decoder";
    return nullptr;
}

}
#endif
