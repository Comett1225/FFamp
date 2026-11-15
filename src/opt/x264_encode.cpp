#include "soft_video.h"
#ifdef ABSENT_X264
#include "nal.h"
#include <cmath>
#include <cstdint>
#include <x264.h>

namespace absent {

namespace {

struct X264Encoder : SoftEncoder {
    x264_t* encoder = nullptr;
    std::vector<uint8_t> avcc;
    Packet packet;

    ~X264Encoder() override {
        if (encoder) x264_encoder_close(encoder);
    }
    const char* name() const override { return "x264"; }
    uint32_t configType() const override { return fourcc("avcC"); }
    std::vector<uint8_t> config() const override { return avcc; }

    void hand(x264_nal_t* nals, int count, int size, const x264_picture_t& out, const PacketSink& sink) {
        if (size <= 0 || count <= 0) return;
        Packet p;
        p.data.assign(nals[0].p_payload, nals[0].p_payload + size);
        p.pts = out.i_pts;
        p.dts = out.i_dts;
        p.sync = out.b_keyframe;
        sink(std::move(p));
    }

    bool encode(const Picture& picture, int64_t pts, int64_t, const PacketSink& sink) override {
        x264_picture_t in, out;
        x264_picture_init(&in);
        in.img.i_csp = X264_CSP_I420;
        in.img.i_plane = 3;
        for (int i = 0; i < 3; ++i) {
            in.img.plane[i] = const_cast<uint8_t*>(picture.plane[i]);
            in.img.i_stride[i] = int(picture.stride[i]);
        }
        in.i_pts = pts;
        x264_nal_t* nals = nullptr;
        int count = 0;
        const int size = x264_encoder_encode(encoder, &nals, &count, &in, &out);
        if (size < 0) return false;
        hand(nals, count, size, out, sink);
        return true;
    }

    bool finish(const PacketSink& sink) override {
        while (x264_encoder_delayed_frames(encoder) > 0) {
            x264_picture_t out;
            x264_nal_t* nals = nullptr;
            int count = 0;
            const int size = x264_encoder_encode(encoder, &nals, &count, nullptr, &out);
            if (size < 0) return false;
            hand(nals, count, size, out, sink);
        }
        return true;
    }
};

}

std::unique_ptr<SoftEncoder> openX264(const EncodeSpec& spec, std::string* error) {
    auto fail = [&](const char* text) {
        if (error) *error = text;
        return nullptr;
    };
    if (spec.width % 2 || spec.height % 2) return fail("the H.264 software encoder needs an even width and height");
    x264_param_t p;
    if (x264_param_default_preset(&p, "veryfast", nullptr) < 0) return fail("the H.264 software encoder did not start");
    p.i_log_level = X264_LOG_NONE;
    p.i_width = spec.width;
    p.i_height = spec.height;
    p.i_csp = X264_CSP_I420;
    const double fps = spec.fps > 0 ? spec.fps : 30;
    p.i_fps_num = uint32_t(std::lround(fps * 1000));
    p.i_fps_den = 1000;
    p.i_timebase_num = 1;
    p.i_timebase_den = spec.timescale;
    p.b_vfr_input = 1;
    p.rc.i_rc_method = X264_RC_ABR;
    p.rc.i_bitrate = std::max(1, spec.bitrate / 1000);
    p.i_keyint_max = std::max(1, spec.keyInterval);
    p.i_keyint_min = std::min(p.i_keyint_min, p.i_keyint_max);
    if (!spec.reorder) p.i_bframe = 0;
    p.b_repeat_headers = 0;
    p.b_annexb = 0;
    p.vui.i_colorprim = 1;
    p.vui.i_transfer = 1;
    p.vui.i_colmatrix = 1;
    p.vui.b_fullrange = 0;
    if (x264_param_apply_profile(&p, "high") < 0) return fail("the H.264 software encoder refused these settings");
    auto encoder = std::make_unique<X264Encoder>();
    encoder->encoder = x264_encoder_open(&p);
    if (!encoder->encoder) return fail("the H.264 software encoder refused these settings");
    x264_nal_t* nals = nullptr;
    int count = 0;
    if (x264_encoder_headers(encoder->encoder, &nals, &count) < 0) return fail("the H.264 software encoder gave no headers");
    NalList sps, pps;
    for (int i = 0; i < count; ++i) {
        if (nals[i].i_payload <= 4) continue;
        std::vector<uint8_t> unit(nals[i].p_payload + 4, nals[i].p_payload + nals[i].i_payload);
        if (nals[i].i_type == NAL_SPS) sps.push_back(unit);
        else if (nals[i].i_type == NAL_PPS) pps.push_back(unit);
    }
    encoder->avcc = buildAvcConfig(sps, pps);
    if (encoder->avcc.empty()) return fail("the H.264 software encoder gave no headers");
    return encoder;
}

}
#else

namespace absent {

std::unique_ptr<SoftEncoder> openX264(const EncodeSpec&, std::string* error) {
    if (error) *error = "built without an H.264 software encoder";
    return nullptr;
}

}
#endif
