#include "absent/media.h"
#include "soft_video.h"
#include <CoreMedia/CoreMedia.h>
#include <CoreVideo/CoreVideo.h>
#include <VideoToolbox/VideoToolbox.h>
#include <algorithm>
#include <atomic>
#include <cstring>
#include <functional>
#include <map>
#include <mutex>

namespace absent {

namespace {

const uint32_t kNone = 0xFFFFFFFFu;

void number(CFMutableDictionaryRef dict, CFStringRef key, int32_t value) {
    CFNumberRef n = CFNumberCreate(nullptr, kCFNumberSInt32Type, &value);
    CFDictionarySetValue(dict, key, n);
    CFRelease(n);
}

CFMutableDictionaryRef dictionary() {
    return CFDictionaryCreateMutable(nullptr, 0, &kCFTypeDictionaryKeyCallBacks, &kCFTypeDictionaryValueCallBacks);
}

OSType pixelFormat(const StreamShape& shape) {
    const bool full = shape.color.full;
    const int chroma = shape.chromaX && shape.chromaY ? 0 : shape.chromaX ? 1 : 2;
    static const OSType narrow[2][3] = {
        {kCVPixelFormatType_420YpCbCr8BiPlanarVideoRange, kCVPixelFormatType_422YpCbCr8BiPlanarVideoRange, kCVPixelFormatType_444YpCbCr8BiPlanarVideoRange},
        {kCVPixelFormatType_420YpCbCr8BiPlanarFullRange, kCVPixelFormatType_422YpCbCr8BiPlanarFullRange, kCVPixelFormatType_444YpCbCr8BiPlanarFullRange}};
    static const OSType deep[2][3] = {
        {kCVPixelFormatType_420YpCbCr10BiPlanarVideoRange, kCVPixelFormatType_422YpCbCr10BiPlanarVideoRange, kCVPixelFormatType_444YpCbCr10BiPlanarVideoRange},
        {kCVPixelFormatType_420YpCbCr10BiPlanarFullRange, kCVPixelFormatType_422YpCbCr10BiPlanarFullRange, kCVPixelFormatType_444YpCbCr10BiPlanarFullRange}};
    return (shape.bits > 8 ? deep : narrow)[full ? 1 : 0][chroma];
}

void streamColor(bool hevc, const std::vector<uint8_t>& c, Color& color) {
    std::vector<const uint8_t*> sets;
    std::vector<size_t> sizes;
    auto list = [&](size_t& at, size_t count) {
        for (size_t i = 0; i < count && at + 2 <= c.size(); ++i) {
            const size_t size = (size_t(c[at]) << 8) | c[at + 1];
            at += 2;
            if (size > c.size() - at) return;
            sets.push_back(c.data() + at);
            sizes.push_back(size);
            at += size;
        }
    };
    int lengthSize = 4;
    if (hevc) {
        if (c.size() < 23) return;
        lengthSize = (c[21] & 3) + 1;
        size_t at = 23;
        for (int n = 0; n < c[22] && at + 3 <= c.size(); ++n) {
            const size_t count = (size_t(c[at + 1]) << 8) | c[at + 2];
            at += 3;
            list(at, count);
        }
    } else {
        if (c.size() < 7) return;
        lengthSize = (c[4] & 3) + 1;
        size_t at = 6;
        list(at, c[5] & 31);
        if (at < c.size()) {
            const size_t count = c[at++];
            list(at, count);
        }
    }
    if (sets.size() < 2) return;
    CMVideoFormatDescriptionRef told = nullptr;
    OSStatus status = hevc ? CMVideoFormatDescriptionCreateFromHEVCParameterSets(nullptr, sets.size(), sets.data(), sizes.data(), lengthSize, nullptr, &told)
                           : CMVideoFormatDescriptionCreateFromH264ParameterSets(nullptr, sets.size(), sets.data(), sizes.data(), lengthSize, &told);
    if (status != noErr || !told) return;
    auto text = [&](CFStringRef key, int (*convert)(CFStringRef)) {
        CFTypeRef value = CMFormatDescriptionGetExtension(told, key);
        return value && CFGetTypeID(value) == CFStringGetTypeID() ? convert(static_cast<CFStringRef>(value)) : 2;
    };
    color.full = CMFormatDescriptionGetExtension(told, kCMFormatDescriptionExtension_FullRangeVideo) == kCFBooleanTrue;
    color.primaries = text(kCMFormatDescriptionExtension_ColorPrimaries, CVColorPrimariesGetIntegerCodePointForString);
    color.transfer = text(kCMFormatDescriptionExtension_TransferFunction, CVTransferFunctionGetIntegerCodePointForString);
    color.matrix = text(kCMFormatDescriptionExtension_YCbCrMatrix, CVYCbCrMatrixGetIntegerCodePointForString);
    color.known = color.matrix != 2 || color.transfer != 2 || color.primaries != 2;
    CFRelease(told);
}

bool describe(CVPixelBufferRef buffer, Picture& p) {
    if (CVPixelBufferGetPlaneCount(buffer) != 2) return false;
    switch (CVPixelBufferGetPixelFormatType(buffer)) {
        case kCVPixelFormatType_420YpCbCr8BiPlanarFullRange:
        case kCVPixelFormatType_422YpCbCr8BiPlanarFullRange:
        case kCVPixelFormatType_444YpCbCr8BiPlanarFullRange:
            p.color.full = true;
            [[fallthrough]];
        case kCVPixelFormatType_420YpCbCr8BiPlanarVideoRange:
        case kCVPixelFormatType_422YpCbCr8BiPlanarVideoRange:
        case kCVPixelFormatType_444YpCbCr8BiPlanarVideoRange:
            p.bits = 8;
            p.shift = 0;
            break;
        case kCVPixelFormatType_420YpCbCr10BiPlanarFullRange:
        case kCVPixelFormatType_422YpCbCr10BiPlanarFullRange:
        case kCVPixelFormatType_444YpCbCr10BiPlanarFullRange:
            p.color.full = true;
            [[fallthrough]];
        case kCVPixelFormatType_420YpCbCr10BiPlanarVideoRange:
        case kCVPixelFormatType_422YpCbCr10BiPlanarVideoRange:
        case kCVPixelFormatType_444YpCbCr10BiPlanarVideoRange:
            p.bits = 10;
            p.shift = 6;
            break;
        default:
            return false;
    }
    p.width = int(CVPixelBufferGetWidth(buffer));
    p.height = int(CVPixelBufferGetHeight(buffer));
    p.chromaWidth = int(CVPixelBufferGetWidthOfPlane(buffer, 1));
    p.chromaHeight = int(CVPixelBufferGetHeightOfPlane(buffer, 1));
    p.interleaved = true;
    return true;
}

int codePoint(CVPixelBufferRef buffer, CFStringRef key, int (*convert)(CFStringRef)) {
    CFTypeRef value = CVBufferCopyAttachment(buffer, key, nullptr);
    if (!value) return 2;
    int code = CFGetTypeID(value) == CFStringGetTypeID() ? convert(static_cast<CFStringRef>(value)) : 2;
    CFRelease(value);
    return code;
}

void fill(const Picture& p, CVPixelBufferRef buffer) {
    CVPixelBufferLockBaseAddress(buffer, 0);
    const bool wide = p.bits > 8;
    const int lift = 16 - p.bits;
    uint8_t* luma = static_cast<uint8_t*>(CVPixelBufferGetBaseAddressOfPlane(buffer, 0));
    const size_t lumaStride = CVPixelBufferGetBytesPerRowOfPlane(buffer, 0);
    for (int y = 0; y < p.height; ++y) {
        const uint8_t* from = p.plane[0] + size_t(y) * p.stride[0];
        uint8_t* to = luma + size_t(y) * lumaStride;
        if (!wide) {
            memcpy(to, from, size_t(p.width));
            continue;
        }
        const uint16_t* in = reinterpret_cast<const uint16_t*>(from);
        uint16_t* out = reinterpret_cast<uint16_t*>(to);
        for (int x = 0; x < p.width; ++x) out[x] = uint16_t(in[x] << lift);
    }
    uint8_t* chroma = static_cast<uint8_t*>(CVPixelBufferGetBaseAddressOfPlane(buffer, 1));
    const size_t chromaStride = CVPixelBufferGetBytesPerRowOfPlane(buffer, 1);
    for (int y = 0; y < p.chromaHeight; ++y) {
        const uint8_t* u = p.plane[1] + size_t(y) * p.stride[1];
        const uint8_t* v = p.plane[2] + size_t(y) * p.stride[2];
        uint8_t* to = chroma + size_t(y) * chromaStride;
        if (!wide) {
            for (int x = 0; x < p.chromaWidth; ++x) {
                to[2 * x] = u[x];
                to[2 * x + 1] = v[x];
            }
            continue;
        }
        const uint16_t* a = reinterpret_cast<const uint16_t*>(u);
        const uint16_t* b = reinterpret_cast<const uint16_t*>(v);
        uint16_t* out = reinterpret_cast<uint16_t*>(to);
        for (int x = 0; x < p.chromaWidth; ++x) {
            out[2 * x] = uint16_t(a[x] << lift);
            out[2 * x + 1] = uint16_t(b[x] << lift);
        }
    }
    CVPixelBufferUnlockBaseAddress(buffer, 0);
}

}

bool withPixelBuffer(void* pixelBuffer, const std::function<bool(const Picture&)>& use) {
    auto buffer = static_cast<CVPixelBufferRef>(pixelBuffer);
    Picture p;
    if (!buffer || !describe(buffer, p)) return false;
    if (CVPixelBufferLockBaseAddress(buffer, kCVPixelBufferLock_ReadOnly) != kCVReturnSuccess) return false;
    for (int i = 0; i < 2; ++i) {
        p.plane[i] = static_cast<const uint8_t*>(CVPixelBufferGetBaseAddressOfPlane(buffer, size_t(i)));
        p.stride[i] = CVPixelBufferGetBytesPerRowOfPlane(buffer, size_t(i));
    }
    bool ok = p.plane[0] && p.plane[1] && use(p);
    CVPixelBufferUnlockBaseAddress(buffer, kCVPixelBufferLock_ReadOnly);
    return ok;
}

namespace {

bool locked(const Frame& frame, const std::function<bool(Picture&)>& use) {
    return withPixelBuffer(frame.native.get(), [&](const Picture& seen) {
        Picture p = seen;
        p.color = frame.color;
        p.color.full = seen.color.full;
        return use(p);
    });
}

}

bool copyYuv(const Frame& frame, std::vector<uint8_t>& out) {
    if (frame.bits == 8 && frame.chromaWidth == (frame.width + 1) / 2 && frame.chromaHeight == (frame.height + 1) / 2) return copyI420(frame, out);
    return locked(frame, [&](Picture& p) {
        pictureToPlanar(p, out);
        return true;
    });
}

bool copyBGRA(const Frame& frame, std::vector<uint8_t>& out) {
    if (frame.width <= 0 || frame.height <= 0) return false;
    out.resize(size_t(frame.width) * size_t(frame.height) * 4);
    return locked(frame, [&](Picture& p) { return pictureToBGRA(p, out.data(), size_t(p.width) * 4); });
}

bool copyI420(const Frame& frame, std::vector<uint8_t>& out) {
    auto buffer = static_cast<CVPixelBufferRef>(frame.native.get());
    if (!buffer || CVPixelBufferGetPlaneCount(buffer) != 2 || frame.bits != 8) return false;
    if (CVPixelBufferGetWidthOfPlane(buffer, 1) != (CVPixelBufferGetWidth(buffer) + 1) / 2 || CVPixelBufferGetHeightOfPlane(buffer, 1) != (CVPixelBufferGetHeight(buffer) + 1) / 2) return false;
    if (CVPixelBufferLockBaseAddress(buffer, kCVPixelBufferLock_ReadOnly) != kCVReturnSuccess) return false;
    const size_t w = CVPixelBufferGetWidth(buffer), h = CVPixelBufferGetHeight(buffer);
    const size_t cw = (w + 1) / 2, ch = (h + 1) / 2;
    out.resize(w * h + 2 * cw * ch);
    const uint8_t* y = static_cast<const uint8_t*>(CVPixelBufferGetBaseAddressOfPlane(buffer, 0));
    const size_t yStride = CVPixelBufferGetBytesPerRowOfPlane(buffer, 0);
    for (size_t row = 0; row < h; ++row) memcpy(out.data() + row * w, y + row * yStride, w);
    const uint8_t* uv = static_cast<const uint8_t*>(CVPixelBufferGetBaseAddressOfPlane(buffer, 1));
    const size_t uvStride = CVPixelBufferGetBytesPerRowOfPlane(buffer, 1);
    uint8_t* u = out.data() + w * h;
    uint8_t* v = u + cw * ch;
    for (size_t row = 0; row < ch; ++row) {
        const uint8_t* line = uv + row * uvStride;
        for (size_t x = 0; x < cw; ++x) {
            u[row * cw + x] = line[2 * x];
            v[row * cw + x] = line[2 * x + 1];
        }
    }
    CVPixelBufferUnlockBaseAddress(buffer, kCVPixelBufferLock_ReadOnly);
    return true;
}

struct VideoDecoder::Impl {
    Reader* file = nullptr;
    const Track* track = nullptr;
    CMVideoFormatDescriptionRef format = nullptr;
    VTDecompressionSessionRef session = nullptr;
    std::mutex lock;
    std::map<int64_t, CVPixelBufferRef> ready;
    std::atomic<uint64_t> shown{0};
    std::atomic<int64_t> floor{0};
    bool keep = true;
    uint32_t next = kNone;
    uint64_t fed = 0;
    uint64_t restarts = 0;
    std::unique_ptr<SoftDecoder> soft;
    CVPixelBufferPoolRef pool = nullptr;
    OSType poolFormat = 0;
    int poolWidth = 0;
    int poolHeight = 0;
    std::vector<uint8_t> packet;
    StreamShape shape;
    const char* backend = "videotoolbox";
    struct Shown {
        uint32_t first = kNone;
        uint32_t second = kNone;
        bool firstTop = true;
    };
    std::vector<Shown> list;
    std::vector<uint32_t> keys;

