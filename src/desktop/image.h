#pragma once
#include "../soft_video.h"
#include <memory>
#include <mutex>
#include <vector>

namespace absent {

struct Image {
    std::vector<uint8_t> bytes;
    std::vector<uint8_t> alpha;
    Picture picture;
};

class ImagePool : public std::enable_shared_from_this<ImagePool> {
public:
    std::shared_ptr<Image> take();
    std::shared_ptr<Image> copy(const Picture& p);

private:
    std::mutex lock;
    std::vector<std::unique_ptr<Image>> spare;
};

void fillImage(Image& image, const Picture& p);
void shapeImage(Image& image, int width, int height, int chromaWidth, int chromaHeight, int bits, bool interleaved);
const Image* imageOf(const void* native);

}
