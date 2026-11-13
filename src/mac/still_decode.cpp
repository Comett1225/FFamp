#include "../soft_video.h"
#include <CoreGraphics/CoreGraphics.h>
#include <ImageIO/ImageIO.h>

namespace absent {

namespace {

struct StillDecoder : SoftDecoder {
    std::vector<uint8_t> pixels;
    std::vector<uint8_t> planes;

    const char* name() const override { return "imageio"; }
    void reset() override {}

    bool decode(const uint8_t* data, size_t size, int64_t stamp, const PictureSink& sink) override {
        CFDataRef bytes = CFDataCreateWithBytesNoCopy(nullptr, data, CFIndex(size), kCFAllocatorNull);
        CGImageSourceRef source = bytes ? CGImageSourceCreateWithData(bytes, nullptr) : nullptr;
        CGImageRef image = source ? CGImageSourceCreateImageAtIndex(source, 0, nullptr) : nullptr;
        bool done = false;
        const size_t width = image ? CGImageGetWidth(image) : 0, height = image ? CGImageGetHeight(image) : 0;
        if (width > 0 && height > 0 && width <= 32768 && height <= 32768) {
            pixels.resize(width * height * 4);
            CGColorSpaceRef space = CGColorSpaceCreateWithName(kCGColorSpaceSRGB);
            CGContextRef context = CGBitmapContextCreate(pixels.data(), width, height, 8, width * 4, space, uint32_t(kCGImageAlphaNoneSkipFirst) | uint32_t(kCGBitmapByteOrder32Little));
            if (context) {
                CGContextSetInterpolationQuality(context, kCGInterpolationNone);
                CGContextDrawImage(context, CGRectMake(0, 0, CGFloat(width), CGFloat(height)), image);
                CGContextRelease(context);
                const size_t count = width * height;
                planes.resize(count * 3);
                uint8_t* luma = planes.data();
                uint8_t* blue = luma + count;
                uint8_t* red = blue + count;
                const uint8_t* from = pixels.data();
                for (size_t i = 0; i < count; ++i, from += 4) {
                    const int b = from[0], g = from[1], r = from[2];
                    luma[i] = uint8_t((11966 * r + 40254 * g + 4064 * b + (16 << 16) + 32768) >> 16);
                    blue[i] = uint8_t((-6596 * r - 22188 * g + 28784 * b + (128 << 16) + 32768) >> 16);
                    red[i] = uint8_t((28784 * r - 26145 * g - 2639 * b + (128 << 16) + 32768) >> 16);
                }
                Picture p;
                p.stamp = stamp;
                p.width = p.chromaWidth = int(width);
                p.height = p.chromaHeight = int(height);
                p.plane[0] = luma;
                p.plane[1] = blue;
                p.plane[2] = red;
                p.stride[0] = p.stride[1] = p.stride[2] = width;
                p.color.known = true;
                sink(p);
                done = true;
            }
            CGColorSpaceRelease(space);
        }
        if (image) CGImageRelease(image);
        if (source) CFRelease(source);
        if (bytes) CFRelease(bytes);
        return done;
    }
};

}

std::unique_ptr<SoftDecoder> openStillDecoder(std::string*) {
    return std::make_unique<StillDecoder>();
}

}
