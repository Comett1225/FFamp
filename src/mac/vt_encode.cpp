#include "absent/media.h"
#include "soft_video.h"
#include <CoreMedia/CoreMedia.h>
#include <CoreVideo/CoreVideo.h>
#include <VideoToolbox/VideoToolbox.h>
#include <cstring>
#include <deque>
#include <mutex>

namespace absent {

namespace {

void number(CFMutableDictionaryRef dict, CFStringRef key, int32_t value) {
    CFNumberRef n = CFNumberCreate(nullptr, kCFNumberSInt32Type, &value);
    CFDictionarySetValue(dict, key, n);
    CFRelease(n);
}

void property(VTCompressionSessionRef session, CFStringRef key, int32_t value) {
    CFNumberRef n = CFNumberCreate(nullptr, kCFNumberSInt32Type, &value);
    VTSessionSetProperty(session, key, n);
    CFRelease(n);
}

void parameterSets(CMFormatDescriptionRef format, bool hevc, std::vector<uint8_t>& out) {
    if (hevc) return;
    size_t count = 0;
    int lengthSize = 4;
    const uint8_t* sps = nullptr;
    size_t spsSize = 0;
    if (CMVideoFormatDescriptionGetH264ParameterSetAtIndex(format, 0, &sps, &spsSize, &count, &lengthSize) != noErr || count < 2 || spsSize < 4) return;
    const uint8_t* pps = nullptr;
    size_t ppsSize = 0;
    if (CMVideoFormatDescriptionGetH264ParameterSetAtIndex(format, 1, &pps, &ppsSize, nullptr, nullptr) != noErr) return;
    out = {1, sps[1], sps[2], sps[3], uint8_t(0xFC | (lengthSize - 1)), 0xE1, uint8_t(spsSize >> 8), uint8_t(spsSize)};
    out.insert(out.end(), sps, sps + spsSize);
    out.push_back(1);
    out.push_back(uint8_t(ppsSize >> 8));
    out.push_back(uint8_t(ppsSize));
    out.insert(out.end(), pps, pps + ppsSize);
}

}

struct VideoEncoder::Impl {
    EncodeSpec spec;
    VTCompressionSessionRef session = nullptr;
    mutable std::mutex lock;
    std::deque<Packet> packets;
    std::vector<uint8_t> config;
    bool failed = false;
    std::unique_ptr<SoftEncoder> soft;
    std::vector<uint8_t> planar;
    const char* backend = "videotoolbox";

    PacketSink keep() {
        return [this](Packet&& packet) {
            std::lock_guard<std::mutex> hold(lock);
            packets.push_back(std::move(packet));
        };
    }