    void pairFields(const H264Layout& layout) {
        const auto& samples = track->samples;
        std::vector<uint8_t> kind(samples.size(), 0);
        std::vector<uint32_t> number(samples.size(), 0);
        std::vector<uint8_t> bytes;
        for (uint32_t i = 0; i < samples.size(); ++i) {
            bytes.resize(samples[i].size);
            if (file->read(*track, i, bytes.data())) kind[i] = uint8_t(h264FieldOf(layout, bytes.data(), bytes.size(), number[i]));
        }
        std::vector<uint8_t> second(samples.size(), 0);
        for (uint32_t i = 0; i + 1 < samples.size(); ++i) {
            if (kind[i] && kind[i + 1] && kind[i] != kind[i + 1] && number[i] == number[i + 1] && !second[i]) second[i + 1] = 1;
        }
        std::vector<Shown> paired;
        for (uint32_t index : track->order) {
            if (index >= samples.size() || second[index]) continue;
            Shown item;
            item.first = index;
            if (index + 1 < samples.size() && second[index + 1]) item.second = index + 1;
            item.firstTop = kind[index] != 2;
            paired.push_back(item);
        }
        list = std::move(paired);
        keys.clear();
        for (uint32_t index : track->syncs) if (index < samples.size() && !second[index]) keys.push_back(index);
    }

