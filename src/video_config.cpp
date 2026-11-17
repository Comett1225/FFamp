#include "soft_video.h"
#include <algorithm>

namespace absent {

namespace {

struct Bits {
    const uint8_t* p;
    size_t n;
    size_t at = 0;
    bool ok = true;

    uint32_t take(int count) {
        uint32_t value = 0;
        for (int i = 0; i < count; ++i) {
            if (at >= n * 8) {
                ok = false;
                return 0;
            }
            value = (value << 1) | ((p[at >> 3] >> (7 - (at & 7))) & 1);
            ++at;
        }
        return value;
    }
    void skip(int count) {
        if (at + size_t(count) > n * 8) ok = false;
        else at += size_t(count);
    }
    void uvlc() {
        int zeros = 0;
        while (ok && !take(1)) if (++zeros >= 32) return;
        skip(zeros);
    }
    uint32_t ue() {
        int zeros = 0;
        while (ok && !take(1)) {
            if (++zeros > 31) {
                ok = false;
                return 0;
            }
        }
        return ok ? (1u << zeros) - 1 + take(zeros) : 0;
    }
    int32_t se() {
        const uint32_t code = ue();
        return code & 1 ? int32_t((code + 1) / 2) : -int32_t(code / 2);
    }
};

std::vector<uint8_t> unescaped(const uint8_t* p, size_t n, size_t limit) {
    std::vector<uint8_t> out;
    out.reserve(std::min(n, limit));
    int zeros = 0;
    for (size_t i = 0; i < n && out.size() < limit; ++i) {
        if (zeros >= 2 && p[i] == 3) {
            zeros = 0;
            continue;
        }
        zeros = p[i] == 0 ? zeros + 1 : 0;
        out.push_back(p[i]);
    }
    return out;
}

int vp9Matrix(int space) {
    switch (space) {
        case 1: return 5;
        case 2: return 1;
        case 3: return 6;
        case 4: return 7;
        case 5: return 9;
        case 7: return 0;
        default: return 2;
    }
}

int vp9Level(int width, int height, double fps, int bitrate) {
    struct Level { int id; uint64_t rate; uint64_t picture; int kbps; };
    static const Level levels[] = {
        {10, 829440, 36864, 200}, {11, 2764800, 73728, 800}, {20, 4608000, 122880, 1800}, {21, 9216000, 245760, 3600},
        {30, 20736000, 552960, 7200}, {31, 36864000, 983040, 12000}, {40, 83558400, 2228224, 18000}, {41, 160432128, 2228224, 30000},
        {50, 311951360, 8912896, 60000}, {51, 588251136, 8912896, 120000}, {52, 1176502272, 8912896, 180000},
        {60, 1176502272, 35651584, 180000}, {61, 2353004544ull, 35651584, 240000}, {62, 4706009088ull, 35651584, 480000}};
    uint64_t picture = uint64_t(width) * uint64_t(height);
    uint64_t rate = uint64_t(double(picture) * (fps > 0 ? fps : 30));
    for (const Level& level : levels) if (picture <= level.picture && rate <= level.rate && bitrate / 1000 <= level.kbps) return level.id;
    return 62;
}

bool readLeb(const uint8_t* p, size_t n, size_t& at, uint64_t& value) {
    value = 0;
    for (int i = 0; i < 8; ++i) {
        if (at >= n) return false;
        uint8_t byte = p[at++];
        value |= uint64_t(byte & 0x7F) << (7 * i);
        if (!(byte & 0x80)) return true;
    }
    return false;
}

bool sequenceHeader(const uint8_t* p, size_t n, uint8_t bytes[3], StreamShape& shape) {
    Bits b{p, n};
    uint32_t profile = b.take(3);
    b.skip(1);
    bool reduced = b.take(1);
    uint32_t level = 0, tier = 0;
    if (reduced) {
        level = b.take(5);
    } else {
        bool decoderModel = false;
        uint32_t delayBits = 0;
        if (b.take(1)) {
            b.skip(64);
            if (b.take(1)) b.uvlc();
            decoderModel = b.take(1);
            if (decoderModel) {
                delayBits = b.take(5) + 1;
                b.skip(42);
            }
        }
        bool displayDelay = b.take(1);
        uint32_t points = b.take(5) + 1;
        for (uint32_t i = 0; i < points && b.ok; ++i) {
            b.skip(12);
            uint32_t pointLevel = b.take(5);
            uint32_t pointTier = pointLevel > 7 ? b.take(1) : 0;
            if (i == 0) {
                level = pointLevel;
                tier = pointTier;
            }
            if (decoderModel && b.take(1)) b.skip(int(2 * delayBits + 1));
            if (displayDelay && b.take(1)) b.skip(4);
        }
    }
    int widthBits = int(b.take(4)) + 1, heightBits = int(b.take(4)) + 1;
    shape.width = int(b.take(widthBits)) + 1;
    shape.height = int(b.take(heightBits)) + 1;
    if (!reduced && b.take(1)) b.skip(7);
    b.skip(3);
    if (!reduced) {
        b.skip(4);
        bool orderHint = b.take(1);
        if (orderHint) b.skip(2);
        uint32_t screen = b.take(1) ? 2 : b.take(1);
        if (screen > 0 && !b.take(1)) b.skip(1);
        if (orderHint) b.skip(3);
    }
    b.skip(3);
    bool high = b.take(1);
    bool twelve = profile == 2 && high ? b.take(1) : false;
    bool mono = profile == 1 ? false : b.take(1);
    Color color;
    color.primaries = 2;
    color.transfer = 2;
    color.matrix = 2;
    if (b.take(1)) {
        color.primaries = int(b.take(8));
        color.transfer = int(b.take(8));
        color.matrix = int(b.take(8));
        color.known = true;
    }
    uint32_t subX = 1, subY = 1, position = 0;
    if (mono) {
        color.full = b.take(1);
    } else if (color.primaries == 1 && color.transfer == 13 && color.matrix == 0) {
        color.full = true;
        subX = subY = 0;
    } else {
        color.full = b.take(1);
        if (profile == 1) {
            subX = subY = 0;
        } else if (profile == 2) {
            if (twelve) {
                subX = b.take(1);
                subY = subX ? b.take(1) : 0;
            } else {
                subY = 0;
            }
        }
        if (subX && subY) position = b.take(2);
    }
    if (!b.ok) return false;
    shape.bits = twelve ? 12 : high ? 10 : 8;
    shape.chromaX = int(subX);
    shape.chromaY = int(subY);
    shape.color = color;
    bytes[0] = 0x81;
    bytes[1] = uint8_t((profile << 5) | level);
    bytes[2] = uint8_t((tier << 7) | (uint32_t(high) << 6) | (uint32_t(twelve) << 5) | (uint32_t(mono) << 4) | (subX << 3) | (subY << 2) | position);
    return true;
}

}

bool vp9Shape(const uint8_t* frame, size_t size, StreamShape& shape) {
    Bits b{frame, size};
    if (b.take(2) != 2) return false;
    uint32_t profile = b.take(1);
    profile |= b.take(1) << 1;
    if (profile == 3) b.skip(1);
    if (b.take(1)) return false;
    if (b.take(1)) return false;
    b.skip(2);
    if (b.take(24) != 0x498342) return false;
    shape.bits = profile >= 2 ? (b.take(1) ? 12 : 10) : 8;
    uint32_t space = b.take(3);
    shape.chromaX = shape.chromaY = 1;
    Color color;
    color.matrix = vp9Matrix(int(space));
    color.primaries = color.matrix == 9 ? 9 : color.matrix == 1 ? 1 : 2;
    color.transfer = color.matrix == 1 ? 1 : 2;
    color.known = space != 0;
    if (space != 7) {
        color.full = b.take(1);
        if (profile == 1 || profile == 3) {
            shape.chromaX = int(b.take(1));
            shape.chromaY = int(b.take(1));
            b.skip(1);
        }
    } else {
        color.full = true;
        if (profile == 1 || profile == 3) {
            shape.chromaX = shape.chromaY = 0;
            b.skip(1);
        }
    }
    shape.width = int(b.take(16)) + 1;
    shape.height = int(b.take(16)) + 1;
    shape.color = color;
    return b.ok;
}

std::vector<uint8_t> vp9Config(const StreamShape& shape, double fps, int bitrate) {
    int profile = (shape.bits > 8 ? 2 : 0) + (shape.chromaX && shape.chromaY ? 0 : 1);
    int chroma = shape.chromaX && shape.chromaY ? 1 : shape.chromaX ? 2 : 3;
    return {1, 0, 0, 0, uint8_t(profile), uint8_t(vp9Level(shape.width, shape.height, fps, bitrate)),
        uint8_t((shape.bits << 4) | (chroma << 1) | (shape.color.full ? 1 : 0)),
        uint8_t(shape.color.primaries), uint8_t(shape.color.transfer), uint8_t(shape.color.matrix), 0, 0};
}

bool av1Config(const uint8_t* unit, size_t size, std::vector<uint8_t>& config, StreamShape& shape) {
    size_t at = 0;
    while (at < size) {
        size_t start = at;
        uint8_t header = unit[at++];
        if (header & 0x80) return false;
        int type = (header >> 3) & 15;
        if (header & 4) ++at;
        if (!(header & 2)) return false;
        uint64_t length = 0;
        if (!readLeb(unit, size, at, length) || length > size - at) return false;
        if (type == 1) {
            uint8_t bytes[3];
            if (!sequenceHeader(unit + at, size_t(length), bytes, shape)) return false;
            config.assign({bytes[0], bytes[1], bytes[2], 0});
            config.insert(config.end(), unit + start, unit + at + length);
            return true;
        }
        at += size_t(length);
    }
    return false;
}

int mpegChroma(Reader& file, const Track& track) {
    if (track.samples.empty() || track.samples[0].size > (64u << 20)) return 1;
    std::vector<uint8_t> first(track.samples[0].size);
    if (!file.read(track, 0, first.data())) return 1;
    for (size_t i = 0; i + 6 <= first.size(); ++i)
        if (first[i] == 0 && first[i + 1] == 0 && first[i + 2] == 1 && first[i + 3] == 0xB5 && (first[i + 4] >> 4) == 1) return (first[i + 5] >> 1) & 3;
    return 1;
}

bool shapeFromConfig(uint32_t configType, const std::vector<uint8_t>& c, StreamShape& shape) {
    if (configType == fourcc("hvcC") && c.size() >= 23) {
        int chroma = c[16] & 3;
        shape.bits = 8 + (c[17] & 7);
        shape.chromaX = chroma == 3 ? 0 : 1;
        shape.chromaY = chroma == 1 || chroma == 0 ? 1 : 0;
        return true;
    }
    if (configType == fourcc("av1C") && c.size() >= 4) {
        shape.bits = (c[2] & 0x40) ? ((c[2] & 0x20) ? 12 : 10) : 8;
        shape.chromaX = (c[2] >> 3) & 1;
        shape.chromaY = (c[2] >> 2) & 1;
        return true;
    }
    if (configType == fourcc("vpcC") && c.size() >= 7) {
        int chroma = (c[6] >> 1) & 7;
        shape.bits = c[6] >> 4;
        shape.chromaX = chroma == 3 ? 0 : 1;
        shape.chromaY = chroma <= 1 ? 1 : 0;
        return shape.bits == 8 || shape.bits == 10 || shape.bits == 12;
    }
    return false;
}

bool h264Layout(const std::vector<uint8_t>& c, H264Layout& layout) {
    if (c.size() < 8 || (c[5] & 31) < 1) return false;
    layout.lengthSize = (c[4] & 3) + 1;
    const size_t size = (size_t(c[6]) << 8) | c[7];
    if (size < 4 || size > c.size() - 8) return false;
    const std::vector<uint8_t> sps = unescaped(c.data() + 9, size - 1, 4096);
    Bits b{sps.data(), sps.size()};
    const uint32_t profile = b.take(8);
    b.skip(16);
    b.ue();
    static const uint32_t extended[] = {100, 110, 122, 244, 44, 83, 86, 118, 128, 138, 139, 134, 135};
    if (std::find(std::begin(extended), std::end(extended), profile) != std::end(extended)) {
        const uint32_t chroma = b.ue();
        if (chroma == 3) layout.separatePlanes = b.take(1);
        b.ue();
        b.ue();
        b.skip(1);
        if (b.take(1)) {
            for (int i = 0; i < (chroma != 3 ? 8 : 12) && b.ok; ++i) {
                if (!b.take(1)) continue;
                int last = 8, next = 8;
                for (int j = 0; j < (i < 6 ? 16 : 64) && b.ok; ++j) {
                    if (next != 0) next = int((last + int64_t(b.se()) + 256) % 256);
                    last = next == 0 ? last : next;
                }
            }
        }
    }
    const uint32_t frameNumMinus4 = b.ue();
    if (frameNumMinus4 > 12) return false;
    layout.frameNumBits = int(frameNumMinus4) + 4;
    const uint32_t order = b.ue();
    if (order == 0) {
        b.ue();
    } else if (order == 1) {
        b.skip(1);
        b.se();
        b.se();
        const uint32_t cycle = b.ue();
        if (cycle > 255) return false;
        for (uint32_t i = 0; i < cycle; ++i) b.se();
    }
    b.ue();
    b.skip(1);
    b.ue();
    b.ue();
    layout.fields = !b.take(1);
    return b.ok && layout.frameNumBits <= 16;
}

bool h264Intra(const H264Layout& layout, const uint8_t* sample, size_t size) {
    size_t at = 0;
    while (size - at > size_t(layout.lengthSize)) {
        size_t length = 0;
        for (int i = 0; i < layout.lengthSize; ++i) length = (length << 8) | sample[at++];
        if (length > size - at) break;
        const int type = length ? sample[at] & 31 : 0;
        if (type == 5) return true;
        if (type == 1) {
            const std::vector<uint8_t> header = unescaped(sample + at + 1, length - 1, 16);
            Bits b{header.data(), header.size()};
            b.ue();
            const uint32_t kind = b.ue() % 5;
            return b.ok && (kind == 2 || kind == 4);
        }
        at += length;
    }
    return false;
}

int h264FieldOf(const H264Layout& layout, const uint8_t* sample, size_t size, uint32_t& frameNumber) {
    bool top = false, bottom = false, whole = false;
    size_t at = 0;
    while (size - at > size_t(layout.lengthSize)) {
        size_t length = 0;
        for (int i = 0; i < layout.lengthSize; ++i) length = (length << 8) | sample[at++];
        if (length > size - at) break;
        const int type = length ? sample[at] & 31 : 0;
        if (type == 1 || type == 5) {
            const std::vector<uint8_t> header = unescaped(sample + at + 1, length - 1, 64);
            Bits b{header.data(), header.size()};
            b.ue();
            b.ue();
            b.ue();
            if (layout.separatePlanes) b.skip(2);
            frameNumber = b.take(layout.frameNumBits);
            const bool field = b.take(1);
            const bool low = field && b.take(1);
            if (!b.ok) return 0;
            if (!field) whole = true;
            else if (low) bottom = true;
            else top = true;
        }
        at += length;
    }
    if (whole || top == bottom) return 0;
    return top ? 1 : 2;
}

}
