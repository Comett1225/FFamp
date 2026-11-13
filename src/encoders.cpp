#include "absent/encoders.h"
#include <algorithm>

namespace absent {

std::vector<EncoderInfo> encoders() {
    std::vector<EncoderInfo> list;
#ifdef __APPLE__
    list.push_back({"h264_videotoolbox", fourcc("avc1"), Kind::Video, true});
    list.push_back({"hevc_videotoolbox", fourcc("hvc1"), Kind::Video, true});
    list.push_back({"aac_at", fourcc("mp4a"), Kind::Audio, false});
#endif
#ifdef ABSENT_SVTAV1
    list.push_back({"libsvtav1", fourcc("av01"), Kind::Video, false});
#endif
#ifdef ABSENT_VPX
    list.push_back({"libvpx-vp9", fourcc("vp09"), Kind::Video, false});
#endif
#ifdef ABSENT_OAPV
    list.push_back({"liboapv", fourcc("apv1"), Kind::Video, false});
#endif
#ifdef ABSENT_OPUS
    list.push_back({"libopus", fourcc("Opus"), Kind::Audio, false});
#endif
#ifdef ABSENT_LAME
    list.push_back({"libmp3lame", fourcc(".mp3"), Kind::Audio, false});
#endif
    list.push_back({"flac", fourcc("fLaC"), Kind::Audio, false});
    return list;
}

bool encoderWorks(const EncodeSpec& spec, std::string* error) {
    auto fail = [&](const char* text) {
        if (error) *error = text;
        return false;
    };
    EncodeSpec trial = spec;
    trial.bgra = false;
    auto encoder = VideoEncoder::open(trial, error);
    if (!encoder) return false;
    const size_t luma = size_t(spec.width) * size_t(spec.height);
    std::vector<uint8_t> black(luma + 2 * size_t((spec.width + 1) / 2) * size_t((spec.height + 1) / 2), 128);
    std::fill(black.begin(), black.begin() + long(luma), 16);
    for (int n = 0; n < 2; ++n) if (!encoder->encodeI420(black.data(), n, 1)) return fail("the encoder refused a frame");
    if (!encoder->finish()) return fail("the encoder failed");
    Packet packet;
    int packets = 0;
    while (encoder->take(packet)) ++packets;
    return packets == 2 || fail("the encoder gave no pictures");
}

}
