#include "tools_common.h"
#include <CoreGraphics/CoreGraphics.h>
#include <ImageIO/ImageIO.h>
#include <cmath>
#include <cstdio>
#include <cstring>

namespace absent {

namespace {

const int64_t kMostPixels = 2048 * 2048;
const size_t kMostFrames = 3000;
const int64_t kMostWork = 600000000;
const int64_t kLongest = 10 * 60 * 1000;

struct Held {
    CFTypeRef p;
    explicit Held(CFTypeRef value) : p(value) {}
    Held(const Held&) = delete;
    ~Held() {
        if (p) CFRelease(p);
    }
};

bool fail(std::string* error, const char* text) {
    if (error) *error = text;
    return false;
}

struct Shape {
    int x = 0;
    int y = 0;
    int width = 0;
    int height = 0;
    bool clear = false;
    int dispose = 0;
};

bool skipBlocks(FILE* file) {
    for (;;) {
        const int size = fgetc(file);
        if (size < 0) return false;
        if (size == 0) return true;
        if (fseek(file, size, SEEK_CUR) != 0) return false;
    }
}

std::vector<Shape> shapesOf(const std::string& path) {
    std::vector<Shape> found;
    FILE* file = fopen(path.c_str(), "rb");
    if (!file) return found;
    uint8_t head[13];
    bool ok = fread(head, 1, sizeof head, file) == sizeof head;
    if (ok && (head[10] & 0x80)) ok = fseek(file, 3L << ((head[10] & 7) + 1), SEEK_CUR) == 0;
    Shape next;
    while (ok && found.size() <= kMostFrames) {
        const int kind = fgetc(file);
        if (kind == 0x21) {
            const int label = fgetc(file);
            if (label == 0xF9) {
                const int size = fgetc(file);
                uint8_t control[4];
                if (size < 4 || fread(control, 1, 4, file) != 4 || fseek(file, size - 4, SEEK_CUR) != 0) break;
                next.dispose = (control[0] >> 2) & 7;
                next.clear = control[0] & 1;
            }
            ok = label >= 0 && skipBlocks(file);
        } else if (kind == 0x2C) {
            uint8_t box[9];
            if (fread(box, 1, sizeof box, file) != sizeof box) break;
            next.x = box[0] | (box[1] << 8);
            next.y = box[2] | (box[3] << 8);
            next.width = box[4] | (box[5] << 8);
            next.height = box[6] | (box[7] << 8);
            if ((box[8] & 0x80) && fseek(file, 3L << ((box[8] & 7) + 1), SEEK_CUR) != 0) break;
            found.push_back(next);
            next = Shape();
            ok = fgetc(file) >= 0 && skipBlocks(file);
        } else if (kind != 0) {
            break;
        }
    }
    fclose(file);
    return found;
}

double delayOf(CGImageSourceRef source, size_t index) {
    Held properties(CGImageSourceCopyPropertiesAtIndex(source, index, nullptr));
    if (!properties.p) return 0;
    auto gif = static_cast<CFDictionaryRef>(CFDictionaryGetValue(static_cast<CFDictionaryRef>(properties.p), kCGImagePropertyGIFDictionary));
    if (!gif || CFGetTypeID(gif) != CFDictionaryGetTypeID()) return 0;
    double seconds = 0;
    for (CFStringRef key : {kCGImagePropertyGIFUnclampedDelayTime, kCGImagePropertyGIFDelayTime}) {
        auto number = static_cast<CFNumberRef>(CFDictionaryGetValue(gif, key));
        if (number && CFGetTypeID(number) == CFNumberGetTypeID() && CFNumberGetValue(number, kCFNumberDoubleType, &seconds)) return seconds;
    }
    return 0;
}

}

bool gifRead(const std::string& path, GifInfo& info, const std::function<bool(uint32_t, const uint8_t*)>& frame, std::string* error) {
    info = GifInfo();
    uint8_t head[10] = {};
    FILE* file = fopen(path.c_str(), "rb");
    if (!file) return fail(error, "cannot open file");
    const size_t got = fread(head, 1, sizeof head, file);
    fclose(file);
    if (got < 10 || (memcmp(head, "GIF87a", 6) != 0 && memcmp(head, "GIF89a", 6) != 0)) return fail(error, "not a GIF file");
    info.width = head[6] | (head[7] << 8);
    info.height = head[8] | (head[9] << 8);
    if (info.width <= 0 || info.height <= 0) return fail(error, "the GIF is damaged");
    if (int64_t(info.width) * info.height > kMostPixels) return fail(error, "the GIF is too large");

    Held url(CFURLCreateFromFileSystemRepresentation(nullptr, reinterpret_cast<const UInt8*>(path.data()), CFIndex(path.size()), false));
    if (!url.p) return fail(error, "cannot open file");
    Held source(CGImageSourceCreateWithURL(static_cast<CFURLRef>(url.p), nullptr));
    auto images = static_cast<CGImageSourceRef>(const_cast<void*>(source.p));
    if (!images) return fail(error, "the GIF is damaged");
    const size_t count = CGImageSourceGetCount(images);
    if (count == 0) return fail(error, "the GIF is damaged");
    if (count > kMostFrames || int64_t(count) * info.width * info.height > kMostWork) return fail(error, "the GIF is too large");
    for (size_t i = 0; i < count; ++i) {
        info.delaysMs.push_back(gifDelayMs(delayOf(images, i)));
        info.durationMs += info.delaysMs.back();
    }
    if (info.durationMs > kLongest) return fail(error, "the GIF is too large");
    info.fps = int(std::clamp(std::floor(double(count) * 1000.0 / double(std::max<int64_t>(info.durationMs, 1)) + 0.5), 1.0, 60.0));
    if (!gifVideoSize(info.width, info.height, info.videoWidth, info.videoHeight)) return fail(error, "the GIF is too large");

    const size_t stride = size_t(info.width) * 4;
    std::vector<uint8_t> canvas(stride * size_t(info.height));
    std::vector<Shape> shapes = shapesOf(path);
    if (shapes.size() != count) shapes.clear();
    std::vector<uint8_t> painted(shapes.empty() ? 0 : size_t(info.width) * size_t(info.height), 0), kept;
    auto mark = [&](const Shape& shape, uint8_t value) {
        for (int y = std::max(shape.y, 0); y < std::min(shape.y + shape.height, info.height); ++y)
            for (int x = std::max(shape.x, 0); x < std::min(shape.x + shape.width, info.width); ++x) painted[size_t(y) * size_t(info.width) + size_t(x)] = value;
    };
    Held space(CGColorSpaceCreateDeviceRGB());
    for (size_t i = 0; i < count; ++i) {
        Held image(CGImageSourceCreateImageAtIndex(images, i, nullptr));
        auto picture = static_cast<CGImageRef>(const_cast<void*>(image.p));
        if (!picture) {
            if (i == 0) return fail(error, "the GIF is damaged");
            info.delaysMs.resize(i);
            info.durationMs = 0;
            for (int delay : info.delaysMs) info.durationMs += delay;
            break;
        }
        CGColorSpaceRef own = CGImageGetColorSpace(picture);
        const bool usable = own && CGColorSpaceGetModel(own) == kCGColorSpaceModelRGB && CGColorSpaceSupportsOutput(own);
        Held context(CGBitmapContextCreate(canvas.data(), size_t(info.width), size_t(info.height), 8, stride, usable ? own : static_cast<CGColorSpaceRef>(const_cast<void*>(space.p)),
            uint32_t(kCGImageAlphaPremultipliedFirst) | uint32_t(kCGBitmapByteOrder32Little)));
        auto target = static_cast<CGContextRef>(const_cast<void*>(context.p));
        if (!target) return fail(error, "the GIF cannot be drawn");
        CGContextSetBlendMode(target, kCGBlendModeCopy);
        CGContextSetInterpolationQuality(target, kCGInterpolationNone);
        CGContextClearRect(target, CGRectMake(0, 0, info.width, info.height));
        CGContextDrawImage(target, CGRectMake(0, 0, info.width, info.height), picture);
        if (!shapes.empty()) {
            const Shape& shape = shapes[i];
            if (shape.dispose == 3) kept = painted;
            if (shape.clear) {
                for (size_t at = 0; at < painted.size(); ++at) painted[at] = canvas[at * 4 + 3];
            } else {
                mark(shape, 255);
                for (size_t at = 0; at < painted.size(); ++at) if (!painted[at]) memset(&canvas[at * 4], 0, 4);
            }
        }
        if (!info.transparent) {
            for (size_t at = 3; at < canvas.size(); at += 4) {
                if (canvas[at] != 255) {
                    info.transparent = true;
                    break;
                }
            }
        }
        if (frame && !frame(uint32_t(i), canvas.data())) return fail(error, "cancelled");
        if (!shapes.empty()) {
            if (shapes[i].dispose == 2) mark(shapes[i], 0);
            else if (shapes[i].dispose == 3 && kept.size() == painted.size()) painted = kept;
        }
    }
    return true;
}

}