    CVPixelBufferRef peek(int64_t pts) {
        std::lock_guard<std::mutex> hold(lock);
        auto found = ready.find(pts);
        return found == ready.end() ? nullptr : CVPixelBufferRetain(found->second);
    }

    bool has(int64_t pts) {
        std::lock_guard<std::mutex> hold(lock);
        return ready.count(pts) > 0;
    }

    CVPixelBufferRef weave(CVPixelBufferRef top, CVPixelBufferRef bottom) {
        const OSType format = CVPixelBufferGetPixelFormatType(top);
        const size_t width = CVPixelBufferGetWidth(top), height = CVPixelBufferGetHeight(top);
        if (CVPixelBufferGetPixelFormatType(bottom) != format || CVPixelBufferGetWidth(bottom) != width || CVPixelBufferGetHeight(bottom) != height) return nullptr;
        CFMutableDictionaryRef picture = dictionary();
        CFMutableDictionaryRef surface = dictionary();
        CFDictionarySetValue(picture, kCVPixelBufferIOSurfacePropertiesKey, surface);
        CFRelease(surface);
        CFDictionarySetValue(picture, kCVPixelBufferMetalCompatibilityKey, kCFBooleanTrue);
        CVPixelBufferRef out = nullptr;
        CVReturn made = CVPixelBufferCreate(nullptr, width, height, format, picture, &out);
        CFRelease(picture);
        if (made != kCVReturnSuccess) return nullptr;
        CVPixelBufferLockBaseAddress(top, kCVPixelBufferLock_ReadOnly);
        CVPixelBufferLockBaseAddress(bottom, kCVPixelBufferLock_ReadOnly);
        CVPixelBufferLockBaseAddress(out, 0);
        for (size_t plane = 0; plane < CVPixelBufferGetPlaneCount(out); ++plane) {
            const size_t rows = CVPixelBufferGetHeightOfPlane(out, plane), stride = CVPixelBufferGetBytesPerRowOfPlane(out, plane);
            uint8_t* to = static_cast<uint8_t*>(CVPixelBufferGetBaseAddressOfPlane(out, plane));
            for (size_t row = 0; row < rows; ++row) {
                CVPixelBufferRef source = row % 2 ? bottom : top;
                const size_t sourceStride = CVPixelBufferGetBytesPerRowOfPlane(source, plane);
                const uint8_t* from = static_cast<const uint8_t*>(CVPixelBufferGetBaseAddressOfPlane(source, plane));
                memcpy(to + row * stride, from + row * sourceStride, std::min(stride, sourceStride));
            }
        }
        CVPixelBufferUnlockBaseAddress(out, 0);
        CVPixelBufferUnlockBaseAddress(bottom, kCVPixelBufferLock_ReadOnly);
        CVPixelBufferUnlockBaseAddress(top, kCVPixelBufferLock_ReadOnly);
        CVBufferPropagateAttachments(top, out);
        return out;
    }

