#include "aac_config.h"
#include "absent/media.h"

namespace absent {

namespace {

struct Bits {
    View v;
    uint64_t at = 0;
    bool ok = true;

    uint64_t left() const { return ok ? uint64_t(v.n) * 8 - at : 0; }
    uint32_t take(int count) {
        if (!ok || left() < uint64_t(count)) {
            ok = false;
            return 0;
        }
        uint32_t value = 0;
        for (int i = 0; i < count; ++i, ++at) value = (value << 1) | ((v.p[at >> 3] >> (7 - (at & 7))) & 1);
        return value;
    }
};

const int kRates[16] = {96000, 88200, 64000, 48000, 44100, 32000, 24000, 22050, 16000, 12000, 11025, 8000, 7350, 0, 0, 0};
const int kChannels[16] = {0, 1, 2, 3, 4, 5, 6, 8, 0, 0, 0, 7, 8, 0, 8, 0};

int objectType(Bits& b) {
    int type = int(b.take(5));
    return type == 31 ? 32 + int(b.take(6)) : type;
}

int rate(Bits& b) {
    uint32_t index = b.take(4);
    return index == 15 ? int(b.take(24)) : kRates[index];
}

int programChannels(Bits& b) {
    b.take(10);
    int front = int(b.take(4));
    int side = int(b.take(4));
    int back = int(b.take(4));
    int low = int(b.take(2));
    b.take(7);
    if (b.take(1)) b.take(4);
    if (b.take(1)) b.take(4);
    if (b.take(1)) b.take(3);
    int channels = low;
    for (int i = 0; i < front + side + back; ++i) {
        channels += b.take(1) ? 2 : 1;
        b.take(4);
    }
    return channels;
}

uint32_t descriptorLength(View& v) {
    uint32_t length = 0;
    for (int i = 0; i < 4; ++i) {
        uint8_t byte = v.u8();
        length = (length << 7) | (byte & 0x7F);
        if (!(byte & 0x80)) break;
    }
    return length;
}

}

bool parseElementaryStream(View v, ElementaryStream& out) {
    v.skip(4);
    while (v.left() >= 2) {
        uint8_t tag = v.u8();
        uint32_t length = descriptorLength(v);
        if (!v.ok || length > v.left()) return false;
        if (tag == 0x03) {
            v.skip(2);
            uint8_t flags = v.u8();
            if (flags & 0x80) v.skip(2);
            if (flags & 0x40) v.skip(v.u8());
            if (flags & 0x20) v.skip(2);
        } else if (tag == 0x04) {
            out.objectTypeIndication = v.u8();
            v.skip(12);
            if (v.left() < 2) return v.ok;
            if (v.u8() != 0x05) return true;
            uint32_t size = descriptorLength(v);
            if (!v.ok || size > v.left()) return false;
            out.specific = v.sub(size);
            return true;
        } else {
            v.skip(length);
        }
    }
    return false;
}

bool parseAacConfig(View specific, AacConfig& out) {
    Bits b{specific};
    out.objectType = objectType(b);
    out.rate = rate(b);
    uint32_t layout = b.take(4);
    out.channels = kChannels[layout];
    out.outputRate = out.rate;
    int core = out.objectType;
    if (core == 5 || core == 29) {
        out.spectralBandReplication = true;
        out.parametricStereo = core == 29;
        out.outputRate = rate(b);
        core = objectType(b);
        if (core == 22) b.take(4);
    }
    if (!b.ok || out.rate <= 0) return false;
    bool general = core == 1 || core == 2 || core == 3 || core == 4 || core == 6 || core == 7 || core == 17 || (core >= 19 && core <= 23);
    if (general) {
        if (b.take(1)) out.frameLength = 960;
        if (b.take(1)) b.take(14);
        uint32_t extension = b.take(1);
        if (layout == 0) out.channels = programChannels(b);
        if (core == 6 || core == 20) b.take(3);
        if (extension) {
            if (core == 22) b.take(16);
            if (core == 17 || core == 19 || core == 20 || core == 23) b.take(3);
            b.take(1);
        }
        if (!b.ok) return false;
        if (layout != 0 && !out.spectralBandReplication && b.left() >= 16 && b.take(11) == 0x2B7) {
            if (objectType(b) == 5 && b.take(1)) {
                out.spectralBandReplication = true;
                out.outputRate = rate(b);
                if (b.left() >= 12 && b.take(11) == 0x548) out.parametricStereo = b.take(1);
            }
            if (!b.ok || out.outputRate <= 0) {
                out.spectralBandReplication = false;
                out.parametricStereo = false;
                out.outputRate = out.rate;
            }
        }
    }
    out.outputChannels = out.parametricStereo && out.channels == 1 ? 2 : out.channels;
    return out.channels > 0;
}

bool aacConfigOf(uint32_t configType, const std::vector<uint8_t>& config, AacConfig& out) {
    View v{config.data(), config.size()};
    if (configType == fourcc("esds")) {
        ElementaryStream stream;
        if (!parseElementaryStream(v, stream)) return false;
        v = stream.specific;
    }
    return parseAacConfig(v, out);
}

}
