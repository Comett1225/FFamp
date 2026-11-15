#include "image.h"
#include "absent/media.h"
#include <deque>
#include <mutex>

namespace absent {

std::unique_ptr<SoftEncoder> openPlatformEncoder(const EncodeSpec& spec, std::string* error);

struct VideoEncoder::Impl {
    EncodeSpec spec;
    std::mutex lock;
    std::deque<Packet> packets;
    std::unique_ptr<SoftEncoder> soft;
    std::vector<uint8_t> planar;

    PacketSink keep() {
        return [this](Packet&& packet) {
            std::lock_guard<std::mutex> hold(lock);
            packets.push_back(std::move(packet));
        };
    }

    bool planarFrame(const uint8_t* data, int64_t pts, int64_t duration) {
        Picture p;
        p.width = spec.width;
        p.height = spec.height;
        p.chromaWidth = (spec.width + 1) / 2;
        p.chromaHeight = (spec.height + 1) / 2;
        p.plane[0] = data;
        p.plane[1] = data + size_t(p.width) * size_t(p.height);
        p.plane[2] = p.plane[1] + size_t(p.chromaWidth) * size_t(p.chromaHeight);
        p.stride[0] = size_t(p.width);
        p.stride[1] = p.stride[2] = size_t(p.chromaWidth);
        return soft->encode(p, pts, duration, keep());
    }

    size_t planarSize() const { return size_t(spec.width) * size_t(spec.height) + 2 * size_t((spec.width + 1) / 2) * size_t((spec.height + 1) / 2); }
};

VideoEncoder::VideoEncoder() : impl(new Impl) {}
VideoEncoder::~VideoEncoder() = default;

std::unique_ptr<VideoEncoder> VideoEncoder::open(const EncodeSpec& spec, std::string* error) {
    auto fail = [&](const char* text) {
        if (error) *error = text;
        return nullptr;
    };
    if (spec.width <= 0 || spec.height <= 0 || spec.width > 16384 || spec.height > 16384 || !spec.timescale) return fail("bad encoder settings");
    std::unique_ptr<VideoEncoder> encoder(new VideoEncoder);
    Impl& e = *encoder->impl;
    e.spec = spec;
    if (!spec.codec) e.spec.codec = spec.hevc ? fourcc("hvc1") : fourcc("avc1");
    e.spec.hevc = e.spec.codec == fourcc("hvc1") || e.spec.codec == fourcc("hev1");
    const uint32_t codec = e.spec.codec;
    if (codec == fourcc("av01")) e.soft = openSvtAv1(e.spec, error);
    else if (codec == fourcc("vp09")) e.soft = openVpxEncoder(e.spec, error);
    else if (codec == fourcc("apv1")) e.soft = openApvEncoder(e.spec, error);
    else if (!e.spec.hevc && codec != fourcc("avc1")) return fail("video codec not supported by this encoder");
    else {
        std::string problem;
        e.soft = openPlatformEncoder(e.spec, &problem);
        if (!e.soft) e.soft = e.spec.hevc ? openX265(e.spec, &problem) : openX264(e.spec, &problem);
        if (!e.soft && error) *error = problem;
    }
    if (!e.soft) return nullptr;
    return encoder;
}

bool VideoEncoder::encodeI420(const uint8_t* data, int64_t pts, int64_t duration) {
    return !impl->spec.bgra && impl->planarFrame(data, pts, duration);
}

bool VideoEncoder::encodeBGRA(const uint8_t* data, int64_t pts, int64_t duration, bool redFirst) {
    Impl& e = *impl;
    e.planar.resize(e.planarSize());
    bgraToI420(data, size_t(e.spec.width) * 4, e.spec.width, e.spec.height, e.planar.data(), redFirst);
    return e.planarFrame(e.planar.data(), pts, duration);
}

bool VideoEncoder::encode(const Frame& frame, int64_t pts, int64_t duration) { return encodePixelBuffer(frame.native.get(), pts, duration); }

bool VideoEncoder::encodePixelBuffer(void* pixelBuffer, int64_t pts, int64_t duration) {
    Impl& e = *impl;
    const Image* image = imageOf(pixelBuffer);
    if (!image || image->picture.width != e.spec.width || image->picture.height != e.spec.height) return false;
    pictureToI420(image->picture, e.planar);
    return e.planarFrame(e.planar.data(), pts, duration);
}

bool VideoEncoder::encodeSurface(void*, int64_t, int64_t) { return false; }

const char* VideoEncoder::backend() const { return impl->soft->name(); }

bool VideoEncoder::finish() { return impl->soft->finish(impl->keep()); }

bool VideoEncoder::take(Packet& out) {
    std::lock_guard<std::mutex> hold(impl->lock);
    if (impl->packets.empty()) return false;
    out = std::move(impl->packets.front());
    impl->packets.pop_front();
    return true;
}

uint32_t VideoEncoder::configType() const { return impl->soft->configType(); }

std::vector<uint8_t> VideoEncoder::config() const { return impl->soft->config(); }

}