    ~Impl() {
        if (session) {
            VTDecompressionSessionWaitForAsynchronousFrames(session);
            VTDecompressionSessionInvalidate(session);
            CFRelease(session);
        }
        clear();
        if (format) CFRelease(format);
        if (pool) CFRelease(pool);
    }

    void wait() {
        if (session) VTDecompressionSessionWaitForAsynchronousFrames(session);
    }

    void finishDelayed() {
        if (!session) return;
        VTDecompressionSessionFinishDelayedFrames(session);
        VTDecompressionSessionWaitForAsynchronousFrames(session);
    }

    void restart() {
        wait();
        if (soft) soft->reset();
        clear();
    }

    CVPixelBufferRef surfaceFor(const Picture& p) {
        StreamShape made;
        made.bits = p.bits;
        made.chromaX = p.chromaWidth < p.width;
        made.chromaY = p.chromaHeight < p.height;
        made.color.full = p.color.full;
        const OSType wanted = pixelFormat(made);
        if (!pool || poolFormat != wanted || poolWidth != p.width || poolHeight != p.height) {
            if (pool) CFRelease(pool);
            pool = nullptr;
            CFMutableDictionaryRef picture = dictionary();
            number(picture, kCVPixelBufferPixelFormatTypeKey, int32_t(wanted));
            number(picture, kCVPixelBufferWidthKey, p.width);
            number(picture, kCVPixelBufferHeightKey, p.height);
            CFMutableDictionaryRef surface = dictionary();
            CFDictionarySetValue(picture, kCVPixelBufferIOSurfacePropertiesKey, surface);
            CFRelease(surface);
            CFDictionarySetValue(picture, kCVPixelBufferMetalCompatibilityKey, kCFBooleanTrue);
            CVPixelBufferPoolCreate(nullptr, nullptr, picture, &pool);
            CFRelease(picture);
            poolFormat = wanted;
            poolWidth = p.width;
            poolHeight = p.height;
        }
        CVPixelBufferRef buffer = nullptr;
        if (!pool || CVPixelBufferPoolCreatePixelBuffer(nullptr, pool, &buffer) != kCVReturnSuccess) return nullptr;
        if (int(CVPixelBufferGetWidthOfPlane(buffer, 1)) != p.chromaWidth || int(CVPixelBufferGetHeightOfPlane(buffer, 1)) != p.chromaHeight) {
            CFRelease(buffer);
            return nullptr;
        }
        fill(p, buffer);
        return buffer;
    }

