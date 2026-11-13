#pragma once
#include "absent/media.h"
#include <functional>

namespace absent {

struct Picture {
    const uint8_t* plane[3] = {nullptr, nullptr, nullptr};
    size_t stride[3] = {0, 0, 0};
    int width = 0;
    int height = 0;
    int chromaWidth = 0;
    int chromaHeight = 0;
    int bits = 8;
    int shift = 0;
    bool interleaved = false;
    const uint8_t* alpha = nullptr;
    size_t alphaStride = 0;
    int64_t stamp = 0;
    Color color;
};

struct StreamShape {
    int width = 0;
    int height = 0;
    int bits = 8;
    int chromaX = 1;
    int chromaY = 1;
    Color color;
};

struct H264Layout {
    int lengthSize = 4;
    int frameNumBits = 4;
    bool separatePlanes = false;
    bool fields = false;
};

using PictureSink = std::function<void(const Picture&)>;
using PacketSink = std::function<void(Packet&&)>;

class SoftDecoder {
public:
    virtual ~SoftDecoder() = default;
    virtual const char* name() const = 0;
    virtual bool decode(const uint8_t* data, size_t size, int64_t stamp, const PictureSink& shown) = 0;
    virtual bool drain(const PictureSink&) { return true; }
    virtual void reset() = 0;
};

class SoftEncoder {
public:
    virtual ~SoftEncoder() = default;
    virtual const char* name() const = 0;
    virtual bool encode(const Picture& picture, int64_t pts, int64_t duration, const PacketSink& sink) = 0;
    virtual bool finish(const PacketSink& sink) = 0;
    virtual uint32_t configType() const = 0;
    virtual std::vector<uint8_t> config() const = 0;
};

std::unique_ptr<SoftDecoder> openDav1d(int delay, std::string* error);
std::unique_ptr<SoftDecoder> openVpxDecoder(bool vp8, std::string* error);
std::unique_ptr<SoftEncoder> openSvtAv1(const EncodeSpec& spec, std::string* error);
std::unique_ptr<SoftEncoder> openVpxEncoder(const EncodeSpec& spec, std::string* error);
std::unique_ptr<SoftEncoder> openApvEncoder(const EncodeSpec& spec, std::string* error);
std::unique_ptr<SoftDecoder> openApvDecoder(std::string* error);
std::unique_ptr<SoftDecoder> openVc1(uint32_t codec, const std::vector<uint8_t>& config, int width, int height, std::string* error);
std::unique_ptr<SoftDecoder> openXvid(const std::vector<uint8_t>& esds, int width, int height);

bool vp9Shape(const uint8_t* frame, size_t size, StreamShape& shape);
std::vector<uint8_t> vp9Config(const StreamShape& shape, double fps, int bitrate);
bool av1Config(const uint8_t* unit, size_t size, std::vector<uint8_t>& config, StreamShape& shape);
bool h264Layout(const std::vector<uint8_t>& config, H264Layout& layout);
int h264FieldOf(const H264Layout& layout, const uint8_t* sample, size_t size, uint32_t& frameNumber);
bool shapeFromConfig(uint32_t configType, const std::vector<uint8_t>& config, StreamShape& shape);

bool pictureToBGRA(const Picture& picture, uint8_t* out, size_t stride, bool redFirst = false);
void rowsToBGRA(const Picture& picture, uint8_t* out, size_t stride, bool redFirst, int from, int to);
void pictureRow(const Picture& picture, int plane, int y, float* out);
void inParallel(int rows, const std::function<void(int, int)>& work);
void pictureToPlanar(const Picture& picture, std::vector<uint8_t>& out, bool narrow = false);
void pictureToI420(const Picture& picture, std::vector<uint8_t>& out);
bool withPixelBuffer(void* pixelBuffer, const std::function<bool(const Picture&)>& use);

}
