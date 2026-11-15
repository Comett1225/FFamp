#include "soft_video.h"
#ifdef ABSENT_OPENH264
#include "nal.h"
#include <cstring>
#include <wels/codec_api.h>

namespace absent {

namespace {

struct OpenH264Decoder : SoftDecoder {
    ISVCDecoder* decoder = nullptr;
    std::vector<uint8_t> headers;
    std::vector<uint8_t> annexB;
    int lengthSize = 4;
    bool primed = false;
    Color color;

    ~OpenH264Decoder() override { close(); }
    const char* name() const override { return "openh264"; }
    bool drainKeepsState() const override { return false; }

    void close() {
        if (!decoder) return;
        decoder->Uninitialize();
        WelsDestroyDecoder(decoder);
        decoder = nullptr;
    }

    bool start() {
        if (WelsCreateDecoder(&decoder) != 0 || !decoder) return false;
        int quiet = WELS_LOG_QUIET;
        decoder->SetOption(DECODER_OPTION_TRACE_LEVEL, &quiet);
        SDecodingParam param = {};
        param.sVideoProperty.eVideoBsType = VIDEO_BITSTREAM_AVC;
        param.eEcActiveIdc = ERROR_CON_DISABLE;
        if (decoder->Initialize(&param) != 0) {
            WelsDestroyDecoder(decoder);
            decoder = nullptr;
            return false;
        }
        primed = false;
        return true;
    }

    void reset() override {
        close();
        start();
    }

    void hand(const uint8_t* const planes[3], const SBufferInfo& info, const PictureSink& sink) {
        if (info.iBufferStatus != 1 || !planes[0] || !planes[1] || !planes[2]) return;
        const SSysMEMBuffer& b = info.UsrData.sSystemBuffer;
        Picture p;
        p.width = b.iWidth;
        p.height = b.iHeight;
        p.chromaWidth = (b.iWidth + 1) / 2;
        p.chromaHeight = (b.iHeight + 1) / 2;
        p.plane[0] = planes[0];
        p.plane[1] = planes[1];
        p.plane[2] = planes[2];
        p.stride[0] = size_t(b.iStride[0]);
        p.stride[1] = p.stride[2] = size_t(b.iStride[1]);
        p.stamp = int64_t(info.uiOutYuvTimeStamp);
        p.color = color;
        sink(p);
    }

    bool decode(const uint8_t* data, size_t size, int64_t stamp, const PictureSink& sink) override {
        if (!decoder && !start()) return false;
        annexB.clear();
        if (!primed) annexB = headers;
        for (size_t at = 0; at + size_t(lengthSize) <= size;) {
            size_t length = 0;
            for (int k = 0; k < lengthSize; ++k) length = (length << 8) | data[at + size_t(k)];
            at += size_t(lengthSize);
            if (length > size - at) return false;
            static const uint8_t code[4] = {0, 0, 0, 1};
            annexB.insert(annexB.end(), code, code + 4);
            annexB.insert(annexB.end(), data + at, data + at + length);
            at += length;
        }
        if (annexB.empty()) return false;
        primed = true;
        uint8_t* planes[3] = {nullptr, nullptr, nullptr};
        SBufferInfo info = {};
        info.uiInBsTimeStamp = uint64_t(stamp);
        const DECODING_STATE state = decoder->DecodeFrameNoDelay(annexB.data(), int(annexB.size()), planes, &info);
        hand(planes, info, sink);
        return state == dsErrorFree;
    }

    bool drain(const PictureSink& sink) override {
        if (!decoder) return true;
        int end = 1;
        decoder->SetOption(DECODER_OPTION_END_OF_STREAM, &end);
        for (int guard = 0; guard < 64; ++guard) {
            uint8_t* planes[3] = {nullptr, nullptr, nullptr};
            SBufferInfo info = {};
            decoder->FlushFrame(planes, &info);
            if (info.iBufferStatus != 1) break;
            hand(planes, info, sink);
        }
        return true;
    }
};

}

std::unique_ptr<SoftDecoder> openOpenH264(const std::vector<uint8_t>& config, std::string* error) {
    auto fail = [&](const char* text) {
        if (error) *error = text;
        return nullptr;
    };
    const VideoInfo info = videoInfo(fourcc("avcC"), config);
    if (!info.valid) return fail("the H.264 configuration is not readable");
    if (info.interlaced) return fail("interlaced H.264 is not decoded by the software decoder on this system");
    if (info.bitDepth != 8 || info.chroma != 1) return fail("this H.264 profile (more than 8 bits or other than 4:2:0) is not decoded by the software decoder on this system");
    auto decoder = std::make_unique<OpenH264Decoder>();
    decoder->lengthSize = (config[4] & 3) + 1;
    if (info.colorKnown || info.rangeKnown) {
        decoder->color.known = info.colorKnown;
        decoder->color.primaries = info.primaries;
        decoder->color.transfer = info.transfer;
        decoder->color.matrix = info.matrix;
        decoder->color.full = info.full;
    }
    size_t at = 6;
    auto take = [&](size_t count) {
        for (size_t i = 0; i < count && at + 2 <= config.size(); ++i) {
            const size_t length = (size_t(config[at]) << 8) | config[at + 1];
            at += 2;
            if (length > config.size() - at) return;
            static const uint8_t code[4] = {0, 0, 0, 1};
            decoder->headers.insert(decoder->headers.end(), code, code + 4);
            decoder->headers.insert(decoder->headers.end(), config.begin() + long(at), config.begin() + long(at + length));
            at += length;
        }
    };
    take(config[5] & 31);
    if (at < config.size()) take(config[at++]);
    if (!decoder->start()) return fail("the H.264 software decoder did not start");
    return decoder;
}

}
#else

namespace absent {

std::unique_ptr<SoftDecoder> openOpenH264(const std::vector<uint8_t>&, std::string* error) {
    if (error) *error = "built without an H.264 software decoder";
    return nullptr;
}

}
#endif
