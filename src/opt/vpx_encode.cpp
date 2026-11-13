#include "soft_video.h"
#ifdef ABSENT_VPX
#include <algorithm>
#include <thread>
#include <vpx/vp8cx.h>
#include <vpx/vpx_encoder.h>

namespace absent {

namespace {

struct VpxEncoder : SoftEncoder {
    vpx_codec_ctx_t context;
    bool started = false;
    unsigned long deadline = VPX_DL_REALTIME;
    std::vector<uint8_t> record;

    ~VpxEncoder() override {
        if (started) vpx_codec_destroy(&context);
    }
    const char* name() const override { return "libvpx"; }
    uint32_t configType() const override { return fourcc("vpcC"); }
    std::vector<uint8_t> config() const override { return record; }

    void drain(const PacketSink& sink) {
        vpx_codec_iter_t walk = nullptr;
        while (const vpx_codec_cx_pkt_t* got = vpx_codec_get_cx_data(&context, &walk)) {
            if (got->kind != VPX_CODEC_CX_FRAME_PKT) continue;
            Packet packet;
            const uint8_t* bytes = static_cast<const uint8_t*>(got->data.frame.buf);
            packet.data.assign(bytes, bytes + got->data.frame.sz);
            packet.pts = packet.dts = got->data.frame.pts;
            packet.duration = uint32_t(got->data.frame.duration);
            packet.sync = got->data.frame.flags & VPX_FRAME_IS_KEY;
            sink(std::move(packet));
        }
    }

    bool encode(const Picture& p, int64_t pts, int64_t duration, const PacketSink& sink) override {
        if (p.bits != 8 || p.interleaved) return false;
        vpx_image_t image;
        if (!vpx_img_wrap(&image, VPX_IMG_FMT_I420, unsigned(p.width), unsigned(p.height), 1, const_cast<uint8_t*>(p.plane[0]))) return false;
        for (int i = 0; i < 3; ++i) {
            image.planes[i] = const_cast<uint8_t*>(p.plane[i]);
            image.stride[i] = int(p.stride[i]);
        }
        if (vpx_codec_encode(&context, &image, pts, static_cast<unsigned long>(std::max<int64_t>(duration, 1)), 0, deadline) != VPX_CODEC_OK) return false;
        drain(sink);
        return true;
    }

    bool finish(const PacketSink& sink) override {
        for (;;) {
            if (vpx_codec_encode(&context, nullptr, 0, 0, 0, deadline) != VPX_CODEC_OK) return false;
            bool any = false;
            drain([&](Packet&& packet) {
                any = true;
                sink(std::move(packet));
            });
            if (!any) return true;
        }
    }
};

}

std::unique_ptr<SoftEncoder> openVpxEncoder(const EncodeSpec& spec, std::string* error) {
    auto fail = [&](const char* text) {
        if (error) *error = text;
        return nullptr;
    };
    vpx_codec_enc_cfg_t settings;
    if (vpx_codec_enc_config_default(vpx_codec_vp9_cx(), &settings, 0) != VPX_CODEC_OK) return fail("the VP9 encoder did not start");
    const int speed = spec.speed < 0 ? 8 : spec.speed;
    const bool realtime = speed > 5;
    settings.g_w = unsigned(spec.width);
    settings.g_h = unsigned(spec.height);
    settings.g_timebase.num = 1;
    settings.g_timebase.den = int(spec.timescale);
    settings.g_threads = std::clamp(std::thread::hardware_concurrency(), 1u, 16u);
    settings.g_lag_in_frames = realtime ? 0 : 25;
    settings.rc_end_usage = VPX_VBR;
    settings.rc_target_bitrate = unsigned(std::max(spec.bitrate / 1000, 1));
    settings.kf_min_dist = 0;
    settings.kf_max_dist = unsigned(std::max(spec.keyInterval, 1));
    std::unique_ptr<VpxEncoder> encoder(new VpxEncoder);
    if (vpx_codec_enc_init(&encoder->context, vpx_codec_vp9_cx(), &settings, 0) != VPX_CODEC_OK) return fail("the VP9 encoder refused these settings");
    encoder->started = true;
    encoder->deadline = realtime ? VPX_DL_REALTIME : VPX_DL_GOOD_QUALITY;
    int columns = 0;
    while (columns < 6 && (spec.width >> (columns + 1)) >= 256 && (1u << (columns + 1)) <= settings.g_threads) ++columns;
    vpx_codec_control(&encoder->context, VP8E_SET_CPUUSED, std::clamp(speed, 0, 9));
    vpx_codec_control(&encoder->context, VP9E_SET_ROW_MT, 1);
    vpx_codec_control(&encoder->context, VP9E_SET_TILE_COLUMNS, columns);
    vpx_codec_control(&encoder->context, VP9E_SET_COLOR_SPACE, VPX_CS_BT_709);
    vpx_codec_control(&encoder->context, VP9E_SET_COLOR_RANGE, VPX_CR_STUDIO_RANGE);
    StreamShape shape;
    shape.width = spec.width;
    shape.height = spec.height;
    shape.color.primaries = shape.color.transfer = shape.color.matrix = 1;
    encoder->record = vp9Config(shape, spec.fps, spec.bitrate);
    return encoder;
}

}
#else

namespace absent {

std::unique_ptr<SoftEncoder> openVpxEncoder(const EncodeSpec&, std::string* error) {
    if (error) *error = "built without a VP9 encoder";
    return nullptr;
}

}
#endif
