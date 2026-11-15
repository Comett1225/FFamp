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

    bool planar(IWICBitmapFrameDecode* frame, int64_t stamp, const PictureSink& sink) {
        ComPtr<IWICPlanarBitmapSourceTransform> transform;
        UINT width = 0, height = 0;
        if (FAILED(frame->QueryInterface(IID_PPV_ARGS(&transform))) || FAILED(frame->GetSize(&width, &height)) || !width || !height || width > 32768 || height > 32768) return false;
        WICPixelFormatGUID formats[3] = {GUID_WICPixelFormat8bppY, GUID_WICPixelFormat8bppCb, GUID_WICPixelFormat8bppCr};
        WICBitmapPlaneDescription described[3] = {};
        UINT w = width, h = height;
        BOOL supported = FALSE;
        if (FAILED(transform->DoesSupportTransform(&w, &h, WICBitmapTransformRotate0, WICPlanarOptionsDefault, formats, described, 3, &supported)) || !supported || w != width || h != height) return false;
        size_t at[3] = {}, total = 0;
        for (int i = 0; i < 3; ++i) {
            at[i] = total;
            total += size_t(described[i].Width) * described[i].Height;
        }
        planes.resize(total);
        WICBitmapPlane given[3];
        for (int i = 0; i < 3; ++i) {
            given[i].Format = formats[i];
            given[i].pbBuffer = planes.data() + at[i];
            given[i].cbStride = described[i].Width;
            given[i].cbBufferSize = UINT(size_t(described[i].Width) * described[i].Height);
        }
        if (FAILED(transform->CopyPixels(nullptr, width, height, WICBitmapTransformRotate0, WICPlanarOptionsDefault, given, 3))) return false;
        Picture p;
        p.stamp = stamp;
        p.width = int(width);
        p.height = int(height);
        p.chromaWidth = int(described[1].Width);
        p.chromaHeight = int(described[1].Height);
        for (int i = 0; i < 3; ++i) {
            p.plane[i] = planes.data() + at[i];
            p.stride[i] = described[i].Width;
        }
        p.color.known = true;
        p.color.matrix = 6;
        p.color.primaries = 1;
        p.color.transfer = 1;
        p.color.full = true;
        sink(p);
        return true;
    }

    bool decode(const uint8_t* data, size_t size, int64_t stamp, const PictureSink& sink) override {
        if (!comReady()) return false;
        ComPtr<IWICStream> stream;
        ComPtr<IWICBitmapDecoder> decoder;
        ComPtr<IWICBitmapFrameDecode> frame;
        ComPtr<IWICBitmapSource> converted;
        UINT width = 0, height = 0;
        if (FAILED(factory->CreateStream(&stream)) || FAILED(stream->InitializeFromMemory(const_cast<BYTE*>(data), DWORD(size))) ||
            FAILED(factory->CreateDecoderFromStream(stream.Get(), nullptr, WICDecodeMetadataCacheOnDemand, &decoder)) || FAILED(decoder->GetFrame(0, &frame))) {
            return false;
        }
        if (planar(frame.Get(), stamp, sink)) return true;
        if (FAILED(WICConvertBitmapSource(GUID_WICPixelFormat32bppBGR, frame.Get(), &converted)) || FAILED(converted->GetSize(&width, &height)) || !width || !height ||
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
        p.color.full = false;
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
