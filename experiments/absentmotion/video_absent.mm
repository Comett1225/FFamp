#include "video.h"
#include "absent/media.h"
#include <algorithm>
#include <cmath>
#include <memory>
#include <mutex>
#include <vector>
#include "include/core/SkColorSpace.h"
#include "include/core/SkYUVAInfo.h"
#include "include/gpu/ganesh/GrBackendSurface.h"
#include "include/gpu/ganesh/GrDirectContext.h"
#include "include/gpu/ganesh/GrYUVABackendTextures.h"
#include "include/gpu/ganesh/SkImageGanesh.h"
#include "include/gpu/ganesh/mtl/GrMtlBackendSurface.h"
#include "include/gpu/ganesh/mtl/GrMtlTypes.h"
#import <CoreVideo/CoreVideo.h>
#import <Metal/Metal.h>

namespace {

CVMetalTextureCacheRef Cache() {
    static CVMetalTextureCacheRef cache = [] {
        CVMetalTextureCacheRef made = nullptr;
        id<MTLDevice> device = MTLCreateSystemDefaultDevice();
        if (device) CVMetalTextureCacheCreate(kCFAllocatorDefault, nullptr, device, nullptr, &made);
        return made;
    }();
    return cache;
}

struct Held {
    CVMetalTextureRef planes[2] = {};
    CVPixelBufferRef pixels = nullptr;
};

void Release(void* context) {
    auto* held = static_cast<Held*>(context);
    for (auto plane : held->planes) if (plane) CFRelease(plane);
    if (held->pixels) CVPixelBufferRelease(held->pixels);
    delete held;
}

SkYUVColorSpace Space(CVPixelBufferRef pixels) {
    const bool full = CVPixelBufferGetPixelFormatType(pixels) == kCVPixelFormatType_420YpCbCr8BiPlanarFullRange;
    CFTypeRef matrix = CVBufferCopyAttachment(pixels, kCVImageBufferYCbCrMatrixKey, nullptr);
    int kind = 0;
    if (matrix) {
        if (CFEqual(matrix, kCVImageBufferYCbCrMatrix_ITU_R_709_2)) kind = 709;
        else if (CFEqual(matrix, kCVImageBufferYCbCrMatrix_ITU_R_2020)) kind = 2020;
        else if (CFEqual(matrix, kCVImageBufferYCbCrMatrix_ITU_R_601_4)) kind = 601;
        CFRelease(matrix);
    }
    if (kind == 0 && CVPixelBufferGetHeight(pixels) >= 720) kind = 709;
    if (kind == 2020) return full ? kBT2020_8bit_Full_SkYUVColorSpace : kBT2020_8bit_Limited_SkYUVColorSpace;
    if (kind == 709) return full ? kRec709_Full_SkYUVColorSpace : kRec709_Limited_SkYUVColorSpace;
    return full ? kJPEG_Full_SkYUVColorSpace : kRec601_Limited_SkYUVColorSpace;
}

sk_sp<SkImage> Image(GrDirectContext* context, CVPixelBufferRef pixels) {
    if (!context || context->backend() != GrBackendApi::kMetal || !pixels || !Cache() || CVPixelBufferGetPlaneCount(pixels) != 2) return nullptr;
    const int width = (int)CVPixelBufferGetWidth(pixels), height = (int)CVPixelBufferGetHeight(pixels);
    auto* held = new Held;
    held->pixels = CVPixelBufferRetain(pixels);
    GrBackendTexture textures[SkYUVAInfo::kMaxPlanes];
    for (int i = 0; i < 2; ++i) {
        const MTLPixelFormat format = i == 0 ? MTLPixelFormatR8Unorm : MTLPixelFormatRG8Unorm;
        const size_t w = CVPixelBufferGetWidthOfPlane(pixels, i), h = CVPixelBufferGetHeightOfPlane(pixels, i);
        if (CVMetalTextureCacheCreateTextureFromImage(kCFAllocatorDefault, Cache(), pixels, nullptr, format, w, h, i, &held->planes[i]) != kCVReturnSuccess) {
            Release(held);
            return nullptr;
        }
        GrMtlTextureInfo info;
        info.fTexture.retain((__bridge GrMTLHandle)CVMetalTextureGetTexture(held->planes[i]));
        textures[i] = GrBackendTextures::MakeMtl((int)w, (int)h, skgpu::Mipmapped::kNo, info);
    }
    const SkYUVAInfo yuva({width, height}, SkYUVAInfo::PlaneConfig::kY_UV, SkYUVAInfo::Subsampling::k420, Space(pixels));
    const GrYUVABackendTextures planes(yuva, textures, kTopLeft_GrSurfaceOrigin);
    auto image = SkImages::TextureFromYUVATextures(context, planes, SkColorSpace::MakeSRGB(), Release, held);
    CVMetalTextureCacheFlush(Cache(), 0);
    return image;
}

struct Lane {
    std::unique_ptr<absent::VideoDecoder> decoder;
    int64_t at = -1;
    int64_t shownIndex = -1;
    int64_t owner = INT64_MIN;
    uint64_t used = 0;
    sk_sp<SkImage> shown;
};

struct Video {
    std::unique_ptr<absent::Mp4Reader> file;
    const absent::Track* track = nullptr;
    std::vector<int64_t> times;
    std::vector<std::unique_ptr<Lane>> lanes;
    std::mutex lock;
    uint64_t turns = 0;
    double info[7] = {};