    bool feedSoft(uint32_t index) {
        const Sample& s = track->samples[index];
        packet.resize(s.size);
        if (!file->read(*track, index, packet.data())) return false;
        ++fed;
        return soft->decode(packet.data(), packet.size(), [&](const Picture& p) {
            ++shown;
            if (p.color.known && !shape.color.known) shape.color = p.color;
            if (!keep || s.pts < floor) return;
            CVPixelBufferRef buffer = surfaceFor(p);
            if (!buffer) return;
            std::lock_guard<std::mutex> hold(lock);
            auto found = ready.find(s.pts);
            if (found != ready.end()) CFRelease(found->second);
            ready[s.pts] = buffer;
        });
    }

    void clear() {
        std::lock_guard<std::mutex> hold(lock);
        for (auto& item : ready) CFRelease(item.second);
        ready.clear();
    }

    static void output(void* self, void*, OSStatus status, VTDecodeInfoFlags, CVImageBufferRef image, CMTime pts, CMTime) {
        auto* d = static_cast<Impl*>(self);
        if (status != noErr || !image) return;
        ++d->shown;
        if (!d->keep || pts.value < d->floor) return;
        std::lock_guard<std::mutex> hold(d->lock);
        auto found = d->ready.find(pts.value);
        if (found != d->ready.end()) CFRelease(found->second);
        d->ready[pts.value] = CVPixelBufferRetain(image);
    }

