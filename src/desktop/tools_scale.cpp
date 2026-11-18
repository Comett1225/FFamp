#include "../frames.h"
#include "../tools_common.h"
#include "image.h"

namespace absent {

struct Scaler::Impl {
    int width = 0;
    int height = 0;
};

Scaler::Scaler(int width, int height) : impl(new Impl) {
    impl->width = width;
    impl->height = height;
}

Scaler::~Scaler() = default;

void* Scaler::fit(const Frame& frame) {
    const Image* source = imageOf(frame.native.get());
    if (!source) return nullptr;
    Picture p = source->picture;
    p.color = frame.color;
    p.color.full = source->picture.color.full;
    std::vector<uint8_t> planar;
    if (p.color.known && (p.color.full || (p.color.matrix != 1 && p.color.matrix != 2))) {
        std::vector<uint8_t> bgra;
        if (!pictureToLayout(p, impl->width, impl->height, PixelLayout::Bgra, bgra)) return nullptr;
        planar.resize(pixelBytes(PixelLayout::I420, impl->width, impl->height));
        bgraToI420(bgra.data(), size_t(impl->width) * 4, impl->width, impl->height, planar.data());
    } else if (!pictureToLayout(p, impl->width, impl->height, PixelLayout::I420, planar)) {
        return nullptr;
    }
    auto* image = new Image;
    image->bytes = std::move(planar);
    shapeImage(*image, impl->width, impl->height, (impl->width + 1) / 2, (impl->height + 1) / 2, 8, false);
    image->picture.color.known = true;
    return image;
}

void releaseBuffer(void* buffer) { delete static_cast<Image*>(buffer); }

}
