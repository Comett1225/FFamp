#pragma once
#include "absent/capture.h"
#include "soft_video.h"

namespace absent {

void scalePlane(const uint8_t* in, size_t inStride, int inWidth, int inHeight, uint8_t* out, int outWidth, int outHeight, int bytes, int top, double stepX, double originX, double stepY, double originY);
bool pictureToLayout(const Picture& picture, int width, int height, PixelLayout layout, std::vector<uint8_t>& out);
void turnPixels(const std::vector<uint8_t>& in, int width, int height, PixelLayout layout, int rotation, std::vector<uint8_t>& out);
void layoutToI420(const uint8_t* pixels, int width, int height, PixelLayout layout, std::vector<uint8_t>& out);

}