    bool feed(uint32_t index, bool async) {
        if (soft) return feedSoft(index);
        const Sample& s = track->samples[index];
        CMBlockBufferRef block = nullptr;
        if (CMBlockBufferCreateWithMemoryBlock(nullptr, nullptr, s.size, nullptr, nullptr, 0, s.size, kCMBlockBufferAssureMemoryNowFlag, &block) != noErr) return false;
        char* bytes = nullptr;
        bool ok = CMBlockBufferGetDataPointer(block, 0, nullptr, nullptr, &bytes) == noErr && file->read(*track, index, reinterpret_cast<uint8_t*>(bytes));
        CMSampleBufferRef sample = nullptr;
        if (ok) {
            CMSampleTimingInfo timing;
            timing.duration = CMTimeMake(s.duration, int32_t(track->timescale));
            timing.presentationTimeStamp = CMTimeMake(s.pts, int32_t(track->timescale));
            timing.decodeTimeStamp = CMTimeMake(s.dts, int32_t(track->timescale));
            const size_t size = s.size;
            ok = CMSampleBufferCreateReady(nullptr, block, format, 1, 1, &timing, 1, &size, &sample) == noErr;
        }
        if (ok) {
            VTDecodeFrameFlags flags = async ? kVTDecodeFrame_EnableAsynchronousDecompression : 0;
            OSStatus status = VTDecompressionSessionDecodeFrame(session, sample, flags, nullptr, nullptr);
            ok = status == noErr;
            ++fed;
        }
        if (sample) CFRelease(sample);
        CFRelease(block);
        return ok;
    }

    uint32_t keyFor(uint32_t target) const {
        const auto& syncs = keys;
        auto it = std::upper_bound(syncs.begin(), syncs.end(), target);
        while (it != syncs.begin()) {
            --it;
            if (track->samples[*it].pts <= track->samples[target].pts) return *it;
        }
        return syncs.empty() ? 0 : syncs.front();
    }