    bool softPlanar(const uint8_t* data, int64_t pts, int64_t duration) {
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

    bool planarFrame(const uint8_t* data, int64_t pts, int64_t duration) { return soft ? softPlanar(data, pts, duration) : submitI420(data, pts, duration); }

    bool bgraFrame(const uint8_t* pixels, size_t stride, int64_t pts, int64_t duration, bool redFirst) {
        planar.resize(size_t(spec.width) * size_t(spec.height) + 2 * size_t((spec.width + 1) / 2) * size_t((spec.height + 1) / 2));
        bgraToI420(pixels, stride, spec.width, spec.height, planar.data(), redFirst);
        return planarFrame(planar.data(), pts, duration);
    }

    bool bufferFrame(CVPixelBufferRef buffer, int64_t pts, int64_t duration) {
        if (CVPixelBufferGetPixelFormatType(buffer) != kCVPixelFormatType_32BGRA) return soft ? softBuffer(buffer, pts, duration) : submit(buffer, pts, duration);
        if (int(CVPixelBufferGetWidth(buffer)) != spec.width || int(CVPixelBufferGetHeight(buffer)) != spec.height) return false;
        if (CVPixelBufferLockBaseAddress(buffer, kCVPixelBufferLock_ReadOnly) != kCVReturnSuccess) return false;
        const bool ok = bgraFrame(static_cast<const uint8_t*>(CVPixelBufferGetBaseAddress(buffer)), CVPixelBufferGetBytesPerRow(buffer), pts, duration, false);
        CVPixelBufferUnlockBaseAddress(buffer, kCVPixelBufferLock_ReadOnly);
        return ok;
    }

    bool softBuffer(CVPixelBufferRef buffer, int64_t pts, int64_t duration) {
        if (int(CVPixelBufferGetWidth(buffer)) != spec.width || int(CVPixelBufferGetHeight(buffer)) != spec.height) return false;
        bool fits = withPixelBuffer(buffer, [&](const Picture& p) {
            pictureToI420(p, planar);
            return true;
        });
        return fits && softPlanar(planar.data(), pts, duration);
    }

    ~Impl() {
        if (session) {
            VTCompressionSessionInvalidate(session);
            CFRelease(session);
        }
    }

    static void output(void* self, void*, OSStatus status, VTEncodeInfoFlags, CMSampleBufferRef sample) {
        auto* e = static_cast<Impl*>(self);
        if (status != noErr || !sample) {
            std::lock_guard<std::mutex> hold(e->lock);
            e->failed = true;
            return;
        }
        CMBlockBufferRef block = CMSampleBufferGetDataBuffer(sample);
        Packet packet;
        if (block) packet.data.resize(CMBlockBufferGetDataLength(block));
        if (!block || CMBlockBufferCopyDataBytes(block, 0, packet.data.size(), packet.data.data()) != noErr) {
            std::lock_guard<std::mutex> hold(e->lock);
            e->failed = true;
            return;
        }
        CMTime pts = CMSampleBufferGetPresentationTimeStamp(sample);
        CMTime dts = CMSampleBufferGetDecodeTimeStamp(sample);
        CMTime duration = CMSampleBufferGetDuration(sample);
        packet.pts = CMTimeConvertScale(pts, int32_t(e->spec.timescale), kCMTimeRoundingMethod_RoundHalfAwayFromZero).value;
        packet.dts = CMTIME_IS_VALID(dts) ? CMTimeConvertScale(dts, int32_t(e->spec.timescale), kCMTimeRoundingMethod_RoundHalfAwayFromZero).value : packet.pts;
        packet.duration = CMTIME_IS_VALID(duration) ? uint32_t(CMTimeConvertScale(duration, int32_t(e->spec.timescale), kCMTimeRoundingMethod_RoundHalfAwayFromZero).value) : 0;
        packet.sync = true;
        if (CFArrayRef list = CMSampleBufferGetSampleAttachmentsArray(sample, false)) {
            if (CFArrayGetCount(list) > 0) {
                auto dict = static_cast<CFDictionaryRef>(CFArrayGetValueAtIndex(list, 0));
                packet.sync = !CFDictionaryContainsKey(dict, kCMSampleAttachmentKey_NotSync) || CFDictionaryGetValue(dict, kCMSampleAttachmentKey_NotSync) != kCFBooleanTrue;
            }
        }
        std::lock_guard<std::mutex> hold(e->lock);
        if (e->config.empty()) {
            CMFormatDescriptionRef format = CMSampleBufferGetFormatDescription(sample);
            auto atoms = static_cast<CFDictionaryRef>(CMFormatDescriptionGetExtension(format, kCMFormatDescriptionExtension_SampleDescriptionExtensionAtoms));
            if (atoms && CFGetTypeID(atoms) == CFDictionaryGetTypeID()) {
                auto data = static_cast<CFDataRef>(CFDictionaryGetValue(atoms, e->spec.hevc ? CFSTR("hvcC") : CFSTR("avcC")));
                if (data && CFGetTypeID(data) == CFDataGetTypeID()) e->config.assign(CFDataGetBytePtr(data), CFDataGetBytePtr(data) + CFDataGetLength(data));
            }
            if (e->config.empty()) parameterSets(format, e->spec.hevc, e->config);
        }
        e->packets.push_back(std::move(packet));
    }

    bool submit(CVPixelBufferRef buffer, int64_t pts, int64_t duration) {
        OSStatus status = VTCompressionSessionEncodeFrame(session, buffer, CMTimeMake(pts, int32_t(spec.timescale)), CMTimeMake(duration, int32_t(spec.timescale)), nullptr, nullptr, nullptr);
        return status == noErr;
    }

    bool submitI420(const uint8_t* data, int64_t pts, int64_t duration) {
        CVPixelBufferPoolRef pool = VTCompressionSessionGetPixelBufferPool(session);
        CVPixelBufferRef buffer = nullptr;
        if (!pool || CVPixelBufferPoolCreatePixelBuffer(nullptr, pool, &buffer) != kCVReturnSuccess) return false;
        CVPixelBufferLockBaseAddress(buffer, 0);
        const size_t w = size_t(spec.width), h = size_t(spec.height);
        const size_t cw = (w + 1) / 2, ch = (h + 1) / 2;
        uint8_t* y = static_cast<uint8_t*>(CVPixelBufferGetBaseAddressOfPlane(buffer, 0));
        const size_t yStride = CVPixelBufferGetBytesPerRowOfPlane(buffer, 0);
        for (size_t row = 0; row < h; ++row) memcpy(y + row * yStride, data + row * w, w);
        uint8_t* uv = static_cast<uint8_t*>(CVPixelBufferGetBaseAddressOfPlane(buffer, 1));
        const size_t uvStride = CVPixelBufferGetBytesPerRowOfPlane(buffer, 1);
        const uint8_t* u = data + w * h;
        const uint8_t* v = u + cw * ch;
        for (size_t row = 0; row < ch; ++row) {
            uint8_t* line = uv + row * uvStride;
            for (size_t x = 0; x < cw; ++x) {
                line[2 * x] = u[row * cw + x];
                line[2 * x + 1] = v[row * cw + x];
            }
        }
        CVPixelBufferUnlockBaseAddress(buffer, 0);
        bool ok = submit(buffer, pts, duration);
        CFRelease(buffer);
        return ok;
    }
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
    if (e.spec.codec == fourcc("av01") || e.spec.codec == fourcc("vp09") || e.spec.codec == fourcc("apv1")) {
        e.soft = e.spec.codec == fourcc("av01") ? openSvtAv1(e.spec, error) : e.spec.codec == fourcc("apv1") ? openApvEncoder(e.spec, error) : openVpxEncoder(e.spec, error);
        if (!e.soft) return nullptr;
        e.backend = e.soft->name();
        e.config = e.soft->config();
        return encoder;
    }
    if (!e.spec.hevc && e.spec.codec != fourcc("avc1")) return fail("video codec not supported by this encoder");
    CFMutableDictionaryRef source = CFDictionaryCreateMutable(nullptr, 0, &kCFTypeDictionaryKeyCallBacks, &kCFTypeDictionaryValueCallBacks);
    number(source, kCVPixelBufferPixelFormatTypeKey, kCVPixelFormatType_420YpCbCr8BiPlanarVideoRange);
    number(source, kCVPixelBufferWidthKey, spec.width);
    number(source, kCVPixelBufferHeightKey, spec.height);
    CFMutableDictionaryRef surface = CFDictionaryCreateMutable(nullptr, 0, &kCFTypeDictionaryKeyCallBacks, &kCFTypeDictionaryValueCallBacks);
    CFDictionarySetValue(source, kCVPixelBufferIOSurfacePropertiesKey, surface);
    CFRelease(surface);
    OSStatus status = VTCompressionSessionCreate(nullptr, spec.width, spec.height, e.spec.hevc ? kCMVideoCodecType_HEVC : kCMVideoCodecType_H264,
        nullptr, source, nullptr, Impl::output, &e, &e.session);
    CFRelease(source);
    if (status != noErr) return fail("the system video encoder is not available for this size or codec");
    VTSessionSetProperty(e.session, kVTCompressionPropertyKey_RealTime, kCFBooleanFalse);
    VTSessionSetProperty(e.session, kVTCompressionPropertyKey_ProfileLevel, e.spec.hevc ? kVTProfileLevel_HEVC_Main_AutoLevel : kVTProfileLevel_H264_High_AutoLevel);
    VTSessionSetProperty(e.session, kVTCompressionPropertyKey_AllowFrameReordering, spec.reorder ? kCFBooleanTrue : kCFBooleanFalse);
    property(e.session, kVTCompressionPropertyKey_AverageBitRate, spec.bitrate);
    property(e.session, kVTCompressionPropertyKey_MaxKeyFrameInterval, spec.keyInterval);
    property(e.session, kVTCompressionPropertyKey_ExpectedFrameRate, int32_t(spec.fps + 0.5));
    VTSessionSetProperty(e.session, kVTCompressionPropertyKey_ColorPrimaries, kCVImageBufferColorPrimaries_ITU_R_709_2);
    VTSessionSetProperty(e.session, kVTCompressionPropertyKey_TransferFunction, kCVImageBufferTransferFunction_ITU_R_709_2);
    VTSessionSetProperty(e.session, kVTCompressionPropertyKey_YCbCrMatrix, kCVImageBufferYCbCrMatrix_ITU_R_709_2);
    VTCompressionSessionPrepareToEncodeFrames(e.session);
    return encoder;
}

bool VideoEncoder::encodeI420(const uint8_t* data, int64_t pts, int64_t duration) { return !impl->spec.bgra && impl->planarFrame(data, pts, duration); }

bool VideoEncoder::encodeBGRA(const uint8_t* data, int64_t pts, int64_t duration, bool redFirst) { return impl->bgraFrame(data, size_t(impl->spec.width) * 4, pts, duration, redFirst); }

bool VideoEncoder::encode(const Frame& frame, int64_t pts, int64_t duration) { return encodePixelBuffer(frame.native.get(), pts, duration); }

bool VideoEncoder::encodePixelBuffer(void* pixelBuffer, int64_t pts, int64_t duration) {
    auto buffer = static_cast<CVPixelBufferRef>(pixelBuffer);
    return buffer && impl->bufferFrame(buffer, pts, duration);
}

bool VideoEncoder::encodeSurface(void* surface, int64_t pts, int64_t duration) {
    CVPixelBufferRef buffer = nullptr;
    if (!surface || CVPixelBufferCreateWithIOSurface(nullptr, static_cast<IOSurfaceRef>(surface), nullptr, &buffer) != kCVReturnSuccess) return false;
    bool ok = encodePixelBuffer(buffer, pts, duration);
    CFRelease(buffer);
    return ok;
}

const char* VideoEncoder::backend() const { return impl->backend; }

bool VideoEncoder::finish() {
    if (impl->soft) return impl->soft->finish(impl->keep());
    OSStatus status = VTCompressionSessionCompleteFrames(impl->session, kCMTimeInvalid);
    std::lock_guard<std::mutex> hold(impl->lock);
    return status == noErr && !impl->failed;
}

bool VideoEncoder::take(Packet& out) {
    std::lock_guard<std::mutex> hold(impl->lock);
    if (impl->packets.empty()) return false;
    out = std::move(impl->packets.front());
    impl->packets.pop_front();
    return true;
}

uint32_t VideoEncoder::configType() const { return impl->soft ? impl->soft->configType() : impl->spec.hevc ? fourcc("hvcC") : fourcc("avcC"); }

std::vector<uint8_t> VideoEncoder::config() const {
    std::lock_guard<std::mutex> hold(impl->lock);
    return impl->config;
}

}
