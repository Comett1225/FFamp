#include "nal.h"
#include "absent/media.h"
#include "bytes.h"
#include <algorithm>

namespace absent {

std::vector<uint8_t> unescape(const uint8_t* p, size_t n) {
    std::vector<uint8_t> out;
    out.reserve(n);
    int zeros = 0;
    for (size_t i = 0; i < n; ++i) {
        if (zeros >= 2 && p[i] == 3) {
            zeros = 0;
            continue;
        }
        out.push_back(p[i]);
        zeros = p[i] == 0 ? zeros + 1 : 0;
    }
    return out;
}

namespace {

void avcScalingList(BitReader& r, int size) {
    int last = 8, next = 8;
    for (int j = 0; j < size && r.ok; ++j) {
        if (next != 0) next = int((last + int64_t(r.se()) + 256) % 256);
        last = next == 0 ? last : next;
    }
}

void signalType(BitReader& r, VideoInfo& info) {
    r.skip(3);
    info.full = r.bit();
    info.rangeKnown = r.ok;
    if (r.bit()) {
        info.primaries = int(r.bits(8));
        info.transfer = int(r.bits(8));
        info.matrix = int(r.bits(8));
        info.colorKnown = r.ok;
    }
}

void aspectAndOverscan(BitReader& r) {
    if (r.bit() && r.bits(8) == 255) r.skip(32);
    if (r.bit()) r.skip(1);
}

void hevcScalingLists(BitReader& r) {
    for (int sizeId = 0; sizeId < 4; ++sizeId) {
        for (int matrixId = 0; matrixId < 6; matrixId += sizeId == 3 ? 3 : 1) {
            if (!r.bit()) {
                r.ue();
                continue;
            }
            int count = std::min(64, 1 << (4 + (sizeId << 1)));
            if (sizeId > 1) r.se();
            for (int i = 0; i < count && r.ok; ++i) r.se();
        }
    }
}

bool hevcShortTermSets(BitReader& r) {
    uint32_t count = r.ue();
    if (count > 64) return false;
    std::vector<uint32_t> deltas(count, 0);
    for (uint32_t index = 0; index < count && r.ok; ++index) {
        if (index != 0 && r.bit()) {
            r.skip(1);
            r.ue();
            uint32_t reference = deltas[index - 1];
            uint32_t kept = 0;
            for (uint32_t j = 0; j <= reference && r.ok; ++j) {
                bool used = r.bit();
                bool useDelta = used ? true : r.bit();
                if (useDelta) ++kept;
            }
            deltas[index] = kept;
        } else {
            uint32_t negative = r.ue();
            uint32_t positive = r.ue();
            if (negative > 16 || positive > 16) return false;
            for (uint32_t j = 0; j < negative + positive && r.ok; ++j) {
                r.ue();
                r.skip(1);
            }
            deltas[index] = negative + positive;
        }
    }
    return r.ok;
}

}

VideoInfo parseAvcSps(const uint8_t* nal, size_t n) {
    VideoInfo info;
    if (n < 5) return info;
    std::vector<uint8_t> body = unescape(nal + 1, n - 1);
    BitReader r{body.data(), body.size()};
    info.profile = int(r.bits(8));
    info.compatibility = int(r.bits(8));
    info.level = int(r.bits(8));
    r.ue();
    static const int extended[] = {100, 110, 122, 244, 44, 83, 86, 118, 128, 138, 139, 134, 135};
    if (std::find(std::begin(extended), std::end(extended), info.profile) != std::end(extended)) {
        const uint32_t chroma = r.ue();
        if (chroma > 3) return info;
        info.chroma = int(chroma);
        if (info.chroma == 3) info.separatePlanes = r.bit();
        const uint32_t depth = r.ue();
        const uint32_t depthChroma = r.ue();
        if (depth > 8 || depthChroma > 8) return info;
        info.bitDepth = int(depth) + 8;
        info.bitDepthChroma = int(depthChroma) + 8;
        r.skip(1);
        if (r.bit()) {
            int lists = info.chroma != 3 ? 8 : 12;
            for (int i = 0; i < lists && r.ok; ++i) if (r.bit()) avcScalingList(r, i < 6 ? 16 : 64);
        }
    }
    const uint32_t frameNumMinus4 = r.ue();
    if (frameNumMinus4 > 12) return info;
    info.frameNumBits = int(frameNumMinus4) + 4;
    uint32_t orderType = r.ue();
    info.orderType = int(orderType);
    if (orderType == 0) {
        info.orderBits = int(r.ue()) + 4;
    } else if (orderType == 1) {
        r.skip(1);
        r.se();
        r.se();
        uint32_t cycle = r.ue();
        if (cycle > 255) return info;
        for (uint32_t i = 0; i < cycle && r.ok; ++i) r.se();
    }
    r.ue();
    r.skip(1);
    uint64_t widthBlocks = uint64_t(r.ue()) + 1;
    uint64_t heightUnits = uint64_t(r.ue()) + 1;
    bool framesOnly = r.bit();
    info.interlaced = !framesOnly;
    if (!framesOnly) r.skip(1);
    r.skip(1);
    uint64_t cropLeft = 0, cropRight = 0, cropTop = 0, cropBottom = 0;
    if (r.bit()) {
        cropLeft = r.ue();
        cropRight = r.ue();
        cropTop = r.ue();
        cropBottom = r.ue();
    }
    if (!r.ok || widthBlocks > 4096 || heightUnits > 4096) return info;
    bool plain = info.chroma == 0 || info.separatePlanes;
    uint64_t unitX = plain || info.chroma == 3 ? 1 : 2;
    uint64_t unitY = (plain || info.chroma != 1 ? 1 : 2) * (framesOnly ? 1 : 2);
    uint64_t width = widthBlocks * 16;
    uint64_t height = heightUnits * 16 * (framesOnly ? 1 : 2);
    uint64_t cutX = unitX * (cropLeft + cropRight);
    uint64_t cutY = unitY * (cropTop + cropBottom);
    if (cutX >= width || cutY >= height) return info;
    info.width = int(width - cutX);
    info.height = int(height - cutY);
    info.valid = true;
    if (r.bit()) {
        aspectAndOverscan(r);
        if (r.bit()) signalType(r, info);
        if (r.bit()) {
            r.ue();
            r.ue();
        }
        if (r.bit()) {
            uint32_t ticks = r.bits(32);
            uint32_t scale = r.bits(32);
            if (r.ok) {
                info.ticks = ticks;
                info.timeScale = scale;
            }
        }
    }
    return info;
}

VideoInfo parseHevcSps(const uint8_t* nal, size_t n) {
    VideoInfo info;
    if (n < 16) return info;
    std::vector<uint8_t> body = unescape(nal + 2, n - 2);
    BitReader r{body.data(), body.size()};
    r.skip(4);
    int subLayersMinus1 = int(r.bits(3));
    info.subLayers = subLayersMinus1 + 1;
    info.nested = r.bit();
    info.profileSpace = int(r.bits(2));
    info.tier = int(r.bit());
    info.profile = int(r.bits(5));
    info.compatibilityFlags = r.bits(32);
    uint64_t constraintHigh = r.bits(16);
    info.constraintFlags = (constraintHigh << 32) | r.bits(32);
    info.level = int(r.bits(8));
    bool profilePresent[8] = {}, levelPresent[8] = {};
    for (int i = 0; i < subLayersMinus1; ++i) {
        profilePresent[i] = r.bit();
        levelPresent[i] = r.bit();
    }
    if (subLayersMinus1 > 0) r.skip(size_t(2 * (8 - subLayersMinus1)));
    for (int i = 0; i < subLayersMinus1; ++i) {
        if (profilePresent[i]) r.skip(88);
        if (levelPresent[i]) r.skip(8);
    }
    r.ue();
    const uint32_t chroma = r.ue();
    if (chroma > 3) return info;
    info.chroma = int(chroma);
    if (info.chroma == 3) info.separatePlanes = r.bit();
    uint64_t width = r.ue();
    uint64_t height = r.ue();
    uint64_t cropLeft = 0, cropRight = 0, cropTop = 0, cropBottom = 0;
    if (r.bit()) {
        cropLeft = r.ue();
        cropRight = r.ue();
        cropTop = r.ue();
        cropBottom = r.ue();
    }
    const uint32_t depth = r.ue();
    const uint32_t depthChroma = r.ue();
    if (!r.ok || depth > 8 || depthChroma > 8 || width > 65536 || height > 65536) return info;
    info.bitDepth = int(depth) + 8;
    info.bitDepthChroma = int(depthChroma) + 8;
    bool plain = info.chroma == 0 || info.separatePlanes;
    uint64_t unitX = plain || info.chroma == 3 ? 1 : 2;
    uint64_t unitY = plain || info.chroma != 1 ? 1 : 2;
    uint64_t cutX = unitX * (cropLeft + cropRight);
    uint64_t cutY = unitY * (cropTop + cropBottom);
    if (cutX >= width || cutY >= height) return info;
    info.width = int(width - cutX);
    info.height = int(height - cutY);
    info.valid = true;
    const uint32_t orderMinus4 = r.ue();
    if (orderMinus4 > 12) return info;
    const uint32_t orderBits = orderMinus4 + 4;
    info.orderBits = int(orderBits);
    bool everyLayer = r.bit();
    for (int i = everyLayer ? 0 : subLayersMinus1; i <= subLayersMinus1; ++i) {
        r.ue();
        r.ue();
        r.ue();
    }
    for (int i = 0; i < 6; ++i) r.ue();
    if (r.bit() && r.bit()) hevcScalingLists(r);
    r.skip(2);
    if (r.bit()) {
        r.skip(8);
        r.ue();
        r.ue();
        r.skip(1);
    }
    if (!r.ok || !hevcShortTermSets(r)) return info;
    if (r.bit()) {
        uint32_t count = r.ue();
        if (count > 32) return info;
        for (uint32_t i = 0; i < count; ++i) r.skip(orderBits + 1);
    }
    r.skip(2);
    if (!r.ok || !r.bit()) return info;
    aspectAndOverscan(r);
    if (r.bit()) signalType(r, info);
    if (r.bit()) {
        r.ue();
        r.ue();
    }
    r.skip(1);
    bool fields = r.bit();
    r.skip(1);
    if (r.ok) info.interlaced = fields;
    if (r.bit()) for (int i = 0; i < 4; ++i) r.ue();
    if (r.bit()) {
        uint32_t ticks = r.bits(32);
        uint32_t scale = r.bits(32);
        if (r.ok) {
            info.ticks = ticks;
            info.timeScale = scale;
        }
    }
    return info;
}

VideoInfo videoInfo(uint32_t configType, const std::vector<uint8_t>& config) {
    View v{config.data(), config.size()};
    if (configType == fourcc("avcC")) {
        v.skip(5);
        uint32_t count = v.u8() & 31;
        if (!count) return {};
        View sps = v.sub(v.u16());
        return v.ok ? parseAvcSps(sps.p, sps.n) : VideoInfo();
    }
    if (configType == fourcc("hvcC")) {
        v.skip(22);
        uint32_t arrays = v.u8();
        for (uint32_t i = 0; i < arrays && v.ok; ++i) {
            uint32_t type = v.u8() & 63;
            uint32_t count = v.u16();
            for (uint32_t k = 0; k < count && v.ok; ++k) {
                View nal = v.sub(v.u16());
                if (v.ok && type == 33) return parseHevcSps(nal.p, nal.n);
            }
        }
    }
    return {};
}

std::vector<uint8_t> buildAvcConfig(const NalList& sps, const NalList& pps) {
    if (sps.empty() || pps.empty() || sps[0].size() < 4) return {};
    VideoInfo info = parseAvcSps(sps[0].data(), sps[0].size());
    Bytes b;
    b.u8(1);
    b.u8(sps[0][1]);
    b.u8(sps[0][2]);
    b.u8(sps[0][3]);
    b.u8(0xFF);
    b.u8(0xE0 | std::min<size_t>(sps.size(), 31));
    for (size_t i = 0; i < sps.size() && i < 31; ++i) {
        b.u16(uint32_t(sps[i].size()));
        b.raw(sps[i].data(), sps[i].size());
    }
    b.u8(uint32_t(std::min<size_t>(pps.size(), 255)));
    for (size_t i = 0; i < pps.size() && i < 255; ++i) {
        b.u16(uint32_t(pps[i].size()));
        b.raw(pps[i].data(), pps[i].size());
    }
    int profile = sps[0][1];
    if (info.valid && profile != 66 && profile != 77 && profile != 88) {
        b.u8(0xFC | info.chroma);
        b.u8(0xF8 | (info.bitDepth - 8));
        b.u8(0xF8 | (info.bitDepthChroma - 8));
        b.u8(0);
    }
    return b.d;
}

std::vector<uint8_t> buildHevcConfig(const NalList& vps, const NalList& sps, const NalList& pps) {
    if (vps.empty() || sps.empty() || pps.empty()) return {};
    VideoInfo info = parseHevcSps(sps[0].data(), sps[0].size());
    if (!info.valid) return {};
    Bytes b;
    b.u8(1);
    b.u8(uint32_t(info.profileSpace << 6 | info.tier << 5 | info.profile));
    b.u32(info.compatibilityFlags);
    b.u16(uint32_t(info.constraintFlags >> 32));
    b.u32(uint32_t(info.constraintFlags));
    b.u8(uint32_t(info.level));
    b.u16(0xF000);
    b.u8(0xFC);
    b.u8(0xFC | info.chroma);
    b.u8(0xF8 | (info.bitDepth - 8));
    b.u8(0xF8 | (info.bitDepthChroma - 8));
    b.u16(0);
    b.u8(uint32_t(info.subLayers << 3 | (info.nested ? 4 : 0) | 3));
    b.u8(3);
    const NalList* lists[3] = {&vps, &sps, &pps};
    for (int i = 0; i < 3; ++i) {
        b.u8(0x80 | (32 + i));
        b.u16(uint32_t(std::min<size_t>(lists[i]->size(), 64)));
        for (size_t k = 0; k < lists[i]->size() && k < 64; ++k) {
            b.u16(uint32_t((*lists[i])[k].size()));
            b.raw((*lists[i])[k].data(), (*lists[i])[k].size());
        }
    }
    return b.d;
}

}
