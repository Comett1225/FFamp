#pragma once
#include "../soft_video.h"
#include <va/va.h>
#include <string>
#include <vector>

namespace absent {

struct Va {
    VADisplay display = nullptr;
    decltype(&::vaQueryConfigEntrypoints) vaQueryConfigEntrypoints = nullptr;
    decltype(&::vaCreateConfig) vaCreateConfig = nullptr;
    decltype(&::vaDestroyConfig) vaDestroyConfig = nullptr;
    decltype(&::vaCreateSurfaces) vaCreateSurfaces = nullptr;
    decltype(&::vaDestroySurfaces) vaDestroySurfaces = nullptr;
    decltype(&::vaCreateContext) vaCreateContext = nullptr;
    decltype(&::vaDestroyContext) vaDestroyContext = nullptr;
    decltype(&::vaCreateBuffer) vaCreateBuffer = nullptr;
    decltype(&::vaDestroyBuffer) vaDestroyBuffer = nullptr;
    decltype(&::vaMapBuffer) vaMapBuffer = nullptr;
    decltype(&::vaUnmapBuffer) vaUnmapBuffer = nullptr;
    decltype(&::vaBeginPicture) vaBeginPicture = nullptr;
    decltype(&::vaRenderPicture) vaRenderPicture = nullptr;
    decltype(&::vaEndPicture) vaEndPicture = nullptr;
    decltype(&::vaSyncSurface) vaSyncSurface = nullptr;
    decltype(&::vaCreateImage) vaCreateImage = nullptr;
    decltype(&::vaDestroyImage) vaDestroyImage = nullptr;
    decltype(&::vaGetImage) vaGetImage = nullptr;
    decltype(&::vaSyncSurface2) vaSyncSurface2 = nullptr;
    bool d3d12 = false;
};

const Va* vaapi();
bool vaDecodes(VAProfile profile);
inline bool d3d12WrongTenBit(bool deep, int height) { return deep && vaapi() && vaapi()->d3d12 && height % 16 >= 2 && height % 16 <= 9; }

class VaSession {
public:
    explicit VaSession(const std::function<bool(int64_t)>* wanted = nullptr) : wanted(wanted) {}
    ~VaSession() { close(); }
    bool open(VAProfile profile, int width, int height, bool deep, int count);
    void close();
    bool ready() const { return context != VA_INVALID_ID; }
    int width() const { return frameWidth; }
    int height() const { return frameHeight; }
    bool tenBit() const { return deep; }
    VASurfaceID spare(const std::vector<VASurfaceID>& busy) const;
    bool param(VABufferType type, const void* data, size_t size);
    bool slice(const void* params, size_t size, const uint8_t* data, size_t length);
    bool submit(VASurfaceID target);
    bool hand(VASurfaceID surface, int width, int height, int64_t stamp, const Color& color, const PictureSink& sink, int left = 0, int top = 0);

private:
    void drop();
    const std::function<bool(int64_t)>* wanted = nullptr;
    const Va* va = nullptr;
    VAConfigID config = VA_INVALID_ID;
    VAContextID context = VA_INVALID_ID;
    std::vector<VASurfaceID> surfaces;
    std::vector<VABufferID> params;
    std::vector<VABufferID> slices;
    VAImage image{};
    bool imaged = false;
    bool failed = false;
    bool deep = false;
    int frameWidth = 0;
    int frameHeight = 0;
};

std::unique_ptr<SoftDecoder> openVaapiVp9(const StreamShape& shape, int width, int height, std::string* error);
std::unique_ptr<SoftDecoder> openVaapiAv1(const std::vector<uint8_t>& config, const StreamShape& shape, int width, int height, std::string* error);
std::unique_ptr<SoftDecoder> openVaapiH264(const std::vector<uint8_t>& config, std::string* error);
std::unique_ptr<SoftDecoder> openVaapiHevc(const std::vector<uint8_t>& config, std::string* error);

}
