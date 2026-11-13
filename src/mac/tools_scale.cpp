#include "tools_common.h"
#include <CoreVideo/CoreVideo.h>
#include <VideoToolbox/VideoToolbox.h>

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

void tag(CVPixelBufferRef buffer) {
    CVBufferSetAttachment(buffer, kCVImageBufferColorPrimariesKey, kCVImageBufferColorPrimaries_ITU_R_709_2, kCVAttachmentMode_ShouldPropagate);
    CVBufferSetAttachment(buffer, kCVImageBufferTransferFunctionKey, kCVImageBufferTransferFunction_ITU_R_709_2, kCVAttachmentMode_ShouldPropagate);
    CVBufferSetAttachment(buffer, kCVImageBufferYCbCrMatrixKey, kCVImageBufferYCbCrMatrix_ITU_R_709_2, kCVAttachmentMode_ShouldPropagate);
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
    bool present = false;
    const bool matrix = tagged(source, kCVImageBufferYCbCrMatrixKey, kCVImageBufferYCbCrMatrix_ITU_R_709_2, present);
    if (!present) tag(source);
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
