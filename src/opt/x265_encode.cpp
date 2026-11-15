#include "soft_video.h"
#ifdef ABSENT_X265
#include "nal.h"
#include <cmath>
#include <x265.h>

namespace absent {

namespace {

struct X265Encoder : SoftEncoder {
    x265_param* param = nullptr;
    x265_encoder* encoder = nullptr;
    x265_picture* picture = nullptr;
    std::vector<uint8_t> hvcc;

    ~X265Encoder() override {
        if (picture) x265_picture_free(picture);
        if (encoder) x265_encoder_close(encoder);
        if (param) x265_param_free(param);
    }
    const char* name() const override { return "x265"; }
    uint32_t configType() const override { return fourcc("hvcC"); }
    std::vector<uint8_t> config() const override { return hvcc; }

    void hand(x265_nal* nals, uint32_t count, const x265_picture& out, const PacketSink& sink) {
        if (!count) return;
        Packet p;
        for (uint32_t i = 0; i < count; ++i) p.data.insert(p.data.end(), nals[i].payload, nals[i].payload + nals[i].sizeBytes);
        p.pts = out.pts;
        p.dts = out.dts;
        p.sync = IS_X265_TYPE_I(out.sliceType);
        sink(std::move(p));
    }

    bool encode(const Picture& in, int64_t pts, int64_t, const PacketSink& sink) override {
        for (int i = 0; i < 3; ++i) {
            picture->planes[i] = const_cast<uint8_t*>(in.plane[i]);
            picture->stride[i] = int(in.stride[i]);
        }
        picture->pts = pts;
        x265_nal* nals = nullptr;
        uint32_t count = 0;
        x265_picture out;
        x265_picture_init(param, &out);
        if (x265_encoder_encode(encoder, &nals, &count, picture, &out) < 0) return false;
        hand(nals, count, out, sink);
        return true;
    }

    bool finish(const PacketSink& sink) override {
        for (;;) {
            x265_nal* nals = nullptr;
            uint32_t count = 0;
            x265_picture out;
            x265_picture_init(param, &out);
            const int made = x265_encoder_encode(encoder, &nals, &count, nullptr, &out);
            if (made < 0) return false;
            if (made == 0) return true;
            hand(nals, count, out, sink);
        }
    }
};

}

std::unique_ptr<SoftEncoder> openX265(const EncodeSpec& spec, std::string* error) {
    auto fail = [&](const char* text) {
        if (error) *error = text;
        return nullptr;
    };
    if (spec.width % 2 || spec.height % 2) return fail("the HEVC software encoder needs an even width and height");
    auto encoder = std::make_unique<X265Encoder>();
    x265_param* p = encoder->param = x265_param_alloc();
    if (!p || x265_param_default_preset(p, "fast", nullptr) < 0) return fail("the HEVC software encoder did not start");
    p->logLevel = X265_LOG_NONE;
    p->sourceWidth = spec.width;
    p->sourceHeight = spec.height;
    p->internalCsp = X265_CSP_I420;
    p->internalBitDepth = 8;
    const double fps = spec.fps > 0 ? spec.fps : 30;
    p->fpsNum = uint32_t(std::lround(fps * 1000));
    p->fpsDenom = 1000;
    p->rc.rateControlMode = X265_RC_ABR;
    p->rc.bitrate = std::max(1, spec.bitrate / 1000);
    p->keyframeMax = std::max(1, spec.keyInterval);
    p->keyframeMin = std::min(p->keyframeMin, p->keyframeMax);
    if (!spec.reorder) p->bframes = 0;
    p->bRepeatHeaders = 0;
    p->bOpenGOP = 0;
    p->bAnnexB = 0;
    p->bEmitInfoSEI = 0;
    p->vui.bEnableVideoSignalTypePresentFlag = 1;
    p->vui.bEnableColorDescriptionPresentFlag = 1;
    p->vui.colorPrimaries = 1;
    p->vui.transferCharacteristics = 1;
    p->vui.matrixCoeffs = 1;
    p->vui.videoFormat = 5;
    if (x265_param_apply_profile(p, "main") < 0) return fail("the HEVC software encoder refused these settings");
    encoder->encoder = x265_encoder_open(p);
    if (!encoder->encoder) return fail("the HEVC software encoder refused these settings");
    encoder->picture = x265_picture_alloc();
    x265_picture_init(p, encoder->picture);
    encoder->picture->colorSpace = X265_CSP_I420;
    encoder->picture->bitDepth = 8;
    x265_nal* nals = nullptr;
    uint32_t count = 0;
    if (x265_encoder_headers(encoder->encoder, &nals, &count) < 0) return fail("the HEVC software encoder gave no headers");
    NalList vps, sps, pps;
    for (uint32_t i = 0; i < count; ++i) {
        if (nals[i].sizeBytes <= 4) continue;
        std::vector<uint8_t> unit(nals[i].payload + 4, nals[i].payload + nals[i].sizeBytes);
        if (nals[i].type == NAL_UNIT_VPS) vps.push_back(unit);
        else if (nals[i].type == NAL_UNIT_SPS) sps.push_back(unit);
        else if (nals[i].type == NAL_UNIT_PPS) pps.push_back(unit);
    }
    encoder->hvcc = buildHevcConfig(vps, sps, pps);
    if (encoder->hvcc.empty()) return fail("the HEVC software encoder gave no headers");
    return encoder;
}

}
#else

namespace absent {

std::unique_ptr<SoftEncoder> openX265(const EncodeSpec&, std::string* error) {
    if (error) *error = "built without an HEVC software encoder";
    return nullptr;
}

}
#endif
