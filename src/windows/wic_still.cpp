#include "../soft_video.h"
#include "mf.h"
#include <wincodec.h>

namespace absent {

namespace {

struct WicStill : SoftDecoder {
    ComPtr<IWICImagingFactory> factory;
    std::vector<uint8_t> pixels;
    std::vector<uint8_t> planes;

    const char* name() const override { return "wic"; }
    void reset() override {}

    bool decode(const uint8_t* data, size_t size, int64_t stamp, const PictureSink& sink) override {
        if (!comReady()) return false;
        ComPtr<IWICStream> stream;
        ComPtr<IWICBitmapDecoder> decoder;
        ComPtr<IWICBitmapFrameDecode> frame;
        ComPtr<IWICBitmapSource> converted;
        UINT width = 0, height = 0;
        if (FAILED(factory->CreateStream(&stream)) || FAILED(stream->InitializeFromMemory(const_cast<BYTE*>(data), DWORD(size))) ||
            FAILED(factory->CreateDecoderFromStream(stream.Get(), nullptr, WICDecodeMetadataCacheOnDemand, &decoder)) || FAILED(decoder->GetFrame(0, &frame)) ||
            FAILED(WICConvertBitmapSource(GUID_WICPixelFormat32bppBGR, frame.Get(), &converted)) || FAILED(converted->GetSize(&width, &height)) || !width || !height ||
            width > 32768 || height > 32768) {
            return false;
        }
        const size_t count = size_t(width) * height;
        pixels.resize(count * 4);
        if (FAILED(converted->CopyPixels(nullptr, width * 4, UINT(pixels.size()), pixels.data()))) return false;
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
        return true;
    }
};

}

std::unique_ptr<SoftDecoder> openStillDecoder(std::string* error) {
    auto still = std::make_unique<WicStill>();
    if (!comReady() || FAILED(CoCreateInstance(CLSID_WICImagingFactory, nullptr, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&still->factory)))) {
        if (error) *error = "the Windows Imaging Component did not start";
        return nullptr;
    }
    return still;
}

}
