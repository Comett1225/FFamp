#include "soft_video.h"
#ifdef ABSENT_XVID
#include "aac_config.h"
#include <algorithm>
#include <cstring>
#include <mutex>
#include <xvid.h>

namespace absent {

namespace {

const uint8_t kPackedMark[] = {0, 0, 1, 0xB2, 'D', 'i', 'v', 'X', '5', '0', '3', 'b', '1', '3', '9', '3', 'p', 0};
const size_t kPadding = 64;

bool started() {
    static bool ready = false;
    static std::once_flag once;
    std::call_once(once, [] {
        xvid_gbl_init_t init;
        memset(&init, 0, sizeof init);
        init.version = XVID_VERSION;
        ready = xvid_global(nullptr, XVID_GBL_INIT, &init, nullptr) >= 0;
    });
    return ready;
}

struct XvidDecoder : SoftDecoder {
    void* handle = nullptr;
    int width = 0;
    int height = 0;
    std::vector<uint8_t> headers;
    std::vector<uint8_t> buffer;

    ~XvidDecoder() override { reset(); }
    const char* name() const override { return "xvid"; }
    void reset() override {
        if (handle) xvid_decore(handle, XVID_DEC_DESTROY, nullptr, nullptr);
        handle = nullptr;
    }

    bool open() {
        xvid_dec_create_t create;
        memset(&create, 0, sizeof create);
        create.version = XVID_VERSION;
        create.width = width;
        create.height = height;
        if (xvid_decore(nullptr, XVID_DEC_CREATE, &create, nullptr) < 0 || !create.handle) return false;
        handle = create.handle;
        return feed(headers.data(), headers.size(), 0, nullptr, true);
    }

    bool feed(const uint8_t* data, size_t size, int64_t stamp, const PictureSink* sink, bool mark = false) {
        if (size > 0x7FFFFFF0u) return false;
        buffer.assign(data, data + size);
        for (size_t i = 0; i + 8 <= size && memcmp(&buffer[i], "\0\0\x01\xB6", 4) != 0; ++i) {
            if (memcmp(&buffer[i], "\0\0\x01\xB2" "DivX", 8) == 0) buffer[i + 4] = 'd';
        }
        if (mark) buffer.insert(buffer.end(), kPackedMark, kPackedMark + sizeof kPackedMark);
        size = buffer.size();
        buffer.resize(size + kPadding, 0);
        for (size_t at = 0; size - at >= 4;) {
            xvid_dec_frame_t frame;
            memset(&frame, 0, sizeof frame);
            frame.version = XVID_VERSION;
            frame.general = XVID_LOWDELAY;
            frame.bitstream = buffer.data() + at;
            frame.length = int(size - at);
            frame.output.csp = XVID_CSP_INTERNAL;
            frame.output.plane[0] = buffer.data();
            frame.output.stride[0] = width;
            xvid_dec_stats_t stats;
            memset(&stats, 0, sizeof stats);
            stats.version = XVID_VERSION;
            const int used = xvid_decore(handle, XVID_DEC_DECODE, &frame, &stats);
            if (used < 0) return false;
            if (used == 0) break;
            at += std::min(size - at, size_t(used));
            if (stats.type <= 0 || !sink || frame.output.plane[0] == buffer.data()) continue;
            Picture p;
            p.stamp = stamp;
            p.width = width;
            p.height = height;
            p.chromaWidth = (width + 1) / 2;
            p.chromaHeight = (height + 1) / 2;
            for (int i = 0; i < 3; ++i) {
                p.plane[i] = static_cast<const uint8_t*>(frame.output.plane[i]);
                p.stride[i] = size_t(frame.output.stride[i]);
            }
            (*sink)(p);
        }
        return true;
    }

    bool decode(const uint8_t* data, size_t size, int64_t stamp, const PictureSink& sink) override {
        if (!handle && !open()) return false;
        return feed(data, size, stamp, &sink);
    }
};

}

std::unique_ptr<SoftDecoder> openXvid(const std::vector<uint8_t>& esds, int width, int height) {
    if (width <= 0 || height <= 0 || width > 8191 || height > 8191 || !started()) return nullptr;
    std::unique_ptr<XvidDecoder> decoder(new XvidDecoder);
    decoder->width = width;
    decoder->height = height;
    ElementaryStream stream;
    if (parseElementaryStream(View{esds.data(), esds.size()}, stream) && stream.specific.p) decoder->headers.assign(stream.specific.p, stream.specific.p + stream.specific.n);
    if (!decoder->open()) return nullptr;
    return decoder;
}

}
#else

namespace absent {

std::unique_ptr<SoftDecoder> openXvid(const std::vector<uint8_t>&, int, int) { return nullptr; }

}
#endif
