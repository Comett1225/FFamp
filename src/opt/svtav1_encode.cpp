#include "soft_video.h"
#ifdef ABSENT_SVTAV1
#include <algorithm>
#include <cstring>
#include <map>
#include <svt-av1/EbSvtAv1Enc.h>

namespace absent {

namespace {

struct SvtEncoder : SoftEncoder {
    EbComponentType* handle = nullptr;
    bool started = false;
    std::vector<uint8_t> record;
    std::map<int64_t, std::pair<int64_t, int64_t>> times;
    int64_t sent = 0;

    ~SvtEncoder() override {
        if (started) svt_av1_enc_deinit(handle);
        if (handle) svt_av1_enc_deinit_handle(handle);
    }
    const char* name() const override { return "svt-av1"; }
    uint32_t configType() const override { return fourcc("av1C"); }
    std::vector<uint8_t> config() const override { return record; }

    bool drain(bool all, const PacketSink& sink) {
        for (;;) {
            EbBufferHeaderType* out = nullptr;
            EbErrorType status = svt_av1_enc_get_packet(handle, &out, all ? 1 : 0);
            if (status == EB_NoErrorEmptyQueue) return true;
            if (status != EB_ErrorNone || !out) return false;
            const bool last = out->flags & EB_BUFFERFLAG_EOS;
            if (out->n_filled_len > 0) {
                Packet packet;
                const size_t delimiter = out->n_filled_len >= 2 && out->p_buffer[0] == 0x12 && out->p_buffer[1] == 0 ? 2 : 0;
                packet.data.assign(out->p_buffer + delimiter, out->p_buffer + out->n_filled_len);
                auto found = times.find(out->pts);
                if (found != times.end()) {
                    packet.pts = packet.dts = found->second.first;
                    packet.duration = uint32_t(found->second.second);
                    times.erase(found);
                }
                packet.sync = out->pic_type == EB_AV1_KEY_PICTURE;
                sink(std::move(packet));
            }
            svt_av1_enc_release_out_buffer(&out);
            if (last) return true;
        }
    }

    bool encode(const Picture& p, int64_t pts, int64_t duration, const PacketSink& sink) override {
        if (p.bits != 8 || p.interleaved) return false;
        EbSvtIOFormat planes = {};
        planes.luma = const_cast<uint8_t*>(p.plane[0]);
        planes.cb = const_cast<uint8_t*>(p.plane[1]);
        planes.cr = const_cast<uint8_t*>(p.plane[2]);
        planes.y_stride = uint32_t(p.stride[0]);
        planes.cb_stride = uint32_t(p.stride[1]);
        planes.cr_stride = uint32_t(p.stride[2]);
        EbBufferHeaderType in = {};
        in.size = sizeof in;
        in.p_buffer = reinterpret_cast<uint8_t*>(&planes);
        in.n_filled_len = uint32_t(p.width * p.height + 2 * p.chromaWidth * p.chromaHeight);
        in.pts = sent;
        in.pic_type = EB_AV1_INVALID_PICTURE;
        times[sent++] = {pts, duration};
        if (svt_av1_enc_send_picture(handle, &in) != EB_ErrorNone) return false;
        return drain(false, sink);
    }

    bool finish(const PacketSink& sink) override {
        EbBufferHeaderType end = {};
        end.size = sizeof end;
        end.flags = EB_BUFFERFLAG_EOS;
        end.pic_type = EB_AV1_INVALID_PICTURE;
        if (svt_av1_enc_send_picture(handle, &end) != EB_ErrorNone) return false;
        return drain(true, sink);
    }
};

void quiet(void*, SvtAv1LogLevel, const char*, const char*, va_list) {}

}

std::unique_ptr<SoftEncoder> openSvtAv1(const EncodeSpec& spec, std::string* error) {
    auto fail = [&](const char* text) {
        if (error) *error = text;
        return nullptr;
    };
    svt_av1_set_log_callback(quiet, nullptr);
    std::unique_ptr<SvtEncoder> encoder(new SvtEncoder);
    EbSvtAv1EncConfiguration settings;
    if (svt_av1_enc_init_handle(&encoder->handle, &settings) != EB_ErrorNone) return fail("the AV1 encoder did not start");
    settings.source_width = uint32_t(spec.width);
    settings.source_height = uint32_t(spec.height);
    settings.frame_rate_numerator = uint32_t(spec.fps * 1000 + 0.5);
    settings.frame_rate_denominator = 1000;
    settings.encoder_bit_depth = 8;
    settings.encoder_color_format = EB_YUV420;
    settings.enc_mode = int8_t(std::clamp(spec.speed < 0 ? 10 : spec.speed, 0, 13));
    settings.rate_control_mode = SVT_AV1_RC_MODE_VBR;
    settings.target_bit_rate = uint32_t(spec.bitrate);
    settings.intra_period_length = std::max(spec.keyInterval, 1) - 1;
    settings.color_primaries = EB_CICP_CP_BT_709;
    settings.transfer_characteristics = EB_CICP_TC_BT_709;
    settings.matrix_coefficients = EB_CICP_MC_BT_709;
    settings.color_range = EB_CR_STUDIO_RANGE;
    if (svt_av1_enc_set_parameter(encoder->handle, &settings) != EB_ErrorNone) return fail("the AV1 encoder refused these settings");
    if (svt_av1_enc_init(encoder->handle) != EB_ErrorNone) return fail("the AV1 encoder refused these settings");
    encoder->started = true;
    EbBufferHeaderType* header = nullptr;
    if (svt_av1_enc_stream_header(encoder->handle, &header) != EB_ErrorNone || !header) return fail("the AV1 encoder gave no stream header");
    StreamShape shape;
    bool described = av1Config(header->p_buffer, header->n_filled_len, encoder->record, shape);
    svt_av1_enc_stream_header_release(header);
    if (!described) return fail("the AV1 encoder gave no stream header");
    return encoder;
}

}
#else

namespace absent {

std::unique_ptr<SoftEncoder> openSvtAv1(const EncodeSpec&, std::string* error) {
    if (error) *error = "built without an AV1 encoder";
    return nullptr;
}

}
#endif