    Lane* add() {
        auto lane = std::make_unique<Lane>();
        lane->decoder = absent::VideoDecoder::open(*file, *track);
        if (!lane->decoder) return nullptr;
        return lanes.emplace_back(std::move(lane)).get();
    }

    Lane* route(int64_t index, int64_t layer) {
        Lane* best = nullptr;
        int64_t gap = INT64_MAX;
        for (auto& lane : lanes) {
            const int64_t d = index - lane->at;
            if (lane->at >= 0 && d >= 0 && d <= 60 && d < gap) gap = d, best = lane.get();
        }
        for (auto& lane : lanes) if (!best && lane->owner == layer) best = lane.get();
        for (auto& lane : lanes) if (!best && lane->at < 0) best = lane.get();
        if (!best && lanes.size() < 3) best = add();
        if (!best) {
            best = lanes.front().get();
            for (auto& lane : lanes) if (lane->used < best->used) best = lane.get();
        }
        best->used = ++turns;
        best->owner = layer;
        return best;
    }

    int show(jlong desktop, const std::string& uri, int64_t t, int64_t layer) {
        std::lock_guard<std::mutex> hold(lock);
        auto it = std::upper_bound(times.begin(), times.end(), t);
        const int64_t index = it == times.begin() ? 0 : (it - times.begin()) - 1;
        Lane* lane = route(index, layer);
        if (!lane) return -1;
        if (lane->shownIndex != index || !lane->shown) {
            absent::Frame frame = lane->decoder->frame((uint32_t)index);
            auto made = frame ? Image(DesktopContext(desktop), static_cast<CVPixelBufferRef>(frame.native.get())) : nullptr;
            if (!made) return -1;
            lane->shown = std::move(made);
            lane->shownIndex = index;
        }
        lane->at = index;
        if (layer) DesktopInstallLayer(desktop, uri, layer, lane->shown); else DesktopInstall(desktop, uri, lane->shown);
        return 1;
    }
};

Video* Open(const std::string& path) {
    auto v = std::make_unique<Video>();
    v->file = absent::Mp4Reader::open(path);
    if (!v->file) return nullptr;
    v->track = v->file->first(absent::Kind::Video);
    if (!v->track || v->track->order.empty() || !v->add()) return nullptr;
    const absent::Track& t = *v->track;
    for (uint32_t i : t.order) v->times.push_back((int64_t)std::llround((double)(t.samples[i].pts - t.mediaStart) * 1e6 / t.timescale));
    const double values[7] = {(double)t.width, (double)t.height, t.seconds() * 1e6, t.rate(), (double)t.rotation, v->file->first(absent::Kind::Audio) ? 1.0 : 0.0, 1.0};
    std::copy(values, values + 7, v->info);
    return v.release();
}

Video* video(jlong v) { return reinterpret_cast<Video*>(v); }

}

#define JNI(name) Java_app_absentmotion_pc_engine_DesktopNative_##name
extern "C" {
JNIEXPORT jlong JNICALL JNI(videoOpen)(JNIEnv* e, jobject, jstring path) { return reinterpret_cast<jlong>(Open(DesktopString(e, path))); }
JNIEXPORT jdoubleArray JNICALL JNI(videoInfo)(JNIEnv* e, jobject, jlong v) {
    auto result = e->NewDoubleArray(7);
    if (!result) return nullptr;
    e->SetDoubleArrayRegion(result, 0, 7, video(v)->info);
    return result;
}
JNIEXPORT jint JNICALL JNI(videoFrame)(JNIEnv* e, jobject, jlong h, jlong v, jstring uri, jlong timeUs, jboolean) {
    return video(v)->show(h, DesktopString(e, uri), timeUs, 0);
}
JNIEXPORT jint JNICALL JNI(videoFrameLayer)(JNIEnv* e, jobject, jlong h, jlong v, jstring uri, jlong layer, jlong timeUs, jint) {
    return video(v)->show(h, DesktopString(e, uri), timeUs, layer);
}
JNIEXPORT void JNICALL JNI(videoClose)(JNIEnv*, jobject, jlong v) { delete video(v); }
}