    CVPixelBufferRef take(int64_t pts) {
        std::lock_guard<std::mutex> hold(lock);
        auto found = ready.find(pts);
        if (found == ready.end()) return nullptr;
        CVPixelBufferRef buffer = CVPixelBufferRetain(found->second);
        for (auto it = ready.begin(); it != found;) {
            CFRelease(it->second);
            it = ready.erase(it);
        }
        return buffer;
    }
};

VideoDecoder::VideoDecoder() : impl(new Impl) {}
VideoDecoder::~VideoDecoder() = default;

std::unique_ptr<VideoDecoder> VideoDecoder::open(Reader& file, const Track& track, std::string* error, bool software) {
    auto fail = [&](const char* text) {
        if (error) *error = text;
        return nullptr;
    };
    if (track.samples.empty() || !track.timescale || track.width <= 0 || track.height <= 0) return fail("track is empty");
    const uint32_t codec = track.codec;
    const bool vp9 = codec == fourcc("vp09"), vp8 = codec == fourcc("vp08"), av1 = codec == fourcc("av01");
    const bool prores = codec == fourcc("apch") || codec == fourcc("apcn") || codec == fourcc("apcs") || codec == fourcc("apco") || codec == fourcc("ap4h") || codec == fourcc("ap4x");
    const bool h264 = track.configType == fourcc("avcC"), hevc = track.configType == fourcc("hvcC");
    if (!h264 && !hevc && !vp9 && !vp8 && !av1 && !prores) return fail("video codec not supported by this decoder");
    if ((h264 || hevc) && track.config.empty()) return fail("track is empty");

    std::unique_ptr<VideoDecoder> decoder(new VideoDecoder);
    Impl& d = *decoder->impl;
    d.file = &file;
    d.track = &track;
    d.shape.width = track.width;
    d.shape.height = track.height;

    std::vector<uint8_t> config = track.config;
    if (prores) {
        d.shape.bits = 10;
        d.shape.chromaY = 0;
        d.shape.chromaX = codec == fourcc("ap4h") || codec == fourcc("ap4x") ? 0 : 1;
    } else if (!shapeFromConfig(track.configType, config, d.shape) && (vp9 || av1)) {
        config.clear();
    }
    if ((vp9 || av1) && config.empty()) {
        const uint32_t first = track.syncs.empty() ? 0 : track.syncs.front();
        if (first >= track.samples.size()) return fail("track is empty");
        std::vector<uint8_t> key(track.samples[first].size);
        if (!file.read(track, first, key.data())) return fail("cannot read the first key frame");
        if (vp9) {
            if (!vp9Shape(key.data(), key.size(), d.shape)) return fail("the first VP9 key frame has no readable header");
            config = vp9Config(d.shape, track.rate(), 0);
        } else if (!av1Config(key.data(), key.size(), config, d.shape)) {
            return fail("the first AV1 key frame has no sequence header");
        }
    }
    d.keys = track.syncs;
    H264Layout layout;
    if (h264 && h264Layout(config, layout) && layout.fields) d.pairFields(layout);
    if (d.list.empty()) {
        d.list.reserve(track.order.size());
        for (uint32_t index : track.order) {
            if (index >= track.samples.size()) return fail("track is broken");
            Impl::Shown item;
            item.first = index;
            d.list.push_back(item);
        }
    }
    if (h264 || hevc) streamColor(hevc, config, d.shape.color);
    const Color fromStream = d.shape.color;
    if (track.color.known) d.shape.color = track.color;
    if (h264 || hevc || !track.color.known) d.shape.color.full = fromStream.full;

    auto softly = [&]() -> std::unique_ptr<VideoDecoder> {
        d.soft = av1 ? openDav1d(error) : openVpxDecoder(vp8, error);
        if (!d.soft) return nullptr;
        d.backend = d.soft->name();
        return std::move(decoder);
    };
    if (vp8 || (software && (vp9 || av1))) return softly();

    CMVideoCodecType type = h264 ? kCMVideoCodecType_H264 : hevc ? kCMVideoCodecType_HEVC : vp9 ? kCMVideoCodecType_VP9 : av1 ? kCMVideoCodecType_AV1 : codec;
    if (vp9 || av1) VTRegisterSupplementalVideoDecoderIfAvailable(type);
    CFMutableDictionaryRef extensions = dictionary();
    if (!prores) {
        CFMutableDictionaryRef atoms = dictionary();
        CFDataRef data = CFDataCreate(nullptr, config.data(), CFIndex(config.size()));
        CFDictionarySetValue(atoms, h264 ? CFSTR("avcC") : hevc ? CFSTR("hvcC") : vp9 ? CFSTR("vpcC") : CFSTR("av1C"), data);
        CFRelease(data);
        CFDictionarySetValue(extensions, kCMFormatDescriptionExtension_SampleDescriptionExtensionAtoms, atoms);
        CFRelease(atoms);
    }
    if (d.shape.color.known) {
        if (CFStringRef text = CVColorPrimariesGetStringForIntegerCodePoint(d.shape.color.primaries)) CFDictionarySetValue(extensions, kCMFormatDescriptionExtension_ColorPrimaries, text);
        if (CFStringRef text = CVTransferFunctionGetStringForIntegerCodePoint(d.shape.color.transfer)) CFDictionarySetValue(extensions, kCMFormatDescriptionExtension_TransferFunction, text);
        if (CFStringRef text = CVYCbCrMatrixGetStringForIntegerCodePoint(d.shape.color.matrix)) CFDictionarySetValue(extensions, kCMFormatDescriptionExtension_YCbCrMatrix, text);
        CFDictionarySetValue(extensions, kCMFormatDescriptionExtension_FullRangeVideo, d.shape.color.full ? kCFBooleanTrue : kCFBooleanFalse);
    }
    OSStatus status = CMVideoFormatDescriptionCreate(nullptr, type, track.width, track.height, extensions, &d.format);
    CFRelease(extensions);
    if (status != noErr) return fail("cannot describe the video stream");

    CFMutableDictionaryRef picture = dictionary();
    number(picture, kCVPixelBufferPixelFormatTypeKey, int32_t(pixelFormat(d.shape)));
    CFMutableDictionaryRef surface = dictionary();
    CFDictionarySetValue(picture, kCVPixelBufferIOSurfacePropertiesKey, surface);
    CFRelease(surface);
    CFDictionarySetValue(picture, kCVPixelBufferMetalCompatibilityKey, kCFBooleanTrue);
    VTDecompressionOutputCallbackRecord callback{Impl::output, &d};
    status = VTDecompressionSessionCreate(nullptr, d.format, nullptr, picture, &callback, &d.session);
    CFRelease(picture);
    if (status != noErr) {
        d.session = nullptr;
        if (vp9 || av1) return softly();
        return fail("the system video decoder refused this stream");
    }
    return decoder;
}

const char* VideoDecoder::backend() const { return impl->backend; }
uint32_t VideoDecoder::frames() const { return uint32_t(impl->list.size()); }
uint64_t VideoDecoder::fed() const { return impl->fed; }
uint64_t VideoDecoder::restarts() const { return impl->restarts; }

Frame VideoDecoder::frame(uint32_t index) {
    Impl& d = *impl;
    const Track& track = *d.track;
    if (index >= d.list.size()) return {};
    const Impl::Shown item = d.list[index];
    const bool pair = item.second != kNone;
    const uint32_t target = pair ? item.second : item.first;
    const int64_t pts = track.samples[item.first].pts;
    const int64_t later = track.samples[target].pts;
    d.keep = true;
    d.floor = std::min(pts, later);
    if (!d.has(pts) || !d.has(later)) {
        const uint32_t key = d.keyFor(item.first);
        if (d.next == kNone || d.next < key || d.next > item.first) {
            d.restart();
            d.next = key;
            ++d.restarts;
        }
        while (d.next <= target) {
            if (!d.feed(d.next, true)) break;
            ++d.next;
        }
        d.wait();
        if (!d.has(pts) || !d.has(later)) d.finishDelayed();
    }
    CVPixelBufferRef buffer = nullptr;
    if (pair) {
        CVPixelBufferRef one = d.peek(pts);
        CVPixelBufferRef two = d.peek(later);
        if (one && two) buffer = item.firstTop ? d.weave(one, two) : d.weave(two, one);
        if (one) CFRelease(one);
        if (two) CFRelease(two);
        if (CVPixelBufferRef old = d.take(std::min(pts, later))) CFRelease(old);
    } else {
        buffer = d.take(pts);
    }
    if (!buffer) return {};
    Frame frame;
    frame.native = std::shared_ptr<void>(buffer, [](void* p) { CFRelease(p); });
    frame.width = int(CVPixelBufferGetWidth(buffer));
    frame.height = int(CVPixelBufferGetHeight(buffer));
    frame.pts = pts;
    frame.duration = int64_t(track.samples[item.first].duration) + (pair ? int64_t(track.samples[item.second].duration) : 0);
    Picture described;
    if (describe(buffer, described)) {
        frame.bits = described.bits;
        frame.chromaWidth = described.chromaWidth;
        frame.chromaHeight = described.chromaHeight;
    }
    frame.color = d.shape.color;
    frame.color.full = described.color.full;
    if (!frame.color.known) {
        frame.color.primaries = codePoint(buffer, kCVImageBufferColorPrimariesKey, CVColorPrimariesGetIntegerCodePointForString);
        frame.color.transfer = codePoint(buffer, kCVImageBufferTransferFunctionKey, CVTransferFunctionGetIntegerCodePointForString);
        frame.color.matrix = codePoint(buffer, kCVImageBufferYCbCrMatrixKey, CVYCbCrMatrixGetIntegerCodePointForString);
        frame.color.known = frame.color.matrix != 2 || frame.color.transfer != 2;
    }
    return frame;
}

uint64_t VideoDecoder::run(bool async) {
    Impl& d = *impl;
    d.restart();
    d.keep = false;
    d.shown = 0;
    for (uint32_t i = 0; i < d.track->samples.size(); ++i) d.feed(i, async);
    d.finishDelayed();
    d.next = kNone;
    return d.shown;
}

}
