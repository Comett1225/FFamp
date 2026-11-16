#include "soft_video.h"
#ifdef ABSENT_MPEG2
#include <algorithm>
#include <cstdint>
extern "C" {
#include <mpeg2dec/mpeg2.h>
}

namespace absent {

namespace {

struct Mpeg2Decoder : SoftDecoder {
    mpeg2dec_t* handle = nullptr;
    int references = 0;
    bool closed = false;
    const mpeg2_fbuf_t* reference = nullptr;

    ~Mpeg2Decoder() override {
        if (handle) mpeg2_close(handle);
    }
    const char* name() const override { return "libmpeg2"; }
    bool drainKeepsState() const override { return false; }

    void reset() override {
        mpeg2_reset(handle, 0);
        references = 0;
        reference = nullptr;
    }

    void show(const mpeg2_info_t* info, const PictureSink& sink) {
        const mpeg2_sequence_t* s = info->sequence;
        const mpeg2_picture_t* picture = info->display_picture;
        if (!references || !s || !picture || !info->display_fbuf || !(picture->flags & PIC_FLAG_TAGS)) return;
        const bool orphan = (picture->flags & PIC_MASK_CODING_TYPE) == PIC_FLAG_CODING_TYPE_B && references < 2 && !closed;
        const mpeg2_fbuf_t* shown = orphan ? reference : info->display_fbuf;
        if (!shown) return;
        Picture p;
        p.width = int(std::min(s->picture_width, s->width));
        p.height = int(std::min(s->picture_height, s->height));
        p.chromaWidth = s->chroma_width == s->width ? p.width : (p.width + 1) / 2;
        p.chromaHeight = s->chroma_height == s->height ? p.height : (p.height + 1) / 2;
        for (int c = 0; c < 3; ++c) {
            p.plane[c] = shown->buf[c];
            p.stride[c] = c ? s->chroma_width : s->width;
        }
        p.stamp = int64_t(uint64_t(picture->tag) | uint64_t(picture->tag2) << 32);
        auto told = [](int v) { return v != 0 && v != 2; };
        if ((s->flags & SEQ_FLAG_COLOUR_DESCRIPTION) && (told(s->colour_primaries) || told(s->transfer_characteristics) || told(s->matrix_coefficients))) {
            p.color.known = true;
            p.color.primaries = s->colour_primaries;
            p.color.transfer = s->transfer_characteristics;
            p.color.matrix = s->matrix_coefficients;
        }
        if (p.width > 0 && p.height > 0 && p.plane[0] && p.plane[1] && p.plane[2]) sink(p);
    }

    void run(const PictureSink& sink) {
        for (;;) {
            const mpeg2_state_t state = mpeg2_parse(handle);
            const mpeg2_info_t* info = mpeg2_info(handle);
            switch (state) {
            case STATE_BUFFER:
                return;
            case STATE_SEQUENCE:
                references = 0;
                reference = nullptr;
                break;
            case STATE_GOP:
                closed = info->gop && (info->gop->flags & GOP_FLAG_CLOSED_GOP) && !(info->gop->flags & GOP_FLAG_BROKEN_LINK);
                break;
            case STATE_PICTURE:
                if (info->current_picture && (info->current_picture->flags & PIC_MASK_CODING_TYPE) != PIC_FLAG_CODING_TYPE_B) {
                    ++references;
                    reference = info->current_fbuf;
                }
                break;
            case STATE_SLICE:
            case STATE_END:
            case STATE_INVALID_END:
                show(info, sink);
                break;
            default:
                break;
            }
        }
    }

    bool decode(const uint8_t* data, size_t size, int64_t stamp, const PictureSink& sink) override {
        if (!size) return true;
        mpeg2_tag_picture(handle, uint32_t(uint64_t(stamp)), uint32_t(uint64_t(stamp) >> 32));
        uint8_t* start = const_cast<uint8_t*>(data);
        mpeg2_buffer(handle, start, start + size);
        run(sink);
        return true;
    }

    bool drain(const PictureSink& sink) override {
        static uint8_t end[] = {0, 0, 1, 0xB7};
        mpeg2_buffer(handle, end, end + sizeof(end));
        run(sink);
        return true;
    }
};

}

std::unique_ptr<SoftDecoder> openMpeg2(const std::vector<uint8_t>& config, std::string* error) {
    auto decoder = std::make_unique<Mpeg2Decoder>();
    decoder->handle = mpeg2_init();
    if (!decoder->handle) {
        if (error) *error = "the MPEG-1/2 software decoder did not start";
        return nullptr;
    }
    static const uint8_t sequence[] = {0, 0, 1, 0xB3};
    auto found = std::search(config.begin(), config.end(), std::begin(sequence), std::end(sequence));
    if (found != config.end()) {
        std::vector<uint8_t> header(found, config.end());
        mpeg2_buffer(decoder->handle, header.data(), header.data() + header.size());
        decoder->run([](const Picture&) {});
    }
    return decoder;
}

}
#else

namespace absent {

std::unique_ptr<SoftDecoder> openMpeg2(const std::vector<uint8_t>&, std::string* error) {
    if (error) *error = "MPEG-1 and MPEG-2 video are not decoded on this system yet";
    return nullptr;
}

}
#endif
