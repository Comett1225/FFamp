#include "absent/media.h"
#include "nal.h"
#include "readers.h"
#include "soft_video.h"
#include "own_video.h"
#include <CoreMedia/CoreMedia.h>
#include <CoreVideo/CoreVideo.h>
#include <VideoToolbox/VideoToolbox.h>
#include <algorithm>
#include <atomic>
#include <condition_variable>
#include <cstdlib>
#include <cstring>
#include <deque>
#include <functional>
#include <map>
#include <mutex>
#include <thread>

namespace absent {

std::unique_ptr<SoftDecoder> openStillDecoder(std::string* error);

namespace {

const uint32_t kNone = 0xFFFFFFFFu;
const CFStringRef kAlphaKey = CFSTR("absent.alpha");

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

template <typename T>
void liftRow(const T* __restrict in, T* __restrict out, int count, int lift) {
    for (int x = 0; x < count; ++x) out[x] = T(in[x] << lift);
}

template <typename T>
void weaveRow(const T* __restrict a, const T* __restrict b, T* __restrict out, int count, int lift, int source) {
    if (source == count) {
        for (int x = 0; x < count; ++x) {
            out[2 * x] = T(a[x] << lift);
            out[2 * x + 1] = T(b[x] << lift);
        }
        return;
    }
    for (int x = 0; x < count; ++x) {
        const int from = int(int64_t(x) * source / count);
        out[2 * x] = T(a[from] << lift);
        out[2 * x + 1] = T(b[from] << lift);
    }
}

void fill(const Picture& p, CVPixelBufferRef buffer) {
    CVPixelBufferLockBaseAddress(buffer, 0);
    const bool wide = p.bits > 8;
    const int lift = wide ? 16 - p.bits : 0;
    uint8_t* luma = static_cast<uint8_t*>(CVPixelBufferGetBaseAddressOfPlane(buffer, 0));
    const size_t lumaStride = CVPixelBufferGetBytesPerRowOfPlane(buffer, 0);
    for (int y = 0; y < p.height; ++y) {
        const uint8_t* from = p.plane[0] + size_t(y) * p.stride[0];
        uint8_t* to = luma + size_t(y) * lumaStride;
        if (wide) liftRow(reinterpret_cast<const uint16_t*>(from), reinterpret_cast<uint16_t*>(to), p.width, lift);
        else memcpy(to, from, size_t(p.width));
    }
    uint8_t* chroma = static_cast<uint8_t*>(CVPixelBufferGetBaseAddressOfPlane(buffer, 1));
    const size_t chromaStride = CVPixelBufferGetBytesPerRowOfPlane(buffer, 1);
    const int rows = int(CVPixelBufferGetHeightOfPlane(buffer, 1));
    const int columns = int(CVPixelBufferGetWidthOfPlane(buffer, 1));
    for (int y = 0; y < rows; ++y) {
        const int from = int(int64_t(y) * p.chromaHeight / rows);
        const uint8_t* u = p.plane[1] + size_t(from) * p.stride[1];
        const uint8_t* v = p.plane[2] + size_t(from) * p.stride[2];
        uint8_t* to = chroma + size_t(y) * chromaStride;
        if (wide) weaveRow(reinterpret_cast<const uint16_t*>(u), reinterpret_cast<const uint16_t*>(v), reinterpret_cast<uint16_t*>(to), columns, lift, p.chromaWidth);
        else weaveRow(u, v, to, columns, 0, p.chromaWidth);
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
    CFTypeRef veil = CVBufferCopyAttachment(buffer, kAlphaKey, nullptr);
    if (veil && CFGetTypeID(veil) == CFDataGetTypeID() && size_t(CFDataGetLength(static_cast<CFDataRef>(veil))) == size_t(p.width) * size_t(p.height)) {
        p.alpha = CFDataGetBytePtr(static_cast<CFDataRef>(veil));
        p.alphaStride = size_t(p.width);
    }
    bool ok = p.plane[0] && p.plane[1] && use(p);
    if (veil) CFRelease(veil);
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
    CFMutableDictionaryRef attributes = nullptr;
    VTDecompressionSessionRef session = nullptr;
    std::atomic<bool> broken{false};
    int renewals = 0;
    uint32_t backKeys = 0;
    std::mutex lock;
    std::map<int64_t, CVPixelBufferRef> ready;
    std::atomic<uint64_t> shown{0};
    std::atomic<int64_t> floor{0};
    bool keep = true;
    bool avc = false;
    bool resetNext = false;
    int64_t leadingBefore = INT64_MIN;
    uint32_t leadingUntil = 0;
    uint32_t from = kNone;
    H264Layout layout;
    uint32_t ahead = 8;
    uint32_t next = kNone;
    uint32_t last = kNone;
    int64_t kept = INT64_MIN;
    struct Job {
        std::vector<uint8_t> bytes;
        std::vector<uint8_t> mask;
        int64_t stamp = 0;
    };
    std::unique_ptr<SoftDecoder> softMask;
    std::vector<uint8_t> veil;
    int64_t veilStamp = INT64_MIN;

    bool decodeSoft(const Job& job) {
        veilStamp = INT64_MIN;
        if (softMask && !job.mask.empty()) {
            softMask->decode(job.mask.data(), job.mask.size(), job.stamp, [&](const Picture& m) {
                if (m.bits != 8 || m.interleaved || !m.plane[0] || m.width <= 0 || m.height <= 0) return;
                veil.resize(size_t(m.width) * size_t(m.height));
                for (int y = 0; y < m.height; ++y) memcpy(veil.data() + size_t(y) * size_t(m.width), m.plane[0] + size_t(y) * m.stride[0], size_t(m.width));
                veilStamp = m.stamp;
            });
        }
        return soft->decode(job.bytes.data(), job.bytes.size(), job.stamp, [&](const Picture& p) {
            Picture seen = p;
            if (veilStamp == p.stamp && veil.size() == size_t(p.width) * size_t(p.height)) {
                seen.alpha = veil.data();
                seen.alphaStride = size_t(p.width);
            }
            store(seen);
        });
    }
    std::condition_variable arrived, queued;
    std::deque<Job> jobs;
    bool busy = false;
    bool stopping = false;
    std::thread worker;
    uint64_t fed = 0;
    uint64_t restarts = 0;
    std::unique_ptr<SoftDecoder> soft;
    CVPixelBufferPoolRef pool = nullptr;
    OSType poolFormat = 0;
    int poolWidth = 0;
    int poolHeight = 0;
    StreamShape shape;
    const char* backend = "videotoolbox";
    struct Shown {
        uint32_t first = kNone;
        uint32_t second = kNone;
        bool firstTop = true;
    };
    std::vector<Shown> list;
    std::vector<uint32_t> keys;
    int concealing = 0;
    std::vector<bool> lost;

    Frame concealed(VideoDecoder& owner, uint32_t index) {
        uint32_t earlier = index;
        while (earlier > 0 && lost[earlier]) --earlier;
        const bool none = lost[earlier];
        if ((none && concealing) || concealing >= 30) return {};
        ++concealing;
        if (soft) next = kNone;
        Frame shown;
        if (!none) shown = owner.frame(earlier);
        for (uint32_t later = index + 1; none && !shown && later < list.size() && later <= index + 30; ++later) shown = owner.frame(later);
        --concealing;
        last = index;
        if (shown) {
            const Shown item = list[index];
            shown.pts = track->samples[item.first].pts;
            shown.duration = int64_t(track->samples[item.first].duration) + (item.second != kNone ? int64_t(track->samples[item.second].duration) : 0);
        }
        return shown;
    }
    std::mutex converting;
    std::vector<uint8_t> loose[3];
    std::vector<uint8_t> cover;

    void planar(CVImageBufferRef image, int64_t stamp) {
        std::lock_guard<std::mutex> hold(converting);
        if (CVPixelBufferLockBaseAddress(image, kCVPixelBufferLock_ReadOnly) != kCVReturnSuccess) return;
        Picture p;
        p.stamp = stamp;
        p.width = int(CVPixelBufferGetWidth(image));
        p.height = int(CVPixelBufferGetHeight(image));
        if (CVPixelBufferGetPlaneCount(image) == 3) {
            for (size_t i = 0; i < 3; ++i) {
                p.plane[i] = static_cast<const uint8_t*>(CVPixelBufferGetBaseAddressOfPlane(image, i));
                p.stride[i] = CVPixelBufferGetBytesPerRowOfPlane(image, i);
            }
            p.chromaWidth = int(CVPixelBufferGetWidthOfPlane(image, 1));
            p.chromaHeight = int(CVPixelBufferGetHeightOfPlane(image, 1));
        } else {
            const uint8_t* from = static_cast<const uint8_t*>(CVPixelBufferGetBaseAddress(image));
            const size_t stride = CVPixelBufferGetBytesPerRow(image);
            const size_t width = size_t(p.width), height = size_t(p.height), half = width / 2;
            if (CVPixelBufferGetPixelFormatType(image) == kCVPixelFormatType_4444AYpCbCr16) {
                for (auto& plane : loose) plane.resize(width * height * 2);
                cover.resize(width * height);
                for (size_t y = 0; from && y < height; ++y) {
                    const uint16_t* row = reinterpret_cast<const uint16_t*>(from + y * stride);
                    for (size_t x = 0; x < width; ++x) {
                        cover[y * width + x] = uint8_t(row[4 * x] >> 8);
                        for (size_t k = 0; k < 3; ++k) reinterpret_cast<uint16_t*>(loose[k].data())[y * width + x] = uint16_t(row[4 * x + 1 + k] >> 6);
                    }
                }
                for (int i = 0; i < 3; ++i) {
                    p.plane[i] = from ? loose[i].data() : nullptr;
                    p.stride[i] = width * 2;
                }
                p.bits = 10;
                p.chromaWidth = p.width;
                p.chromaHeight = p.height;
                p.alpha = cover.data();
                p.alphaStride = width;
                if (p.plane[0]) store(p, image);
                CVPixelBufferUnlockBaseAddress(image, kCVPixelBufferLock_ReadOnly);
                return;
            }
            loose[0].resize(width * height);
            loose[1].resize(half * height);
            loose[2].resize(half * height);
            for (size_t y = 0; from && y < height; ++y) {
                const uint8_t* row = from + y * stride;
                for (size_t x = 0; x < width; ++x) loose[0][y * width + x] = row[2 * x + 1];
                for (size_t x = 0; x < half; ++x) {
                    loose[1][y * half + x] = row[4 * x];
                    loose[2][y * half + x] = row[4 * x + 2];
                }
            }
            for (int i = 0; i < 3; ++i) {
                p.plane[i] = from ? loose[i].data() : nullptr;
                p.stride[i] = i ? half : width;
            }
            p.chromaWidth = int(half);
            p.chromaHeight = p.height;
        }
        if (p.plane[0] && p.plane[1] && p.plane[2] && p.chromaWidth > 0) store(p, image);
        CVPixelBufferUnlockBaseAddress(image, kCVPixelBufferLock_ReadOnly);
    }

    void pairFields(const H264Layout& layout) {
        const auto& samples = track->samples;
        std::vector<uint8_t> kind(samples.size(), 0);
        std::vector<uint32_t> number(samples.size(), 0);
        std::vector<uint8_t> bytes;
        bool fields = false;
        for (uint32_t i = 0; i < samples.size(); ++i) {
            if (i == 256 && !fields) return;
            bytes.resize(samples[i].size);
            if (file->read(*track, i, bytes.data())) kind[i] = uint8_t(h264FieldOf(layout, bytes.data(), bytes.size(), number[i]));
            fields = fields || kind[i];
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

    bool renew() {
        broken = false;
        if (!format || !attributes) return false;
        if (session) {
            VTDecompressionSessionInvalidate(session);
            CFRelease(session);
            session = nullptr;
        }
        clear();
        VTDecompressionOutputCallbackRecord callback{output, this};
        if (VTDecompressionSessionCreate(nullptr, format, nullptr, attributes, &callback, &session) == noErr) return true;
        session = nullptr;
        return false;
    }

    ~Impl() {
        if (worker.joinable()) {
            {
                std::lock_guard<std::mutex> hold(lock);
                stopping = true;
            }
            queued.notify_all();
            worker.join();
        }
        if (session) {
            VTDecompressionSessionWaitForAsynchronousFrames(session);
            VTDecompressionSessionInvalidate(session);
            CFRelease(session);
        }
        clear();
        if (format) CFRelease(format);
        if (attributes) CFRelease(attributes);
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
        if (soft) {
            std::unique_lock<std::mutex> hold(lock);
            jobs.clear();
            arrived.wait(hold, [&] { return !busy; });
            hold.unlock();
            soft->reset();
            if (softMask) softMask->reset();
        }
        clear();
        kept = INT64_MIN;
        resetNext = avc;
        leadingUntil = 0;
    }

    void store(const Picture& p, CVBufferRef labels = nullptr) {
        ++shown;
        if (p.color.known) {
            std::lock_guard<std::mutex> hold(lock);
            if (!shape.color.known || p.color.matrix == 0) shape.color = p.color;
        }
        if (!keep || p.stamp < floor) return;
        CVPixelBufferRef buffer = surfaceFor(p);
        if (!buffer) return;
        if (labels) CVBufferPropagateAttachments(labels, buffer);
        if (p.alpha) {
            CFMutableDataRef kept = CFDataCreateMutable(nullptr, 0);
            for (int y = 0; y < p.height; ++y) CFDataAppendBytes(kept, p.alpha + size_t(y) * p.alphaStride, p.width);
            CVBufferSetAttachment(buffer, kAlphaKey, kept, kCVAttachmentMode_ShouldNotPropagate);
            CFRelease(kept);
        }
        {
            std::lock_guard<std::mutex> hold(lock);
            auto found = ready.find(p.stamp);
            if (found != ready.end()) CFRelease(found->second);
            ready[p.stamp] = buffer;
        }
        arrived.notify_all();
    }

    void work() {
        std::unique_lock<std::mutex> hold(lock);
        for (;;) {
            queued.wait(hold, [&] { return stopping || !jobs.empty(); });
            if (stopping) return;
            Job job = std::move(jobs.front());
            jobs.pop_front();
            busy = true;
            hold.unlock();
            decodeSoft(job);
            hold.lock();
            busy = false;
            arrived.notify_all();
        }
    }

    void settle(int64_t pts, int64_t later) {
        if (!soft) {
            wait();
            if (!has(pts) || !has(later)) finishDelayed();
            return;
        }
        std::unique_lock<std::mutex> hold(lock);
        arrived.wait(hold, [&] { return (ready.count(pts) && ready.count(later)) || (jobs.empty() && !busy); });
        if (ready.count(pts) && ready.count(later)) return;
        hold.unlock();
        if (soft->drainKeepsState()) {
            soft->drain([&](const Picture& p) { store(p); });
            return;
        }
        for (int extra = 0; extra < 16 && next != kNone && next < track->samples.size(); ++extra) {
            feedSoft(next++, false);
            if (has(pts) && has(later)) return;
        }
        soft->drain([&](const Picture& p) { store(p); });
        soft->reset();
        next = kNone;
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
        if (p.chromaWidth < 1 || p.chromaHeight < 1 || int(CVPixelBufferGetWidthOfPlane(buffer, 1)) < p.chromaWidth || int(CVPixelBufferGetHeightOfPlane(buffer, 1)) < p.chromaHeight) {
            CFRelease(buffer);
            return nullptr;
        }
        fill(p, buffer);
        return buffer;
    }

    bool feedSoft(uint32_t index, bool queue) {
        const Sample& s = track->samples[index];
        Job job;
        job.bytes.resize(s.size);
        job.stamp = s.pts;
        if (!file->read(*track, index, job.bytes.data())) return false;
        if (softMask) file->readAlpha(*track, index, job.mask);
        ++fed;
        if (!queue) return decodeSoft(job);
        if (!worker.joinable()) worker = std::thread([this] { work(); });
        {
            std::lock_guard<std::mutex> hold(lock);
            jobs.push_back(std::move(job));
        }
        queued.notify_one();
        return true;
    }

    void clear() {
        std::lock_guard<std::mutex> hold(lock);
        for (auto& item : ready) CFRelease(item.second);
        ready.clear();
    }

    static void output(void* self, void*, OSStatus status, VTDecodeInfoFlags, CVImageBufferRef image, CMTime pts, CMTime) {
        auto* d = static_cast<Impl*>(self);
        if (status == kVTInvalidSessionErr || status == kVTVideoDecoderMalfunctionErr) d->broken = true;
        if (status != noErr || !image) return;
        if (CVPixelBufferGetPlaneCount(image) != 2) {
            d->planar(image, pts.value);
            return;
        }
        ++d->shown;
        if (!d->keep || pts.value < d->floor) return;
        std::lock_guard<std::mutex> hold(d->lock);
        auto found = d->ready.find(pts.value);
        if (found != d->ready.end()) CFRelease(found->second);
        d->ready[pts.value] = CVPixelBufferRetain(image);
    }

    bool feed(uint32_t index, bool async) {
        const Sample& s = track->samples[index];
        if (!s.size) {
            ++fed;
            return true;
        }
        if (soft) return feedSoft(index, keep);
        if (!session) return false;
        if (index < leadingUntil && s.pts < leadingBefore && s.duration) {
            ++fed;
            return true;
        }
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
        if (ok && resetNext) {
            CMSetAttachment(sample, kCMSampleBufferAttachmentKey_ResetDecoderBeforeDecoding, kCFBooleanTrue, kCMAttachmentMode_ShouldNotPropagate);
            resetNext = false;
        }
        if (ok) {
            VTDecodeFrameFlags flags = async ? kVTDecodeFrame_EnableAsynchronousDecompression : 0;
            OSStatus status = VTDecompressionSessionDecodeFrame(session, sample, flags, nullptr, nullptr);
            if (status == kVTInvalidSessionErr || status == kVTVideoDecoderMalfunctionErr) broken = true;
            ok = status == noErr;
            ++fed;
        }
        if (sample) CFRelease(sample);
        CFRelease(block);
        return ok;
    }

    bool intra(uint32_t index) const {
        std::vector<uint8_t> bytes(track->samples[index].size);
        return file->read(*track, index, bytes.data()) && h264Intra(layout, bytes.data(), bytes.size());
    }

    uint32_t startFor(uint32_t key, bool& clean) const {
        clean = !avc || intra(key);
        auto it = std::lower_bound(keys.begin(), keys.end(), key);
        for (int back = 0; !clean && back < 2 && it != keys.begin(); ++back) {
            key = *--it;
            clean = intra(key);
        }
        return key;
    }

    void prime() {
        resetNext = !keys.empty() && intra(keys.front());
        if (resetNext) feed(keys.front(), true);
        resetNext = false;
    }

    uint32_t keyFor(uint32_t target) const {
        const auto& syncs = keys;
        auto it = std::upper_bound(syncs.begin(), syncs.end(), target);
        while (it != syncs.begin()) {
            --it;
            if (track->samples[*it].pts <= track->samples[target].pts) {
                for (uint32_t back = backKeys; back && it != syncs.begin(); --back) --it;
                return *it;
            }
        }
        return syncs.empty() || syncs.front() > target ? 0 : syncs.front();
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
    if (track.samples.empty() || !track.timescale || track.timescale > uint32_t(INT32_MAX) || track.width <= 0 || track.height <= 0) return fail("track is empty");
    const uint32_t codec = track.codec;
    const bool vc1 = vc1Codec(codec) && track.configType == fourcc("bmih");
    const bool msmpeg4 = msmpeg4Codec(codec);
    if (const char* name = windowsMediaName(track.codec); name && !vc1 && !msmpeg4) {
        if (error) *error = std::string("this video codec (") + name + ") is not decoded yet";
        return nullptr;
    }
    const bool vp9 = codec == fourcc("vp09"), vp8 = codec == fourcc("vp08"), av1 = codec == fourcc("av01");
    const bool prores = codec == fourcc("apch") || codec == fourcc("apcn") || codec == fourcc("apcs") || codec == fourcc("apco") || codec == fourcc("ap4h") || codec == fourcc("ap4x");
    const bool h264 = track.configType == fourcc("avcC"), hevc = track.configType == fourcc("hvcC");
    const bool apv = codec == fourcc("apv1");
    const bool part2 = codec == fourcc("mp4v") && track.configType == fourcc("esds");
    const bool jpeg = codec == fourcc("jpeg");
    const bool tape = (codec >> 16) == 0x6476;
    const bool older = jpeg || tape || codec == fourcc("mp1v") || codec == fourcc("mp2v") || codec == fourcc("h263");
    const bool own = ownVideoName(codec) != nullptr;
    if (!h264 && !hevc && !vp9 && !vp8 && !av1 && !prores && !apv && !part2 && !older && !vc1 && !msmpeg4 && !own) return fail("video codec not supported by this decoder");
    if ((h264 || hevc) && track.config.empty()) return fail("track is empty");

    std::unique_ptr<VideoDecoder> decoder(new VideoDecoder);
    Impl& d = *decoder->impl;
    d.file = &file;
    d.track = &track;
    d.avc = h264;
    d.shape.width = track.width;
    d.shape.height = track.height;
    if (uint64_t(track.width) * uint64_t(track.height) > 2304u * 1296u) d.ahead = 4;

    std::vector<uint8_t> config = track.config;
    if (prores) {
        d.shape.bits = 10;
        d.shape.chromaY = 0;
        d.shape.chromaX = codec == fourcc("ap4h") || codec == fourcc("ap4x") ? 0 : 1;
    } else if (apv) {
        const bool described = track.configType == fourcc("apvC") && config.size() >= 22;
        const int chroma = described ? config[20] >> 4 : 2;
        d.shape.bits = described ? 8 + (config[20] & 15) : 10;
        d.shape.chromaX = chroma == 2 ? 1 : 0;
        d.shape.chromaY = 0;
        if (chroma != 2 && chroma != 3) return fail("this APV picture format is not supported");
    } else if (jpeg) {
        std::vector<uint8_t> first(track.samples[0].size);
        if (!file.read(track, 0, first.data())) return fail("cannot read the first picture");
        d.shape.color.full = true;
        d.shape.chromaX = d.shape.chromaY = 0;
        for (size_t i = 2; i + 3 < first.size() && first[i] == 0xFF;) {
            const size_t length = (size_t(first[i + 2]) << 8) | first[i + 3];
            if (first[i + 1] >= 0xC0 && first[i + 1] <= 0xC2 && i + 14 < first.size() && first[i + 9] >= 3) {
                d.shape.chromaX = (first[i + 11] >> 4) == 2 * (first[i + 14] >> 4) ? 1 : 0;
                d.shape.chromaY = (first[i + 11] & 15) == 2 * (first[i + 14] & 15) ? 1 : 0;
                break;
            }
            if (first[i + 1] == 0xDA) break;
            i += 2 + length;
        }
    } else if (part2 || older) {
        d.shape.chromaY = (tape && codec != fourcc("dvcp")) || ((codec == fourcc("mp1v") || codec == fourcc("mp2v")) && mpegChroma(file, track) != 1) ? 0 : 1;
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
    if (h264 && h264Layout(config, d.layout) && d.layout.fields) d.pairFields(d.layout);
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
        if (track.alpha && (vp8 || vp9)) d.softMask = openVpxDecoder(vp8, nullptr);
        d.soft = own ? openOwnVideo(track, error) : jpeg ? openStillDecoder(error) : apv ? openApvDecoder(error) : av1 ? openDav1d(int(d.ahead), error) : openVpxDecoder(vp8, error);
        if (!d.soft) return nullptr;
        if (av1) d.ahead += d.ahead / 2;
        if (jpeg) {
            d.shape.color = Color();
            d.shape.color.known = true;
        }
        d.backend = d.soft->name();
        return std::move(decoder);
    };
    auto ownH264 = [&]() -> std::unique_ptr<VideoDecoder> {
        d.avc = false;
        uint32_t led = 0;
        d.soft = openH264(config, error, [&d, &led](std::vector<uint8_t>& bytes) {
            if (led >= d.track->samples.size()) return false;
            bytes.resize(d.track->samples[led].size);
            return d.file->read(*d.track, led++, bytes.data());
        });
        if (!d.soft) return nullptr;
        d.backend = d.soft->name();
        return std::move(decoder);
    };
    if (h264) {
        const VideoInfo info = videoInfo(track.configType, config);
        if (software || std::getenv("FFAMP_OWN_H264") || info.chroma != 1 || info.bitDepth > 8 || info.bitDepthChroma > 8) return ownH264();
    }
    if (own || vp8 || apv || (vp9 && track.alpha) || (software && (vp9 || av1)) || (jpeg && !(d.shape.chromaX && d.shape.chromaY))) return softly();
    if (vc1 || msmpeg4) {
        d.soft = vc1 ? openVc1(codec, config, track.width, track.height, error) : openMsmpeg4(codec, config, track.width, track.height, error);
        if (!d.soft) return nullptr;
        d.backend = d.soft->name();
        return decoder;
    }
    if (part2 && (d.soft = openXvid(config, track.width, track.height))) {
        d.backend = d.soft->name();
        return decoder;
    }

    CMVideoCodecType type = h264 ? kCMVideoCodecType_H264 : hevc ? kCMVideoCodecType_HEVC : vp9 ? kCMVideoCodecType_VP9 : av1 ? kCMVideoCodecType_AV1 : codec;
    if (vp9 || av1) VTRegisterSupplementalVideoDecoderIfAvailable(type);
    CFMutableDictionaryRef extensions = dictionary();
    if (!prores && !older) {
        CFMutableDictionaryRef atoms = dictionary();
        CFDataRef data = CFDataCreate(nullptr, config.data(), CFIndex(config.size()));
        CFDictionarySetValue(atoms, h264 ? CFSTR("avcC") : hevc ? CFSTR("hvcC") : vp9 ? CFSTR("vpcC") : part2 ? CFSTR("esds") : CFSTR("av1C"), data);
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
    bool veiled = false;
    if (codec == fourcc("ap4h") || codec == fourcc("ap4x")) {
        std::vector<uint8_t> first(track.samples[0].size);
        veiled = first.size() >= 26 && file.read(track, 0, first.data()) && (first[25] & 15);
    }
    const OSType asked = veiled ? kCVPixelFormatType_4444AYpCbCr16 : part2 || (older && !jpeg) ? (d.shape.chromaY ? kCVPixelFormatType_420YpCbCr8Planar : kCVPixelFormatType_422YpCbCr8) : pixelFormat(d.shape);
    number(picture, kCVPixelBufferPixelFormatTypeKey, int32_t(asked));
    CFMutableDictionaryRef surface = dictionary();
    CFDictionarySetValue(picture, kCVPixelBufferIOSurfacePropertiesKey, surface);
    CFRelease(surface);
    CFDictionarySetValue(picture, kCVPixelBufferMetalCompatibilityKey, kCFBooleanTrue);
    VTDecompressionOutputCallbackRecord callback{Impl::output, &d};
    status = VTDecompressionSessionCreate(nullptr, d.format, nullptr, picture, &callback, &d.session);
    d.attributes = picture;
    if (status != noErr) {
        d.session = nullptr;
        if (vp9 || av1) return softly();
        if (h264) return ownH264();
        if (part2) return fail("this MPEG-4 video uses Advanced Simple Profile tools (B-frames, quarter-pixel motion or custom matrices) that the system decoder cannot play");
        return fail("the system video decoder refused this stream");
    }
    return decoder;
}

const char* VideoDecoder::backend() const { return impl->backend; }
uint32_t VideoDecoder::frames() const { return uint32_t(impl->list.size()); }
int64_t VideoDecoder::pts(uint32_t index) const { return index < impl->list.size() ? impl->track->samples[impl->list[index].first].pts : 0; }
uint64_t VideoDecoder::fed() const { return impl->fed; }
uint64_t VideoDecoder::restarts() const { return impl->restarts; }

Frame VideoDecoder::frame(uint32_t index) {
    Impl& d = *impl;
    const Track& track = *d.track;
    if (index >= d.list.size()) return {};
    if (index < d.lost.size() && d.lost[index]) return d.concealed(*this, index);
    const Impl::Shown item = d.list[index];
    const bool pair = item.second != kNone && !(d.soft && d.soft->weavesFields());
    const uint32_t target = item.second != kNone ? item.second : item.first;
    const int64_t pts = track.samples[item.first].pts;
    const int64_t later = pair ? track.samples[target].pts : pts;
    d.keep = true;
    d.floor = std::min(pts, later);
    const bool following = d.last != kNone && index == d.last + 1;
    d.last = index;
    auto present = [&] { return d.has(pts) && d.has(later); };
    if (!present() && d.next != kNone && d.next > target && d.floor >= d.kept) d.settle(pts, later);
    const bool missed = !d.soft && following && d.next != kNone && d.next > target && d.floor >= d.kept;
    if (!present() && !missed) {
        bool clean = true;
        const uint32_t key = d.startFor(d.keyFor(item.first), clean);
        if (d.next == kNone || d.next < key || d.next > item.first || key < d.from) {
            d.restart();
            if (!clean) d.prime();
            d.next = key;
            d.from = key;
            if (d.avc && track.samples[key].pts <= pts) {
                const auto after = std::upper_bound(d.keys.begin(), d.keys.end(), key);
                d.leadingBefore = track.samples[key].pts;
                d.leadingUntil = after == d.keys.end() ? uint32_t(track.samples.size()) : *after;
            }
            ++d.restarts;
        }
        while (d.next <= target) {
            d.feed(d.next, true);
            ++d.next;
        }
    }
    if (following && d.next != kNone) {
        const uint64_t limit = std::min<uint64_t>(uint64_t(target) + d.ahead, track.samples.size() - 1);
        while (d.next <= limit) {
            d.feed(d.next, true);
            ++d.next;
        }
    }
    if (!present()) d.settle(pts, later);
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
    if (!buffer && d.soft && d.backKeys < 3 && !d.keys.empty() && d.keyFor(item.first) != d.keys.front()) {
        const uint32_t saved = d.backKeys++;
        d.next = kNone;
        Frame retried = frame(index);
        d.backKeys = saved;
        return retried;
    }
    if (!buffer && !d.soft && (d.broken || !d.session) && d.renewals < 3 && d.renew()) {
        ++d.renewals;
        d.next = kNone;
        d.lost.clear();
        return frame(index);
    }
    if (!buffer) {
        if (d.lost.size() < d.list.size()) d.lost.resize(d.list.size(), false);
        d.lost[index] = true;
        return d.concealed(*this, index);
    }
    d.renewals = 0;
    d.kept = d.floor;
    Frame frame;
    frame.native = std::shared_ptr<void>(buffer, [](void* p) { CFRelease(p); });
    frame.width = int(CVPixelBufferGetWidth(buffer));
    frame.height = int(CVPixelBufferGetHeight(buffer));
    frame.pts = pts;
    frame.duration = int64_t(track.samples[item.first].duration) + (item.second != kNone ? int64_t(track.samples[item.second].duration) : 0);
    Picture described;
    if (describe(buffer, described)) {
        frame.bits = described.bits;
        frame.chromaWidth = described.chromaWidth;
        frame.chromaHeight = described.chromaHeight;
    }
    {
        std::lock_guard<std::mutex> hold(d.lock);
        frame.color = d.shape.color;
    }
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
    if (d.soft) d.soft->drain([&](const Picture& p) { d.store(p); });
    d.next = kNone;
    d.last = kNone;
    return d.shown;
}

}
