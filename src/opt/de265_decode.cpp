#include "soft_video.h"
#ifdef ABSENT_DE265
#include "nal.h"
#include <algorithm>
#include <cstring>
#include <libde265/de265.h>
#include <thread>

namespace absent {

namespace {

struct De265Decoder : SoftDecoder {
    de265_decoder_context* context = nullptr;
    std::vector<std::vector<uint8_t>> headers;
    int lengthSize = 4;
    bool primed = false;
    Color color;
    std::vector<uint8_t> grey;
    int greyBits = 0;

    ~De265Decoder() override {
        if (context) de265_free_decoder(context);
    }
    const char* name() const override { return "libde265"; }

    bool start() {
        context = de265_new_decoder();
        if (!context) return false;
        de265_set_parameter_bool(context, DE265_DECODER_PARAM_SUPPRESS_FAULTY_PICTURES, 1);
        de265_set_limit_TID(context, 100);
        de265_start_worker_threads(context, int(std::clamp(std::thread::hardware_concurrency(), 1u, 16u)));
        primed = false;
        return true;
    }

    void reset() override {
        if (context) de265_reset(context);
        primed = false;
    }

    void pictures(const PictureSink& sink) {
        while (const de265_image* image = de265_get_next_picture(context)) {
            const bool mono = de265_get_chroma_format(image) == de265_chroma_mono;
            Picture p;
            p.width = de265_get_image_width(image, 0);
            p.height = de265_get_image_height(image, 0);
            p.bits = de265_get_bits_per_pixel(image, 0);
            for (int c = 0; c < (mono ? 1 : 3); ++c) {
                int stride = 0;
                p.plane[c] = de265_get_image_plane(image, c, &stride);
                p.stride[c] = size_t(stride);
            }
            if (mono) {
                p.chromaWidth = (p.width + 1) / 2;
                p.chromaHeight = (p.height + 1) / 2;
                const size_t bytes = p.bits > 8 ? 2 : 1;
                p.stride[1] = p.stride[2] = size_t(p.chromaWidth) * bytes;
                if (grey.size() != p.stride[1] * size_t(p.chromaHeight) || greyBits != p.bits) {
                    greyBits = p.bits;
                    grey.resize(p.stride[1] * size_t(p.chromaHeight));
                    const uint16_t middle = uint16_t(1 << (p.bits - 1));
                    for (size_t i = 0; i < grey.size(); i += bytes) {
                        if (bytes == 1) grey[i] = 128;
                        else memcpy(&grey[i], &middle, 2);
                    }
                }
                p.plane[1] = p.plane[2] = grey.data();
            } else {
                p.chromaWidth = de265_get_image_width(image, 1);
                p.chromaHeight = de265_get_image_height(image, 1);
            }
            p.stamp = de265_get_image_PTS(image);
            p.color = color;
            if (p.plane[0] && p.plane[1] && p.plane[2]) sink(p);
        }
    }

    void run(const PictureSink& sink) {
        for (int more = 1; more;) {
            more = 0;
            const de265_error error = de265_decode(context, &more);
            pictures(sink);
            if (error == DE265_ERROR_WAITING_FOR_INPUT_DATA) break;
            if (!de265_isOK(error)) break;
        }
    }

    bool decode(const uint8_t* data, size_t size, int64_t stamp, const PictureSink& sink) override {
        if (!context && !start()) return false;
        if (!primed) {
            for (const auto& unit : headers) de265_push_NAL(context, unit.data(), int(unit.size()), stamp, nullptr);
            primed = true;
        }
        for (size_t at = 0; at + size_t(lengthSize) <= size;) {
            size_t length = 0;
            for (int k = 0; k < lengthSize; ++k) length = (length << 8) | data[at + size_t(k)];
            at += size_t(lengthSize);
            if (length > size - at) return false;
            if (de265_push_NAL(context, data + at, int(length), stamp, nullptr) != DE265_OK) return false;
            at += length;
        }
        de265_push_end_of_frame(context);
        run(sink);
        return true;
    }

    bool drain(const PictureSink& sink) override {
        if (!context) return true;
        de265_flush_data(context);
        run(sink);
        pictures(sink);
        return true;
    }
};

}

std::unique_ptr<SoftDecoder> openDe265(const std::vector<uint8_t>& config, std::string* error) {
    auto fail = [&](const char* text) {
        if (error) *error = text;
        return nullptr;
    };
    const VideoInfo info = videoInfo(fourcc("hvcC"), config);
    if (!info.valid || config.size() < 23) return fail("the HEVC configuration is not readable");
    auto decoder = std::make_unique<De265Decoder>();
    decoder->lengthSize = (config[21] & 3) + 1;
    if (info.colorKnown || info.rangeKnown) {
        decoder->color.known = info.colorKnown;
        decoder->color.primaries = info.primaries;
        decoder->color.transfer = info.transfer;
        decoder->color.matrix = info.matrix;
        decoder->color.full = info.full;
    }
    size_t at = 23;
    for (int n = 0; n < config[22] && at + 3 <= config.size(); ++n) {
        const size_t count = (size_t(config[at + 1]) << 8) | config[at + 2];
        at += 3;
        for (size_t i = 0; i < count && at + 2 <= config.size(); ++i) {
            const size_t length = (size_t(config[at]) << 8) | config[at + 1];
            at += 2;
            if (length > config.size() - at) break;
            decoder->headers.emplace_back(config.begin() + long(at), config.begin() + long(at + length));
            at += length;
        }
    }
    if (!decoder->start()) return fail("the HEVC software decoder did not start");
    return decoder;
}

}
#else

namespace absent {

std::unique_ptr<SoftDecoder> openDe265(const std::vector<uint8_t>&, std::string* error) {
    if (error) *error = "built without an HEVC software decoder";
    return nullptr;
}

}
#endif
