#include "soft_video.h"
#ifdef ABSENT_DAV1D
#include <cerrno>
#include <cstring>
#include <dav1d/dav1d.h>

namespace absent {

namespace {

void quiet(void*, const char*, va_list) {}

struct Dav1dDecoder : SoftDecoder {
    Dav1dContext* context = nullptr;
    std::vector<uint8_t> grey;

    ~Dav1dDecoder() override {
        if (context) dav1d_close(&context);
    }
    const char* name() const override { return "dav1d"; }
    void reset() override { dav1d_flush(context); }

    void show(const Dav1dPicture& shown, const PictureSink& sink) {
        Picture p;
        p.stamp = shown.m.timestamp;
        p.width = shown.p.w;
        p.height = shown.p.h;
        p.bits = shown.p.bpc;
        const bool mono = shown.p.layout == DAV1D_PIXEL_LAYOUT_I400;
        const int subX = mono || shown.p.layout != DAV1D_PIXEL_LAYOUT_I444, subY = mono || shown.p.layout == DAV1D_PIXEL_LAYOUT_I420;
        p.chromaWidth = (p.width + subX) >> subX;
        p.chromaHeight = (p.height + subY) >> subY;
        p.plane[0] = static_cast<const uint8_t*>(shown.data[0]);
        p.stride[0] = size_t(shown.stride[0]);
        if (mono) {
            const size_t bytes = p.bits > 8 ? 2 : 1;
            p.stride[1] = p.stride[2] = size_t(p.chromaWidth) * bytes;
            if (grey.size() != p.stride[1] * size_t(p.chromaHeight)) {
                grey.resize(p.stride[1] * size_t(p.chromaHeight));
                const uint16_t middle = uint16_t(1 << (p.bits - 1));
                for (size_t i = 0; i < grey.size(); i += bytes) {
                    if (bytes == 1) grey[i] = 128;
                    else memcpy(&grey[i], &middle, 2);
                }
            }
            p.plane[1] = p.plane[2] = grey.data();
        } else {
            p.plane[1] = static_cast<const uint8_t*>(shown.data[1]);
            p.plane[2] = static_cast<const uint8_t*>(shown.data[2]);
            p.stride[1] = p.stride[2] = size_t(shown.stride[1]);
        }
        if (shown.seq_hdr) {
            p.color.known = shown.seq_hdr->color_description_present;
            p.color.primaries = shown.seq_hdr->pri;
            p.color.transfer = shown.seq_hdr->trc;
            p.color.matrix = shown.seq_hdr->mtrx;
            p.color.full = shown.seq_hdr->color_range;
        }
        sink(p);
    }

    bool drain(const PictureSink& sink) override {
        for (int empty = 0; empty < 2;) {
            Dav1dPicture shown;
            memset(&shown, 0, sizeof shown);
            const int got = dav1d_get_picture(context, &shown);
            if (got == DAV1D_ERR(EAGAIN)) {
                ++empty;
                continue;
            }
            if (got < 0) return false;
            empty = 1;
            show(shown, sink);
            dav1d_picture_unref(&shown);
        }
        return true;
    }

    bool decode(const uint8_t* bytes, size_t size, int64_t stamp, const PictureSink& sink) override {
        if (!size) return true;
        Dav1dData data;
        memset(&data, 0, sizeof data);
        uint8_t* room = dav1d_data_create(&data, size);
        if (!room) return false;
        memcpy(room, bytes, size);
        data.m.timestamp = stamp;
        bool ok = true;
        for (;;) {
            int sent = data.sz ? dav1d_send_data(context, &data) : 0;
            if (sent < 0 && sent != DAV1D_ERR(EAGAIN)) {
                ok = false;
                break;
            }
            Dav1dPicture shown;
            memset(&shown, 0, sizeof shown);
            int got = dav1d_get_picture(context, &shown);
            if (got == 0) {
                show(shown, sink);
                dav1d_picture_unref(&shown);
            } else if (got != DAV1D_ERR(EAGAIN)) {
                ok = false;
                break;
            }
            if (!data.sz) break;
        }
        if (data.sz) dav1d_data_unref(&data);
        return ok;
    }
};

}

std::unique_ptr<SoftDecoder> openDav1d(std::string* error) {
    Dav1dSettings settings;
    dav1d_default_settings(&settings);
    settings.frame_size_limit = 8192u * 8192u;
    settings.logger.callback = quiet;
    std::unique_ptr<Dav1dDecoder> decoder(new Dav1dDecoder);
    if (dav1d_open(&decoder->context, &settings) < 0) {
        if (error) *error = "the AV1 decoder did not start";
        return nullptr;
    }
    return decoder;
}

}
#else

namespace absent {

std::unique_ptr<SoftDecoder> openDav1d(std::string* error) {
    if (error) *error = "built without an AV1 software decoder";
    return nullptr;
}

}
#endif
