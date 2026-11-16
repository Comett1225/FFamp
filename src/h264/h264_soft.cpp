#include "soft_video.h"
#include "h264/h264_ffamp.h"
#include <cstdlib>

namespace absent {

namespace {

struct OwnH264Decoder : SoftDecoder {
    FFampH264* decoder = nullptr;
    const PictureSink* shown = nullptr;

    ~OwnH264Decoder() override { ffamp_h264_close(decoder); }
    const char* name() const override { return "h264"; }
    bool drainKeepsState() const override { return false; }
    bool weavesFields() const override { return true; }

    static void hand(void* opaque, const FFampH264Frame* f) {
        auto* self = static_cast<OwnH264Decoder*>(opaque);
        if (!self->shown || !*self->shown) return;
        Picture p;
        for (int i = 0; i < 3; ++i) {
            p.plane[i] = f->data[i];
            p.stride[i] = size_t(f->linesize[i]);
        }
        p.width = f->width;
        p.height = f->height;
        p.chromaWidth = -((-f->width) >> f->chroma_shift_x);
        p.chromaHeight = -((-f->height) >> f->chroma_shift_y);
        p.bits = f->bits;
        p.stamp = f->pts;
        p.color.known = f->matrix != 2 || f->primaries != 2 || f->transfer != 2;
        p.color.primaries = f->primaries;
        p.color.transfer = f->transfer;
        p.color.matrix = f->matrix;
        p.color.full = f->full_range != 0;
        if (void* kept = ffamp_h264_keep(f)) p.keep = std::shared_ptr<const void>(kept, ffamp_h264_release);
        (*self->shown)(p);
    }

    bool decode(const uint8_t* data, size_t size, int64_t stamp, const PictureSink& sink) override {
        if (!size || size > size_t(INT32_MAX)) return false;
        shown = &sink;
        const int result = ffamp_h264_decode(decoder, data, int(size), stamp, hand, this);
        shown = nullptr;
        return result >= 0;
    }

    bool drain(const PictureSink& sink) override {
        shown = &sink;
        ffamp_h264_drain(decoder, hand, this);
        shown = nullptr;
        return true;
    }

    void reset() override { ffamp_h264_flush(decoder); }
};

}

std::unique_ptr<SoftDecoder> openH264(const std::vector<uint8_t>& config, std::string* error) {
    if (config.size() > size_t(INT32_MAX)) {
        if (error) *error = "the H.264 configuration is too large";
        return nullptr;
    }
    const char* wanted = std::getenv("FFAMP_H264_THREADS");
    const int threads = wanted ? std::atoi(wanted) : 0;
    const bool slices = std::getenv("FFAMP_H264_SLICES") != nullptr;
    auto decoder = std::make_unique<OwnH264Decoder>();
    decoder->decoder = ffamp_h264_open(config.data(), int(config.size()), threads, slices ? FFAMP_H264_SLICE_THREADS : FFAMP_H264_FRAME_THREADS);
    if (!decoder->decoder) {
        if (error) *error = "the H.264 decoder did not start";
        return nullptr;
    }
    return decoder;
}

}
