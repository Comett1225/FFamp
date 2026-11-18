#include "tools_common.h"
#include <CoreVideo/CoreVideo.h>
#include <VideoToolbox/VideoToolbox.h>
#include <cstring>

namespace absent {

struct Scaler::Impl {
    int width = 0;
    int height = 0;
    VTPixelTransferSessionRef session = nullptr;
    CVPixelBufferPoolRef pool = nullptr;

    ~Impl() {
        if (session) {
            VTPixelTransferSessionInvalidate(session);
            CFRelease(session);
        }
        if (pool) CFRelease(pool);
    }
};

namespace {

void number(CFMutableDictionaryRef dict, CFStringRef key, int32_t value) {
    CFNumberRef n = CFNumberCreate(nullptr, kCFNumberSInt32Type, &value);
    CFDictionarySetValue(dict, key, n);
    CFRelease(n);
}

bool tagged(CVPixelBufferRef buffer, CFStringRef key, CFStringRef wanted, bool& present) {
    CFTypeRef value = CVBufferCopyAttachment(buffer, key, nullptr);
    present = value != nullptr;
    const bool same = value && CFEqual(value, wanted);
    if (value) CFRelease(value);
    return same;
}

void tag(CVPixelBufferRef buffer, const Color& color = Color()) {
    const CFStringRef keys[3] = {kCVImageBufferColorPrimariesKey, kCVImageBufferTransferFunctionKey, kCVImageBufferYCbCrMatrixKey};
    CFStringRef values[3] = {kCVImageBufferColorPrimaries_ITU_R_709_2, kCVImageBufferTransferFunction_ITU_R_709_2, kCVImageBufferYCbCrMatrix_ITU_R_709_2};
    if (color.known) {
        if (CFStringRef named = CVColorPrimariesGetStringForIntegerCodePoint(color.primaries)) values[0] = named;
        if (CFStringRef named = CVTransferFunctionGetStringForIntegerCodePoint(color.transfer)) values[1] = named;
        if (CFStringRef named = CVYCbCrMatrixGetStringForIntegerCodePoint(color.matrix)) values[2] = named;
    }
    for (int i = 0; i < 3; ++i) {
        bool present = false;
        tagged(buffer, keys[i], values[i], present);
        if (!present) CVBufferSetAttachment(buffer, keys[i], values[i], kCVAttachmentMode_ShouldPropagate);
    }
}

void* fromRgb(const Frame& frame, CVPixelBufferPoolRef pool, VTPixelTransferSessionRef session) {
    std::vector<uint8_t> pixels;
    if (!copyBGRA(frame, pixels)) return nullptr;
    CVPixelBufferRef rgb = nullptr;
    if (CVPixelBufferCreate(nullptr, size_t(frame.width), size_t(frame.height), kCVPixelFormatType_32BGRA, nullptr, &rgb) != kCVReturnSuccess) return nullptr;
    CVPixelBufferLockBaseAddress(rgb, 0);
    uint8_t* to = static_cast<uint8_t*>(CVPixelBufferGetBaseAddress(rgb));
    const size_t stride = CVPixelBufferGetBytesPerRow(rgb), row = size_t(frame.width) * 4;
    for (int y = 0; to && y < frame.height; ++y) memcpy(to + size_t(y) * stride, pixels.data() + size_t(y) * row, row);
    CVPixelBufferUnlockBaseAddress(rgb, 0);
    tag(rgb, frame.color);
    CVBufferRemoveAttachment(rgb, kCVImageBufferYCbCrMatrixKey);
    CVPixelBufferRef target = nullptr;
    if (CVPixelBufferPoolCreatePixelBuffer(nullptr, pool, &target) == kCVReturnSuccess) {
        tag(target);
        if (VTPixelTransferSessionTransferImage(session, rgb, target) != noErr) {
            CFRelease(target);
            target = nullptr;
        }
    }
    CFRelease(rgb);
    return target;
}

}

Scaler::Scaler(int width, int height) : impl(new Impl) {
    impl->width = width;
    impl->height = height;
    CFMutableDictionaryRef wanted = CFDictionaryCreateMutable(nullptr, 0, &kCFTypeDictionaryKeyCallBacks, &kCFTypeDictionaryValueCallBacks);
    number(wanted, kCVPixelBufferPixelFormatTypeKey, kCVPixelFormatType_420YpCbCr8BiPlanarVideoRange);
    number(wanted, kCVPixelBufferWidthKey, width);
    number(wanted, kCVPixelBufferHeightKey, height);
    CFMutableDictionaryRef surface = CFDictionaryCreateMutable(nullptr, 0, &kCFTypeDictionaryKeyCallBacks, &kCFTypeDictionaryValueCallBacks);
    CFDictionarySetValue(wanted, kCVPixelBufferIOSurfacePropertiesKey, surface);
    CFRelease(surface);
    CVPixelBufferPoolCreate(nullptr, nullptr, wanted, &impl->pool);
    CFRelease(wanted);
    if (VTPixelTransferSessionCreate(nullptr, &impl->session) == noErr) {
        VTSessionSetProperty(impl->session, kVTPixelTransferPropertyKey_DestinationColorPrimaries, kCVImageBufferColorPrimaries_ITU_R_709_2);
        VTSessionSetProperty(impl->session, kVTPixelTransferPropertyKey_DestinationTransferFunction, kCVImageBufferTransferFunction_ITU_R_709_2);
        VTSessionSetProperty(impl->session, kVTPixelTransferPropertyKey_DestinationYCbCrMatrix, kCVImageBufferYCbCrMatrix_ITU_R_709_2);
        VTSessionSetProperty(impl->session, kVTPixelTransferPropertyKey_DownsamplingMode, kVTDownsamplingMode_Average);
    }
}

Scaler::~Scaler() = default;

void* Scaler::fit(const Frame& frame) {
    auto source = static_cast<CVPixelBufferRef>(frame.native.get());
    if (!source || !impl->pool || !impl->session) return nullptr;
    if (frame.color.known && frame.color.matrix == 0) return fromRgb(frame, impl->pool, impl->session);
    bool present = false;
    tagged(source, kCVImageBufferYCbCrMatrixKey, kCVImageBufferYCbCrMatrix_ITU_R_709_2, present);
    if (!present) tag(source, frame.color);
    const bool matrix = tagged(source, kCVImageBufferYCbCrMatrixKey, kCVImageBufferYCbCrMatrix_ITU_R_709_2, present);
    bool primariesPresent = false, transferPresent = false;
    const bool primaries = tagged(source, kCVImageBufferColorPrimariesKey, kCVImageBufferColorPrimaries_ITU_R_709_2, primariesPresent);
    const bool transfer = tagged(source, kCVImageBufferTransferFunctionKey, kCVImageBufferTransferFunction_ITU_R_709_2, transferPresent);
    const bool sameColour = (matrix || !present) && (primaries || !primariesPresent) && (transfer || !transferPresent);
    if (sameColour && int(CVPixelBufferGetWidth(source)) == impl->width && int(CVPixelBufferGetHeight(source)) == impl->height
        && CVPixelBufferGetPixelFormatType(source) == kCVPixelFormatType_420YpCbCr8BiPlanarVideoRange) {
        CFRetain(source);
        return source;
    }
    CVPixelBufferRef target = nullptr;
    if (CVPixelBufferPoolCreatePixelBuffer(nullptr, impl->pool, &target) != kCVReturnSuccess) return nullptr;
    tag(target);
    if (VTPixelTransferSessionTransferImage(impl->session, source, target) != noErr) {
        CFRelease(target);
        return nullptr;
    }
    return target;
}

void releaseBuffer(void* buffer) {
    if (buffer) CFRelease(static_cast<CFTypeRef>(buffer));
}

}
