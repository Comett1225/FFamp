#ifdef ABSENT_VAAPI
#include "vaapi.h"
#include <va/va_drm.h>
#include <dlfcn.h>
#include <fcntl.h>
#include <algorithm>
#include <unistd.h>
#include <cstdlib>
#include <string>

namespace absent {

namespace {

template <class F>
bool symbol(void* library, const char* name, F& to) {
    to = reinterpret_cast<F>(dlsym(library, name));
    return to != nullptr;
}

void quiet(void*, const char*) {}

const Va* start() {
    if (std::getenv("FFAMP_NO_VAAPI")) return nullptr;
    void* core = dlopen("libva.so.2", RTLD_NOW | RTLD_LOCAL);
    void* drm = core ? dlopen("libva-drm.so.2", RTLD_NOW | RTLD_LOCAL) : nullptr;
    if (!drm) return nullptr;
    static Va va;
    decltype(&::vaGetDisplayDRM) display = nullptr;
    decltype(&::vaInitialize) initialize = nullptr;
    decltype(&::vaTerminate) terminate = nullptr;
    decltype(&::vaSetInfoCallback) info = nullptr;
    decltype(&::vaSetErrorCallback) errors = nullptr;
    decltype(&::vaSetDriverName) driver = nullptr;
    symbol(core, "vaSetDriverName", driver);
    const bool found = symbol(drm, "vaGetDisplayDRM", display) && symbol(core, "vaInitialize", initialize) && symbol(core, "vaTerminate", terminate) &&
        symbol(core, "vaSetInfoCallback", info) && symbol(core, "vaSetErrorCallback", errors) &&
        symbol(core, "vaQueryConfigEntrypoints", va.vaQueryConfigEntrypoints) && symbol(core, "vaCreateConfig", va.vaCreateConfig) &&
        symbol(core, "vaDestroyConfig", va.vaDestroyConfig) && symbol(core, "vaCreateSurfaces", va.vaCreateSurfaces) &&
        symbol(core, "vaDestroySurfaces", va.vaDestroySurfaces) && symbol(core, "vaCreateContext", va.vaCreateContext) &&
        symbol(core, "vaDestroyContext", va.vaDestroyContext) && symbol(core, "vaCreateBuffer", va.vaCreateBuffer) &&
        symbol(core, "vaDestroyBuffer", va.vaDestroyBuffer) && symbol(core, "vaMapBuffer", va.vaMapBuffer) && symbol(core, "vaUnmapBuffer", va.vaUnmapBuffer) &&
        symbol(core, "vaBeginPicture", va.vaBeginPicture) && symbol(core, "vaRenderPicture", va.vaRenderPicture) &&
        symbol(core, "vaEndPicture", va.vaEndPicture) && symbol(core, "vaSyncSurface", va.vaSyncSurface) && symbol(core, "vaCreateImage", va.vaCreateImage) &&
        symbol(core, "vaDestroyImage", va.vaDestroyImage) && symbol(core, "vaGetImage", va.vaGetImage) && symbol(core, "vaDeriveImage", va.vaDeriveImage);
    if (!found) return nullptr;
    const bool wsl = access("/dev/dxg", F_OK) == 0 && !std::getenv("LIBVA_DRIVER_NAME");
    if (wsl) setenv("GALLIUM_DRIVER", "d3d12", 0);
    for (int node = 128; node < 136; ++node) {
        const int fd = open(("/dev/dri/renderD" + std::to_string(node)).c_str(), O_RDWR | O_CLOEXEC);
        if (fd < 0) continue;
        VADisplay shown = display(fd);
        if (shown) {
            info(shown, quiet, nullptr);
            errors(shown, quiet, nullptr);
            if (wsl && driver) {
                char name[] = "d3d12";
                driver(shown, name);
            }
            int major = 0, minor = 0;
            if (initialize(shown, &major, &minor) == VA_STATUS_SUCCESS) {
                va.display = shown;
                return &va;
            }
            terminate(shown);
        }
        close(fd);
    }
    return nullptr;
}

}

const Va* vaapi() {
    static const Va* va = start();
    return va;
}

bool vaDecodes(VAProfile profile) {
    const Va* va = vaapi();
    if (!va) return false;
    VAEntrypoint points[16];
    int count = 0;
    if (va->vaQueryConfigEntrypoints(va->display, profile, points, &count) != VA_STATUS_SUCCESS) return false;
    for (int i = 0; i < count && i < 16; ++i) if (points[i] == VAEntrypointVLD) return true;
    return false;
}

bool VaSession::open(VAProfile profile, int width, int height, bool tenBit, int count) {
    close();
    va = vaapi();
    if (!va || width <= 0 || height <= 0) return false;
    deep = tenBit;
    VAConfigAttrib format{VAConfigAttribRTFormat, uint32_t(deep ? VA_RT_FORMAT_YUV420_10 : VA_RT_FORMAT_YUV420)};
    if (va->vaCreateConfig(va->display, profile, VAEntrypointVLD, &format, 1, &config) != VA_STATUS_SUCCESS) {
        config = VA_INVALID_ID;
        return false;
    }
    VASurfaceAttrib pixels{};
    pixels.type = VASurfaceAttribPixelFormat;
    pixels.flags = VA_SURFACE_ATTRIB_SETTABLE;
    pixels.value.type = VAGenericValueTypeInteger;
    pixels.value.value.i = int(deep ? VA_FOURCC_P010 : VA_FOURCC_NV12);
    surfaces.resize(size_t(count));
    if (va->vaCreateSurfaces(va->display, format.value, unsigned(width), unsigned(height), surfaces.data(), unsigned(count), &pixels, 1) != VA_STATUS_SUCCESS) {
        surfaces.clear();
        close();
        return false;
    }
    if (va->vaCreateContext(va->display, config, width, height, VA_PROGRESSIVE, surfaces.data(), count, &context) != VA_STATUS_SUCCESS) {
        context = VA_INVALID_ID;
        close();
        return false;
    }
    frameWidth = width;
    frameHeight = height;
    return true;
}

void VaSession::drop() {
    for (VABufferID id : params) va->vaDestroyBuffer(va->display, id);
    for (VABufferID id : slices) va->vaDestroyBuffer(va->display, id);
    params.clear();
    slices.clear();
}

void VaSession::close() {
    if (!va) return;
    drop();
    if (imaged) va->vaDestroyImage(va->display, image.image_id);
    imaged = false;
    if (context != VA_INVALID_ID) va->vaDestroyContext(va->display, context);
    if (!surfaces.empty()) va->vaDestroySurfaces(va->display, surfaces.data(), int(surfaces.size()));
    if (config != VA_INVALID_ID) va->vaDestroyConfig(va->display, config);
    context = VA_INVALID_ID;
    config = VA_INVALID_ID;
    surfaces.clear();
    frameWidth = frameHeight = 0;
}

VASurfaceID VaSession::spare(const std::vector<VASurfaceID>& busy) const {
    for (VASurfaceID id : surfaces) {
        bool used = false;
        for (VASurfaceID taken : busy) used = used || taken == id;
        if (!used) return id;
    }
    return VA_INVALID_SURFACE;
}

bool VaSession::param(VABufferType type, const void* data, size_t size) {
    VABufferID id = VA_INVALID_ID;
    if (va->vaCreateBuffer(va->display, context, type, unsigned(size), 1, const_cast<void*>(data), &id) != VA_STATUS_SUCCESS) return false;
    params.push_back(id);
    return true;
}

bool VaSession::slice(const void* data, size_t size, const uint8_t* bytes, size_t length) {
    VABufferID a = VA_INVALID_ID, b = VA_INVALID_ID;
    if (va->vaCreateBuffer(va->display, context, VASliceParameterBufferType, unsigned(size), 1, const_cast<void*>(data), &a) != VA_STATUS_SUCCESS) return false;
    slices.push_back(a);
    if (va->vaCreateBuffer(va->display, context, VASliceDataBufferType, unsigned(length), 1, const_cast<uint8_t*>(bytes), &b) != VA_STATUS_SUCCESS) return false;
    slices.push_back(b);
    return true;
}

bool VaSession::submit(VASurfaceID target) {
    bool ok = !slices.empty() && va->vaBeginPicture(va->display, context, target) == VA_STATUS_SUCCESS;
    if (ok) {
        ok = va->vaRenderPicture(va->display, context, params.data(), int(params.size())) == VA_STATUS_SUCCESS &&
            va->vaRenderPicture(va->display, context, slices.data(), int(slices.size())) == VA_STATUS_SUCCESS;
        ok = va->vaEndPicture(va->display, context) == VA_STATUS_SUCCESS && ok;
    }
    drop();
    return ok;
}

bool VaSession::hand(VASurfaceID surface, int width, int height, int64_t stamp, const Color& color, const PictureSink& sink) {
    if (va->vaSyncSurface(va->display, surface) != VA_STATUS_SUCCESS) return false;
    if (!imaged) {
        VAImageFormat format{};
        format.fourcc = deep ? VA_FOURCC_P010 : VA_FOURCC_NV12;
        format.byte_order = VA_LSB_FIRST;
        format.bits_per_pixel = deep ? 24 : 12;
        if (va->vaCreateImage(va->display, &format, frameWidth, frameHeight, &image) != VA_STATUS_SUCCESS) return false;
        imaged = true;
    }
    if (va->vaGetImage(va->display, surface, 0, 0, unsigned(frameWidth), unsigned(frameHeight), image.image_id) != VA_STATUS_SUCCESS) return false;
    void* mapped = nullptr;
    if (va->vaMapBuffer(va->display, image.buf, &mapped) != VA_STATUS_SUCCESS || !mapped) return false;
    const uint8_t* bytes = static_cast<const uint8_t*>(mapped);
    Picture p;
    p.width = std::min(width, frameWidth);
    p.height = std::min(height, frameHeight);
    p.chromaWidth = (p.width + 1) / 2;
    p.chromaHeight = (p.height + 1) / 2;
    p.interleaved = true;
    p.bits = deep ? 10 : 8;
    p.shift = deep ? 6 : 0;
    p.plane[0] = bytes + image.offsets[0];
    p.plane[1] = bytes + image.offsets[1];
    p.stride[0] = image.pitches[0];
    p.stride[1] = image.pitches[1];
    p.stamp = stamp;
    p.color = color;
    sink(p);
    va->vaUnmapBuffer(va->display, image.buf);
    return true;
}

std::unique_ptr<SoftDecoder> openVaapi(const Track& track, const std::vector<uint8_t>& config, const StreamShape& shape, std::string* error) {
    const bool h264 = track.configType == fourcc("avcC"), hevc = track.configType == fourcc("hvcC");
    const bool vp9 = track.codec == fourcc("vp09"), av1 = track.codec == fourcc("av01");
    if (!h264 && !hevc && !vp9 && !av1) return nullptr;
    if (!vaapi()) {
        if (error) *error = "VA-API is not available on this system";
        return nullptr;
    }
    std::string why;
    std::unique_ptr<SoftDecoder> decoder = h264 ? openVaapiH264(config, &why)
        : hevc ? openVaapiHevc(config, &why)
        : vp9 ? openVaapiVp9(shape, track.width, track.height, &why)
        : openVaapiAv1(config, shape, track.width, track.height, &why);
    if (!decoder && error) *error = "VA-API: " + why;
    return decoder;
}

}
#endif
