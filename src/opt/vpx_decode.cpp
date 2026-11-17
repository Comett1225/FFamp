#include "soft_video.h"
#ifdef ABSENT_VPX
#include <algorithm>
#include <thread>
#include <vpx/vp8dx.h>
#include <vpx/vpx_decoder.h>

namespace absent {

namespace {

struct VpxDecoder : SoftDecoder {
    vpx_codec_ctx_t context;
    bool started = false;
    bool vp8 = false;

    ~VpxDecoder() override {
        if (started) vpx_codec_destroy(&context);
    }
    const char* name() const override { return "libvpx"; }
    void reset() override {}

    bool decode(const uint8_t* data, size_t size, int64_t stamp, const PictureSink& sink) override {
        if (size > 0xFFFFFFFFu || vpx_codec_decode(&context, data, static_cast<unsigned int>(size), nullptr, 0) != VPX_CODEC_OK) return false;
        vpx_codec_iter_t walk = nullptr;
        while (vpx_image_t* image = vpx_codec_get_frame(&context, &walk)) {
            const int kind = image->fmt & ~VPX_IMG_FMT_HIGHBITDEPTH;
            if (kind != VPX_IMG_FMT_I420 && kind != VPX_IMG_FMT_I422 && kind != VPX_IMG_FMT_I440 && kind != VPX_IMG_FMT_I444) return false;
            Picture p;
            p.stamp = stamp;
            p.width = int(image->d_w);
            p.height = int(image->d_h);
            p.bits = int(image->bit_depth);
            p.chromaWidth = int((image->d_w + image->x_chroma_shift) >> image->x_chroma_shift);
            p.chromaHeight = int((image->d_h + image->y_chroma_shift) >> image->y_chroma_shift);
            for (int i = 0; i < 3; ++i) {
                p.plane[i] = image->planes[i];
                p.stride[i] = size_t(image->stride[i]);
            }
            static const int matrices[8] = {2, 5, 1, 6, 7, 9, 2, 0};
            p.color.matrix = vp8 ? 6 : matrices[image->cs & 7];
            p.color.primaries = p.color.matrix == 9 ? 9 : p.color.matrix == 1 ? 1 : 2;
            p.color.transfer = p.color.matrix == 1 ? 1 : 2;
            p.color.known = !vp8 && image->cs != VPX_CS_UNKNOWN;
            p.color.full = image->range == VPX_CR_FULL_RANGE;
            sink(p);
        }
        return true;
    }
};

}

std::unique_ptr<SoftDecoder> openVpxDecoder(bool vp8, std::string* error) {
    std::unique_ptr<VpxDecoder> decoder(new VpxDecoder);
    decoder->vp8 = vp8;
    vpx_codec_dec_cfg_t settings = {};
    settings.threads = std::clamp(std::thread::hardware_concurrency(), 1u, 16u);
    if (vpx_codec_dec_init(&decoder->context, vp8 ? vpx_codec_vp8_dx() : vpx_codec_vp9_dx(), &settings, 0) != VPX_CODEC_OK) {
        if (error) *error = "the VP8/VP9 decoder did not start";
        return nullptr;
    }
    decoder->started = true;
    if (!vp8) vpx_codec_control(&decoder->context, VP9D_SET_LOOP_FILTER_OPT, 1);
    return decoder;
}

}
#else

namespace absent {

std::unique_ptr<SoftDecoder> openVpxDecoder(bool, std::string* error) {
    if (error) *error = "built without a VP8/VP9 software decoder";
    return nullptr;
}

}
#endif
