#pragma once
#include "readers.h"
#include "soft_video.h"
#include <algorithm>
#include <cstring>
#include <vector>

namespace absent {

struct Planes {
    int width = 0;
    int height = 0;
    int chromaWidth = 0;
    int chromaHeight = 0;
    int bits = 8;
    bool alpha = false;
    Color color;
    std::vector<uint8_t> data[4];
    std::vector<uint8_t> veil;

    size_t unit() const { return bits > 8 ? 2 : 1; }
    size_t stride(int plane) const { return size_t(plane == 1 || plane == 2 ? chromaWidth : width) * unit(); }

    void shape(int w, int h, int cw, int ch, int depth, bool withAlpha) {
        width = w;
        height = h;
        chromaWidth = cw;
        chromaHeight = ch;
        bits = depth;
        alpha = withAlpha;
        for (int plane = 0; plane < 4; ++plane) {
            const size_t size = plane == 3 && !withAlpha ? 0 : stride(plane) * size_t(plane == 1 || plane == 2 ? ch : h);
            if (data[plane].size() != size) data[plane].assign(size, 0);
        }
    }

    void rgb(int w, int h, int depth, bool withAlpha) {
        shape(w, h, w, h, depth, withAlpha);
        color = Color();
        color.known = true;
        color.transfer = 13;
        color.matrix = 0;
        color.full = true;
    }

    uint8_t* row(int plane, int y) { return data[plane].data() + size_t(y) * stride(plane); }
    uint16_t* wide(int plane, int y) { return reinterpret_cast<uint16_t*>(row(plane, y)); }

    void clear() {
        for (auto& plane : data) std::fill(plane.begin(), plane.end(), 0);
    }

    Picture picture(int64_t stamp) {
        Picture p;
        p.width = width;
        p.height = height;
        p.chromaWidth = chromaWidth;
        p.chromaHeight = chromaHeight;
        p.bits = bits;
        p.stamp = stamp;
        p.color = color;
        for (int plane = 0; plane < 3; ++plane) {
            p.plane[plane] = data[plane].data();
            p.stride[plane] = stride(plane);
        }
        if (alpha) {
            if (bits == 8) {
                p.alpha = data[3].data();
            } else {
                veil.resize(size_t(width) * size_t(height));
                const int drop = bits - 8;
                const uint16_t* from = reinterpret_cast<const uint16_t*>(data[3].data());
                for (size_t i = 0; i < veil.size(); ++i) veil[i] = uint8_t(std::min((from[i] + (1 << (drop - 1))) >> drop, 255));
                p.alpha = veil.data();
            }
            p.alphaStride = size_t(width);
        }
        return p;
    }
};

std::unique_ptr<SoftDecoder> openOwnVideo(const Track& track, std::string* error);
std::unique_ptr<SoftDecoder> openQtrle(const Track& track, std::string* error);
std::unique_ptr<SoftDecoder> openPng(std::string* error);
std::unique_ptr<SoftDecoder> openDnxhd(std::string* error);
std::unique_ptr<SoftDecoder> openHap(const Track& track, std::string* error);
std::unique_ptr<SoftDecoder> openUtVideo(const Track& track, std::string* error);
std::unique_ptr<SoftDecoder> openFfv1(const Track& track, std::string* error);

}
