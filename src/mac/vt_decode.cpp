#include "absent/media.h"
#include <CoreMedia/CoreMedia.h>
#include <CoreVideo/CoreVideo.h>
#include <VideoToolbox/VideoToolbox.h>
#include <algorithm>
#include <atomic>
#include <cstring>
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

}

bool copyI420(const Frame& frame, std::vector<uint8_t>& out) {
    auto buffer = static_cast<CVPixelBufferRef>(frame.native.get());
    if (!buffer || CVPixelBufferGetPlaneCount(buffer) != 2) return false;
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

    ~Impl() {
        if (session) {
            VTDecompressionSessionWaitForAsynchronousFrames(session);
            VTDecompressionSessionInvalidate(session);
            CFRelease(session);
        }
        clear();
        if (format) CFRelease(format);
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
        const auto& syncs = track->syncs;
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

std::unique_ptr<VideoDecoder> VideoDecoder::open(Reader& file, const Track& track, std::string* error) {
    auto fail = [&](const char* text) {
        if (error) *error = text;
        return nullptr;
    };
    CMVideoCodecType type;
    CFStringRef atom;
    if (track.configType == fourcc("avcC")) {
        type = kCMVideoCodecType_H264;
        atom = CFSTR("avcC");
    } else if (track.configType == fourcc("hvcC")) {
        type = kCMVideoCodecType_HEVC;
        atom = CFSTR("hvcC");
    } else {
        return fail("video codec not supported by this decoder");
    }
    if (track.config.empty() || track.samples.empty() || !track.timescale) return fail("track is empty");
    std::unique_ptr<VideoDecoder> decoder(new VideoDecoder);
    Impl& d = *decoder->impl;
    d.file = &file;
    d.track = &track;

    CFMutableDictionaryRef atoms = dictionary();
    CFDataRef config = CFDataCreate(nullptr, track.config.data(), CFIndex(track.config.size()));
    CFDictionarySetValue(atoms, atom, config);
    CFRelease(config);
    CFMutableDictionaryRef extensions = dictionary();
    CFDictionarySetValue(extensions, kCMFormatDescriptionExtension_SampleDescriptionExtensionAtoms, atoms);
    CFRelease(atoms);
    OSStatus status = CMVideoFormatDescriptionCreate(nullptr, type, track.width, track.height, extensions, &d.format);
    CFRelease(extensions);
    if (status != noErr) return fail("cannot describe the video stream");

    CFMutableDictionaryRef picture = dictionary();
    number(picture, kCVPixelBufferPixelFormatTypeKey, kCVPixelFormatType_420YpCbCr8BiPlanarVideoRange);
    CFMutableDictionaryRef surface = dictionary();
    CFDictionarySetValue(picture, kCVPixelBufferIOSurfacePropertiesKey, surface);
    CFRelease(surface);
    CFDictionarySetValue(picture, kCVPixelBufferMetalCompatibilityKey, kCFBooleanTrue);
    VTDecompressionOutputCallbackRecord callback{Impl::output, &d};
    status = VTDecompressionSessionCreate(nullptr, d.format, nullptr, picture, &callback, &d.session);
    CFRelease(picture);
    if (status != noErr) return fail("the system video decoder refused this stream");
    return decoder;
}

uint32_t VideoDecoder::frames() const { return uint32_t(impl->track->order.size()); }
uint64_t VideoDecoder::fed() const { return impl->fed; }
uint64_t VideoDecoder::restarts() const { return impl->restarts; }

Frame VideoDecoder::frame(uint32_t index) {
    Impl& d = *impl;
    const Track& track = *d.track;
    if (index >= track.order.size()) return {};
    const uint32_t target = track.order[index];
    const int64_t pts = track.samples[target].pts;
    d.keep = true;
    d.floor = pts;
    CVPixelBufferRef buffer = d.take(pts);
    if (!buffer) {
        const uint32_t key = d.keyFor(target);
        if (d.next == kNone || d.next < key || d.next > target) {
            VTDecompressionSessionWaitForAsynchronousFrames(d.session);
            d.clear();
            d.next = key;
            ++d.restarts;
        }
        while (d.next <= target) {
            if (!d.feed(d.next, true)) break;
            ++d.next;
        }
        VTDecompressionSessionWaitForAsynchronousFrames(d.session);
        buffer = d.take(pts);
        if (!buffer) {
            VTDecompressionSessionFinishDelayedFrames(d.session);
            VTDecompressionSessionWaitForAsynchronousFrames(d.session);
            buffer = d.take(pts);
        }
    }
    if (!buffer) return {};
    Frame frame;
    frame.native = std::shared_ptr<void>(buffer, [](void* p) { CFRelease(p); });
    frame.width = int(CVPixelBufferGetWidth(buffer));
    frame.height = int(CVPixelBufferGetHeight(buffer));
    frame.pts = pts;
    return frame;
}

uint64_t VideoDecoder::run(bool async) {
    Impl& d = *impl;
    VTDecompressionSessionWaitForAsynchronousFrames(d.session);
    d.clear();
    d.keep = false;
    d.shown = 0;
    for (uint32_t i = 0; i < d.track->samples.size(); ++i) d.feed(i, async);
    VTDecompressionSessionFinishDelayedFrames(d.session);
    VTDecompressionSessionWaitForAsynchronousFrames(d.session);
    d.next = kNone;
    return d.shown;
}

}
