#include "nal.h"
#include "read_common.h"
#include "readers.h"
#include <algorithm>
#include <cstring>
#include <numeric>

namespace absent {

void riffInfo(Source& source, uint64_t at, uint64_t size, Tags& tags);

std::vector<uint8_t> mpeg4Descriptor(const uint8_t* headers, size_t size) {
    size = std::min<size_t>(size, 1 << 16);
    Bytes b;
    auto open = [&](uint32_t tag, size_t length) {
        b.u8(tag);
        b.u8(0x80 | ((length >> 21) & 0x7F));
        b.u8(0x80 | ((length >> 14) & 0x7F));
        b.u8(0x80 | ((length >> 7) & 0x7F));
        b.u8(length & 0x7F);
    };
    b.u32(0);
    open(0x03, 3 + 5 + 13 + 5 + size + 6);
    b.u16(0);
    b.u8(0);
    open(0x04, 13 + 5 + size);
    b.u8(0x20);
    b.u8(0x11);
    b.u24(0);
    b.u32(0);
    b.u32(0);
    open(0x05, size);
    b.raw(headers, size);
    open(0x06, 1);
    b.u8(0x02);
    return b.d;
}

uint32_t videoFourcc(uint32_t handler, int height) {
    char name[5] = {char(handler >> 24), char(handler >> 16), char(handler >> 8), char(handler), 0};
    for (char& c : name) if (c >= 'a' && c <= 'z') c = char(c - 32);
    auto any = [&](std::initializer_list<const char*> list) {
        for (const char* item : list) if (memcmp(name, item, 4) == 0) return true;
        return false;
    };
    if (any({"XVID", "DIVX", "DX50", "FMP4", "MP4V", "M4S2", "MP4S", "3IV2", "RMP4", "SEDG", "FVFW", "ZMP4", "DIV1", "BLZ0", "UMP4", "WV1F", "WAWV", "FFDS",
            "DCOD", "MVXM", "PM4V", "SMP4", "DXGM", "VIDM", "M4T3", "GEOX", "G264", "HDX4", "DM4V", "DMK2", "DYM4", "DIGI", "EPHV", "EM4A", "M4CC", "SN40",
            "VSPX", "ULDX", "GEOV", "SIPP", "SM4V", "XVIX", "DREX", "QMP4", "PLV1", "GLV4", "GMP4", "MNM4", "GTM4"}) ||
        handler == 0x04000000 || handler == 0x04000010)
        return fourcc("mp4v");
    if (any({"MP43", "DIV3", "MPG3", "DIV5", "DIV6", "DIV4", "DVX3", "AP41", "COL1", "COL0"})) return fourcc("MP43");
    if (any({"MP42", "DIV2"})) return fourcc("MP42");
    if (any({"MPG4", "MP41"})) return fourcc("MPG4");
    if (any({"WMV1"})) return fourcc("WMV1");
    if (any({"WMV2", "GXVE"})) return fourcc("WMV2");
    if (any({"H264", "X264", "AVC1", "VSSH", "DAVC", "SMV2", "Q264", "V264", "GAVC", "UMSV", "TSHD", "INMC"})) return fourcc("avc1");
    if (any({"HEVC", "H265", "HEV1", "HVC1", "DVHE"})) return fourcc("hvc1");
    if (any({"MJPG", "JPEG", "IJPG", "AVRN", "DMB1", "MSC2", "LJPG", "MJPA", "JR24", "JPGL", "ACDV", "QIVG", "SLMJ", "CJPG", "IJLV", "MVJP", "AVI1", "AVI2",
            "MTSJ", "ZJPG", "MMJP"}))
        return fourcc("jpeg");
    if (any({"DVSD", "DV25", "DVSL", "DVC ", "DVCP", "DVIS", "PDVC", "CDVC", "DVCS", "SL25", "SLDV", "AVD1"})) return height == 480 ? fourcc("dvc ") : fourcc("dvcp");
    if (any({"DV50", "DV5N", "DV5P", "CDV5"})) return height == 480 ? fourcc("dv5n") : fourcc("dv5p");
    if (any({"DVHD", "DVH1", "CDVH"})) return height == 720 ? fourcc("dvhp") : fourcc("dvh6");
    if (any({"MPG1", "MPEG", "MP1V", "PIM1", "BW10", "XMPG"}) || handler == 0x01000010) return fourcc("mp1v");
    if (any({"MPG2", "MP2V", "MMES", "M2V1", "PIM2", "VCR2", "DVR ", "LMP2", "SLIF", "EM2V", "M701", "M702", "M703", "M704", "M705", "MPGV"}) || handler == 0x02000010)
        return fourcc("mp2v");
    if (any({"H263", "S263", "X263", "T263", "L263", "VX1K", "ZYGO", "M263", "LSVM", "U263"})) return fourcc("h263");
    if (any({"CVID"})) return fourcc("cvid");
    if (any({"MPNG"})) return fourcc("png ");
    if (!handler || any({"DIB ", "RGB ", "RAW "})) return fourcc("DIB ");
    return 0;
}

namespace {

const uint32_t kPacketFrames = 4096;
const uint64_t kLargestChunk = 256u << 20;
const size_t kMostChunks = 1u << 26;
const size_t kHead = 256;

struct Chunk {
    uint64_t at = 0;
    uint32_t size = 0;
    bool key = true;
};

struct Stream {
    Track track;
    bool video = false;
    bool audio = false;
    bool tape = false;
    bool hevc = false;
    uint32_t handler = 0;
    uint32_t scale = 0;
    uint32_t rate = 0;
    uint32_t start = 0;
    uint32_t tag = 0;
    uint32_t compression = 0;
    uint64_t indexAt = 0;
    uint64_t indexSize = 0;
    std::vector<uint8_t> extra;
    std::vector<uint8_t> bitmap;
    std::vector<Chunk> chunks;
    std::vector<uint64_t> ends;
    std::vector<uint32_t> raw;
};

struct Shown {
    uint64_t at = 0;
    uint32_t size = 0;
    uint32_t raw = 0;
    int64_t count = -1;
    int type = 0;
    bool empty = false;
    bool key = true;
};

struct OrderFields {
    bool usable = false;
    bool separate = false;
    bool framesOnly = true;
    int numberBits = 4;
    int countBits = 4;
};

OrderFields orderFields(const std::vector<uint8_t>& set) {
    OrderFields f;
    std::vector<uint8_t> plain = unescape(set.data(), set.size());
    if (plain.size() < 5) return f;
    BitReader r{plain.data() + 1, plain.size() - 1};
    int profile = int(r.bits(8));
    r.skip(16);
    r.ue();
    if (profile == 100 || profile == 110 || profile == 122 || profile == 244 || profile == 44 || profile == 83 || profile == 86 || profile == 118 || profile == 128) {
        uint32_t chroma = r.ue();
        if (chroma == 3) f.separate = r.bit();
        r.ue();
        r.ue();
        r.skip(1);
        if (r.bit()) {
            for (int i = 0; i < (chroma == 3 ? 12 : 8); ++i) {
                if (!r.bit()) continue;
                int last = 8, next = 8;
                for (int j = 0; j < (i < 6 ? 16 : 64) && r.ok; ++j) {
                    if (next) next = (last + r.se() + 256) % 256;
                    last = next ? next : last;
                }
            }
        }
    }
    f.numberBits = int(r.ue()) + 4;
    uint32_t kind = r.ue();
    if (kind == 0) f.countBits = int(r.ue()) + 4;
    else if (kind == 1) return f;
    r.ue();
    r.skip(1);
    r.ue();
    r.ue();
    f.framesOnly = r.bit();
    f.usable = r.ok && kind == 0 && f.numberBits >= 4 && f.numberBits <= 16 && f.countBits >= 4 && f.countBits <= 16;
    return f;
}

struct HevcFields {
    bool usable = false;
    bool separate = false;
    bool outputFlag = false;
    int extraBits = 0;
    int countBits = 4;
};

bool dropped(const uint8_t* nal, size_t size, bool hevc) {
    if (hevc) {
        const int type = (nal[0] >> 1) & 63;
        return size < 2 || type == 35 || type == 38;
    }
    const int type = nal[0] & 31;
    return type == 9 || type == 12;
}

void hevcPps(const uint8_t* nal, size_t size, HevcFields& f) {
    if (size < 3) return;
    std::vector<uint8_t> plain = unescape(nal + 2, std::min<size_t>(size - 2, 16));
    BitReader r{plain.data(), plain.size()};
    r.ue();
    r.ue();
    r.skip(1);
    f.outputFlag = r.bit();
    f.extraBits = int(r.bits(3));
    f.usable = r.ok;
}

int64_t hevcCount(const uint8_t* nal, size_t size, const HevcFields& f) {
    const int type = (nal[0] >> 1) & 63;
    std::vector<uint8_t> plain = unescape(nal + 2, std::min<size_t>(size - 2, 48));
    BitReader r{plain.data(), plain.size()};
    if (!r.bit()) return -1;
    if (type >= 16 && type <= 23) r.skip(1);
    r.ue();
    r.skip(size_t(f.extraBits));
    r.ue();
    if (f.outputFlag) r.skip(1);
    if (f.separate) r.skip(2);
    const uint32_t low = type == 19 || type == 20 ? 0 : r.bits(f.countBits);
    return r.ok ? int64_t(low) : -1;
}

template <class F>
void eachNal(const uint8_t* p, size_t n, size_t lengthSize, F each) {
    if (!lengthSize) {
        annexNals(p, n, each);
        return;
    }
    size_t at = 0;
    while (n - at >= lengthSize) {
        size_t size = 0;
        for (size_t k = 0; k < lengthSize; ++k) size = size << 8 | p[at + k];
        at += lengthSize;
        if (size == 0 || size > n - at) return;
        each(p + at, size);
        at += size;
    }
}

template <class F>
void configNals(const std::vector<uint8_t>& config, bool hevc, F each) {
    View v{config.data(), config.size()};
    if (hevc) {
        v.skip(22);
        const uint32_t arrays = v.u8();
        for (uint32_t i = 0; i < arrays && v.ok; ++i) {
            v.skip(1);
            const uint32_t count = v.u16();
            for (uint32_t k = 0; k < count && v.ok; ++k) {
                View nal = v.sub(v.u16());
                if (v.ok && nal.n) each(nal.p, nal.n);
            }
        }
        return;
    }
    v.skip(5);
    for (int list = 0; list < 2 && v.ok; ++list) {
        const uint32_t count = list ? v.u8() : v.u8() & 31;
        for (uint32_t k = 0; k < count && v.ok; ++k) {
            View nal = v.sub(v.u16());
            if (v.ok && nal.n) each(nal.p, nal.n);
        }
    }
}

bool packedMark(const uint8_t* p, size_t n) {
    static const char mark[] = "DivX";
    const uint8_t* end = p + n;
    const uint8_t* last = std::search(p, end, mark, mark + 4);
    if (last == end) return false;
    while (last < end && *last >= 0x20 && *last < 0x7F) ++last;
    return last[-1] == 'p';
}

int vopType(const uint8_t* p, size_t n, size_t from, size_t* found) {
    for (size_t i = from; i + 4 < n; ++i) {
        if (p[i] == 0 && p[i + 1] == 0 && p[i + 2] == 1 && p[i + 3] == 0xB6) {
            *found = i;
            return p[i + 4] >> 6;
        }
    }
    return -1;
}

int pictureType(const uint8_t* p, size_t n) {
    for (size_t i = 0; i + 5 < n; ++i) {
        if (p[i] == 0 && p[i + 1] == 0 && p[i + 2] == 1 && p[i + 3] == 0) {
            int type = (p[i + 5] >> 3) & 7;
            return type == 1 ? 0 : type == 3 ? 2 : 1;
        }
    }
    return -1;
}

int cinepakType(const uint8_t* p, size_t n) {
    if (n < 10) return -1;
    const size_t strips = be16(p + 8);
    if (!strips) return -1;
    size_t at = 10;
    for (size_t i = 0; i < strips; ++i) {
        if (n - at < 12) return -1;
        if (p[at] != 0x10) return 1;
        const size_t end = std::min(n, at + std::max<size_t>(be24(p + at + 1), 12));
        for (size_t c = at + 12; c + 4 <= end;) {
            const uint8_t id = p[c];
            if ((id >= 0x20 && id <= 0x27 && (id & 1)) || id == 0x31) return 1;
            c += std::max<size_t>(be24(p + c + 1), 4);
        }
        at = end;
    }
    return 0;
}

const uint8_t* dvSoundPack(const uint8_t* frame, size_t size) {
    for (int c = 0; c < 10; ++c) {
        const size_t at = size_t(c & 1 ? 80 * 6 + 3 : 80 * 6 + 80 * 16 * 3 + 3) + size_t(c) * 12000;
        if (at + 5 > size) return nullptr;
        if (frame[at] == 0x50) return frame + at;
    }
    return nullptr;
}

struct DvSound {
    int rate = 0;
    int quant = 0;
    uint32_t frames = 0;
};

bool dvSound(const uint8_t* frame, size_t size, DvSound& out) {
    static const int rates[3] = {48000, 44100, 32000};
    static const int fewest[2][3] = {{1580, 1452, 1053}, {1896, 1742, 1264}};
    if (size < 80 * 150 * 10) return false;
    const uint8_t* pack = dvSoundPack(frame, size);
    if (!pack) return false;
    const int freq = (pack[4] >> 3) & 7, quant = pack[4] & 7;
    if (freq > 2 || quant > 1) return false;
    const bool pal = frame[3] & 0x80;
    if (size < size_t(80 * 150 * (pal ? 12 : 10))) return false;
    out.rate = rates[freq];
    out.quant = quant;
    out.frames = uint32_t(fewest[pal][freq] + (pack[1] & 0x3F));
    return true;
}

uint16_t dvTwelve(uint16_t sample) {
    const uint16_t wide = sample < 0x800 ? sample : uint16_t(sample | 0xF000);
    int shift = (wide & 0xF00) >> 8;
    if (shift < 2 || shift > 0xD) return wide;
    if (shift < 8) {
        --shift;
        return uint16_t((wide - 256 * shift) << shift);
    }
    shift = 0xE - shift;
    return uint16_t(((wide + (256 * shift + 1)) << shift) - 1);
}

bool dvPcm(const uint8_t* frame, size_t size, uint8_t* out, size_t bytes) {
    static const uint8_t ntsc[10][9] = {{0, 30, 60, 20, 50, 80, 10, 40, 70}, {6, 36, 66, 26, 56, 86, 16, 46, 76}, {12, 42, 72, 2, 32, 62, 22, 52, 82},
        {18, 48, 78, 8, 38, 68, 28, 58, 88}, {24, 54, 84, 14, 44, 74, 4, 34, 64}, {1, 31, 61, 21, 51, 81, 11, 41, 71}, {7, 37, 67, 27, 57, 87, 17, 47, 77},
        {13, 43, 73, 3, 33, 63, 23, 53, 83}, {19, 49, 79, 9, 39, 69, 29, 59, 89}, {25, 55, 85, 15, 45, 75, 5, 35, 65}};
    static const uint8_t pal[12][9] = {{0, 36, 72, 26, 62, 98, 16, 52, 88}, {6, 42, 78, 32, 68, 104, 22, 58, 94}, {12, 48, 84, 2, 38, 74, 28, 64, 100},
        {18, 54, 90, 8, 44, 80, 34, 70, 106}, {24, 60, 96, 14, 50, 86, 4, 40, 76}, {30, 66, 102, 20, 56, 92, 10, 46, 82}, {1, 37, 73, 27, 63, 99, 17, 53, 89},
        {7, 43, 79, 33, 69, 105, 23, 59, 95}, {13, 49, 85, 3, 39, 75, 29, 65, 101}, {19, 55, 91, 9, 45, 81, 35, 71, 107}, {25, 61, 97, 15, 51, 87, 5, 41, 77},
        {31, 67, 103, 21, 57, 93, 11, 47, 83}};
    DvSound sound;
    if (!dvSound(frame, size, sound) || bytes != size_t(sound.frames) * 4) return false;
    memset(out, 0, bytes);
    const bool wide = frame[3] & 0x80;
    const int segments = wide ? 12 : 10, stride = wide ? 108 : 90, half = segments / 2;
    auto place = [&](int row, int column) { return wide ? pal[row][column] : ntsc[row][column]; };
    const uint8_t* p = frame;
    for (int i = 0; i < segments; ++i) {
        p += 6 * 80;
        if (sound.quant == 1 && i == half) break;
        for (int j = 0; j < 9; ++j, p += 16 * 80) {
            for (int d = 8; d < 80; d += 2) {
                if (sound.quant == 0) {
                    const size_t at = size_t(place(i, j) + (d - 8) / 2 * stride) * 2;
                    if (at + 1 >= bytes) continue;
                    out[at] = p[d + 1];
                    out[at + 1] = p[d] == 0x80 && p[d + 1] == 0 ? 0 : p[d];
                    continue;
                }
                uint16_t left = uint16_t((p[d] << 4) | (p[d + 2] >> 4)), right = uint16_t((p[d + 1] << 4) | (p[d + 2] & 15));
                left = left == 0x800 ? 0 : dvTwelve(left);
                right = right == 0x800 ? 0 : dvTwelve(right);
                const size_t at = size_t(place(i % half, j) + (d - 8) / 3 * stride) * 2;
                const size_t other = size_t(place(i % half + half, j) + (d - 8) / 3 * stride) * 2;
                ++d;
                if (at + 1 >= bytes) continue;
                out[at] = uint8_t(left);
                out[at + 1] = uint8_t(left >> 8);
                if (other + 1 < bytes) {
                    out[other] = uint8_t(right);
                    out[other + 1] = uint8_t(right >> 8);
                }
            }
        }
    }
    return true;
}

bool intraOnly(uint32_t codec) {
    return codec == fourcc("jpeg") || (codec >> 16) == 0x6476 || rawVideoCodec(codec);
}

class AviReader : public FileReader {
public:
    std::vector<Stream> streams;
    std::vector<std::pair<uint64_t, uint64_t>> movies;
    uint64_t oldIndexAt = 0;
    uint64_t oldIndexSize = 0;
    uint32_t framePeriod = 0;
    bool indexed = false;
    std::vector<uint8_t> scratch;

    const Stream* streamOf(const Track& track) const {
        for (const Stream& s : streams) if (s.track.id == track.id) return &s;
        return nullptr;
    }

    bool fetch(const Stream& s, uint64_t from, uint8_t* out, size_t count) {
        size_t slot = size_t(std::upper_bound(s.ends.begin(), s.ends.end(), from) - s.ends.begin());
        while (count) {
            if (slot >= s.chunks.size()) return false;
            const uint64_t begins = slot ? s.ends[slot - 1] : 0;
            const uint64_t inside = from - begins;
            const size_t take = size_t(std::min<uint64_t>(count, s.chunks[slot].size - inside));
            const uint8_t* p = source.view(s.chunks[slot].at + inside, take);
            if (!p) return false;
            memcpy(out, p, take);
            out += take;
            from += take;
            count -= take;
            ++slot;
        }
        return true;
    }

    bool read(const Track& track, uint32_t index, uint8_t* out) override {
        const Stream* s = streamOf(track);
        if (!s || index >= track.samples.size()) return false;
        const Sample& sample = track.samples[index];
        if (s->audio && s->tape) {
            if (sample.offset >= s->chunks.size()) return false;
            const Chunk& c = s->chunks[size_t(sample.offset)];
            return source.load(c.at, c.size, scratch, kLargestChunk) && dvPcm(scratch.data(), scratch.size(), out, sample.size);
        }
        if (s->audio) return fetch(*s, sample.offset, out, sample.size);
        if (s->raw.empty()) return source.read(sample.offset, out, sample.size);
        if (index >= s->raw.size() || !source.load(sample.offset, s->raw[index], scratch, kLargestChunk)) return false;
        size_t total = 0;
        bool fits = true;
        const bool hevc = s->hevc;
        annexNals(scratch.data(), scratch.size(), [&](const uint8_t* nal, size_t size) {
            if (dropped(nal, size, hevc) || !fits) return;
            if (size + 4 > sample.size - total) {
                fits = false;
                return;
            }
            out[total] = uint8_t(size >> 24);
            out[total + 1] = uint8_t(size >> 16);
            out[total + 2] = uint8_t(size >> 8);
            out[total + 3] = uint8_t(size);
            memcpy(out + total + 4, nal, size);
            total += size + 4;
        });
        return fits && total == sample.size;
    }

    void streamHeader(uint64_t at, uint64_t end) {
        Stream s;
        uint32_t kind = 0;
        while (end - at >= 8) {
            const uint8_t* h = source.view(at, 8);
            if (!h) break;
            uint32_t id = be32(h);
            uint64_t size = le32(h + 4);
            uint64_t body = at + 8;
            if (size > end - body) size = end - body;
            if (id == fourcc("strh") && size >= 36) {
                const uint8_t* p = source.view(body, 36);
                if (!p) break;
                kind = be32(p);
                s.handler = be32(p + 4);
                if ((kind == fourcc("iavs") || kind == fourcc("ivas")) && streams.empty()) {
                    s.video = s.tape = true;
                    s.track.kind = Kind::Video;
                }
                s.scale = le32(p + 20);
                s.rate = le32(p + 24);
                s.start = le32(p + 28);
            } else if (id == fourcc("strf") && kind == fourcc("vids") && size >= 40) {
                std::vector<uint8_t> format;
                if (!source.load(body, size, format, 1 << 20)) break;
                s.video = true;
                s.track.kind = Kind::Video;
                s.track.width = int(le32(format.data() + 4));
                int32_t height = int32_t(le32(format.data() + 8));
                s.track.height = height < 0 ? -height : height;
                s.track.bits = int(le16(format.data() + 14));
                s.compression = be32(format.data() + 16);
                if (s.compression) s.handler = s.compression;
                s.extra.assign(format.begin() + 40, format.end());
                s.bitmap = format;
            } else if (id == fourcc("strf") && kind == fourcc("auds") && size >= 16) {
                std::vector<uint8_t> format;
                if (!source.load(body, size, format, 1 << 20)) break;
                s.audio = true;
                s.track.kind = Kind::Audio;
                s.tag = le16(format.data());
                s.track.channels = int(le16(format.data() + 2));
                s.track.sampleRate = int(le32(format.data() + 4));
                s.track.bits = int(le16(format.data() + 14));
                if (s.tag == 0xFFFE && format.size() >= 40) s.track.channelMask = le32(format.data() + 20);
                s.tag = extensibleTag(format.data(), format.size());
                s.extra = format;
            } else if (id == fourcc("indx")) {
                s.indexAt = body;
                s.indexSize = size;
            }
            at = body + size + (size & 1);
        }
        if (!s.scale || !s.rate) {
            s.scale = framePeriod ? framePeriod : 1;
            s.rate = framePeriod ? 1000000 : 25;
        }
        s.track.id = uint32_t(streams.size() + 1);
        streams.push_back(std::move(s));
    }

    void header(uint64_t at, uint64_t end) {
        while (end - at >= 12) {
            const uint8_t* h = source.view(at, 12);
            if (!h) break;
            uint64_t size = le32(h + 4);
            uint64_t body = at + 8;
            if (size > end - body) size = end - body;
            if (be32(h) == fourcc("avih") && size >= 4) framePeriod = le32(h + 8);
            if (be32(h) == fourcc("LIST") && be32(h + 8) == fourcc("strl") && size >= 4 && streams.size() < 100) streamHeader(body + 4, body + size);
            at = body + size + (size & 1);
        }
    }

    void outline() {
        const uint64_t total = source.size();
        uint64_t at = 0;
        while (total - at >= 12) {
            const uint8_t* h = source.view(at, 12);
            if (!h || be32(h) != fourcc("RIFF")) break;
            uint64_t size = le32(h + 4);
            uint64_t end = size < 4 || size > total - at - 8 ? total : at + 8 + size;
            uint64_t inner = at + 12;
            while (end - inner >= 8) {
                const uint8_t* c = source.view(inner, std::min<uint64_t>(12, end - inner));
                if (!c) break;
                uint32_t id = be32(c);
                uint64_t length = le32(c + 4);
                uint64_t body = inner + 8;
                bool open = length > end - body;
                if (open) length = end - body;
                if (id == fourcc("LIST") && length >= 4) {
                    uint32_t kind = be32(c + 8);
                    if (kind == fourcc("hdrl") && streams.empty()) header(body + 4, body + length);
                    else if (kind == fourcc("movi")) movies.push_back({body, open || length < 8 ? end : body + length});
                    else if (kind == fourcc("INFO")) riffInfo(source, body + 4, length - 4, found);
                } else if (id == fourcc("idx1") && !oldIndexSize) {
                    oldIndexAt = body;
                    oldIndexSize = length;
                }
                inner = body + length + (length & 1);
            }
            at = end + (end & 1 && end < total ? 1 : 0);
        }
    }

    int streamNumber(const uint8_t* id) const {
        auto digit = [](uint8_t c) { return c >= '0' && c <= '9' ? c - '0' : c >= 'a' && c <= 'f' ? c - 'a' + 10 : c >= 'A' && c <= 'F' ? c - 'A' + 10 : -1; };
        int high = digit(id[0]), low = digit(id[1]);
        if (high < 0 || low < 0 || size_t(high * 16 + low) >= streams.size()) return -1;
        return high * 16 + low;
    }

    Stream* numbered(const uint8_t* id) {
        const int n = streamNumber(id);
        if (n < 0) return nullptr;
        Stream& s = streams[size_t(n)];
        if (s.tape) return id[2] == 'i' && id[3] == 'x' ? nullptr : &s;
        bool sound = id[2] == 'w' && id[3] == 'b';
        bool picture = id[2] == 'd' && (id[3] == 'c' || id[3] == 'b');
        return (s.audio && sound) || (s.video && picture) ? &s : nullptr;
    }

    bool known(const uint8_t* h, uint64_t room, bool whole) {
        const uint32_t id = be32(h);
        const uint64_t size = le32(h + 4);
        if (id == fourcc("LIST")) return room >= 12;
        if (whole && size > room - 8) return false;
        if (id == fourcc("JUNK") || id == fourcc("idx1") || id == fourcc("indx")) return true;
        if (h[0] == 'i' && h[1] == 'x') return streamNumber(h + 2) >= 0;
        if (streamNumber(h) < 0) return false;
        if (h[2] == 'i' && h[3] == 'x') return true;
        auto letter = [](uint8_t c) { return (c >= 'a' && c <= 'z') || c == '_'; };
        return letter(h[2]) && letter(h[3]);
    }

    uint64_t resync(uint64_t at, uint64_t end) {
        while (at < end && end - at >= 8) {
            const size_t count = size_t(std::min<uint64_t>(end - at, 1 << 16));
            const uint8_t* p = source.view(at, count);
            if (!p) return end;
            for (size_t i = 0; i + 8 <= count; ++i) {
                if (known(p + i, end - at - i, true)) return at + i;
            }
            if (count < 8) break;
            at += count - 7;
        }
        return end;
    }

    void keep(Stream& s, uint64_t at, uint64_t size, bool key = true) {
        if (size > kLargestChunk || at > source.size() || size > source.size() - at || s.chunks.size() >= kMostChunks) return;
        s.chunks.push_back({at, uint32_t(size), key});
    }

    void standardIndex(Stream& s, uint64_t at, uint64_t size) {
        if (size < 24) return;
        const uint8_t* h = source.view(at, 24);
        if (!h || h[3] != 1) return;
        uint64_t stride = uint64_t(le16(h)) * 4;
        uint64_t count = le32(h + 4);
        uint64_t base = le64(h + 12);
        if (base >= source.size() && (base >> 32) == (base & 0xFFFFFFFFu) && (base & 0xFFFFFFFFu) < source.size()) base &= 0xFFFFFFFFu;
        if (stride < 8 || count > (size - 24) / stride) return;
        for (uint64_t i = 0; i < count; ++i) {
            const uint8_t* e = source.view(at + 24 + i * stride, 8);
            if (!e) return;
            const uint32_t length = le32(e + 4);
            keep(s, base + le32(e), length & 0x7FFFFFFF, !(length & 0x80000000u));
        }
    }

    void superIndex(Stream& s, uint64_t at, uint64_t size, int depth) {
        if (size < 24 || depth > 8) return;
        const uint8_t* h = source.view(at, 24);
        if (!h) return;
        if (h[3] == 1) {
            standardIndex(s, at, size);
            return;
        }
        if (h[3] != 0 || le16(h) != 4) return;
        uint64_t count = std::min<uint64_t>(le32(h + 4), (size - 24) / 16);
        for (uint64_t i = 0; i < count; ++i) {
            const uint8_t* e = source.view(at + 24 + i * 16, 16);
            if (!e) break;
            uint64_t place = le64(e);
            uint64_t length = le32(e + 8);
            if (length < 8 || place > source.size() || length > source.size() - place) continue;
            superIndex(s, place + 8, length - 8, depth + 1);
        }
    }

    bool openIndexes() {
        bool any = false;
        for (Stream& s : streams) {
            if (s.indexSize >= 24) superIndex(s, s.indexAt, s.indexSize, 0);
            any = any || !s.chunks.empty();
        }
        return any;
    }

    uint64_t firstChunk() {
        for (const auto& movie : movies) {
            const uint64_t found = resync(movie.first + 4, movie.second);
            if (found < movie.second) return found;
        }
        return 0;
    }

    bool oldIndex() {
        if (oldIndexSize < 16 || movies.empty()) return false;
        const uint64_t count = oldIndexSize / 16;
        std::vector<uint8_t> table;
        if (!source.load(oldIndexAt, count * 16, table, uint64_t(kMostChunks) * 16)) return false;
        auto matches = [&](const uint8_t* e, uint64_t base) {
            const uint8_t* there = source.view(base + le32(e + 8), 8);
            return there && memcmp(there, e, 4) == 0 && le32(there + 4) == le32(e + 12);
        };
        uint64_t base = 0;
        const uint8_t* first = nullptr;
        std::vector<size_t> listed;
        for (uint64_t i = 0; i < count; ++i) {
            const uint8_t* e = table.data() + i * 16;
            if (!numbered(e) || !le32(e + 12)) continue;
            if (!first) first = e;
            listed.push_back(size_t(i));
        }
        if (!first) return false;
        const uint64_t starts = firstChunk();
        const uint64_t candidates[3] = {movies[0].first, 0, starts >= le32(first + 8) ? starts - le32(first + 8) : 0};
        bool based = false;
        for (uint64_t candidate : candidates) {
            if (!matches(first, candidate)) continue;
            bool steady = true;
            const size_t step = std::max<size_t>(1, listed.size() / 64);
            for (size_t k = 0; k < listed.size() && steady; k += step) steady = matches(table.data() + listed[k] * 16, candidate);
            if (steady && matches(table.data() + listed.back() * 16, candidate)) {
                base = candidate;
                based = true;
                break;
            }
        }
        if (!based) return false;
        bool anyKey = false;
        for (uint64_t i = 0; i < count; ++i) {
            const uint8_t* e = table.data() + i * 16;
            Stream* s = numbered(e);
            if (!s) continue;
            const bool key = le32(e + 4) & 0x10;
            anyKey = anyKey || key;
            keep(*s, base + le32(e + 8) + 8, le32(e + 12), key);
        }
        if (!anyKey) {
            for (Stream& s : streams) {
                for (size_t k = 0; k < s.chunks.size(); ++k) s.chunks[k].key = k == 0;
            }
        }
        return true;
    }

    void walk(uint64_t from = 0) {
        for (auto& movie : movies) {
            const uint64_t end = movie.second;
            uint64_t at = std::max(movie.first + 4, from);
            if (at >= end) continue;
            if (at > movie.first + 4) at = resync(at, end);
            while (at < end && end - at >= 8) {
                const uint8_t* h = source.view(at, 8);
                if (!h) return;
                if (!known(h, end - at, false)) {
                    at = resync(at + 1, end);
                    continue;
                }
                if (be32(h) == fourcc("LIST")) {
                    at += 12;
                    continue;
                }
                uint64_t size = le32(h + 4);
                if (size > end - at - 8) size = end - at - 8;
                uint8_t id[4];
                memcpy(id, h, 4);
                if (Stream* s = numbered(id)) keep(*s, at + 8, size);
                uint64_t next = at + 8 + size;
                if ((size & 1) && end - next >= 9) {
                    uint8_t both[9];
                    if (source.read(next, both, 9) && !known(both + 1, end - next - 1, true) && known(both, end - next, true)) next -= 1;
                    next += 1;
                } else if (size & 1) {
                    next += 1;
                }
                at = next;
            }
        }
    }

    void rest() {
        uint64_t last = 0;
        for (const Stream& s : streams) {
            for (const Chunk& c : s.chunks) last = std::max(last, c.at + c.size);
        }
        if (!last) return;
        std::vector<size_t> before(streams.size());
        for (size_t i = 0; i < streams.size(); ++i) before[i] = streams[i].chunks.size();
        walk(last);
        for (size_t i = 0; i < streams.size(); ++i) {
            auto& chunks = streams[i].chunks;
            if (chunks.size() == before[i]) continue;
            const uint64_t edge = before[i] ? chunks[before[i] - 1].at : 0;
            chunks.erase(std::remove_if(chunks.begin() + long(before[i]), chunks.end(), [&](const Chunk& c) { return c.at <= edge; }), chunks.end());
        }
    }

    void tapeSound() {
        Stream& dv = streams[0];
        Stream s;
        s.audio = s.tape = true;
        s.track.kind = Kind::Audio;
        s.track.id = uint32_t(streams.size() + 1);
        s.chunks = dv.chunks;
        Track& t = s.track;
        int64_t time = 0;
        std::vector<uint8_t> frame;
        for (size_t i = 0; i < s.chunks.size(); ++i) {
            const Chunk& c = s.chunks[i];
            if (!c.size || !source.load(c.at, c.size, frame, kLargestChunk)) continue;
            if (!dv.track.height) {
                dv.track.width = 720;
                dv.track.height = frame.size() > 3 && (frame[3] & 0x80) ? 576 : 480;
            }
            DvSound sound;
            if (!dvSound(frame.data(), frame.size(), sound) || (t.sampleRate && sound.rate != t.sampleRate)) continue;
            if (!t.sampleRate) {
                t.codec = fourcc("lpcm");
                t.sampleRate = sound.rate;
                t.channels = 2;
                t.bits = 16;
                t.timescale = uint32_t(sound.rate);
            }
            Sample sample;
            sample.offset = i;
            sample.size = sound.frames * 4;
            sample.pts = sample.dts = time;
            sample.duration = sound.frames;
            sample.sync = true;
            time += sound.frames;
            t.samples.push_back(sample);
        }
        if (!t.samples.empty()) {
            s.scale = dv.scale;
            s.rate = dv.rate;
            streams.push_back(std::move(s));
        }
    }

    void sound(Stream& s) {
        Track& t = s.track;
        s.ends.reserve(s.chunks.size());
        uint64_t total = 0;
        for (const Chunk& c : s.chunks) s.ends.push_back(total += c.size);
        total = std::min(total, source.size());
        const bool pcm = s.tag == 1 || s.tag == 3;
        if (pcm) {
            const uint64_t frameBytes = uint64_t(t.channels) * uint64_t((t.bits + 7) / 8);
            const bool sized = s.tag == 3 ? t.bits == 32 || t.bits == 64 : t.bits == 8 || t.bits == 16 || t.bits == 24 || t.bits == 32;
            if (!sized || t.channels < 1 || t.channels > 64 || t.sampleRate < 1) return;
            t.codec = fourcc("lpcm");
            t.floating = s.tag == 3;
            t.timescale = uint32_t(t.sampleRate);
            const uint64_t frames = total / frameBytes;
            for (uint64_t done = 0; done < frames; done += kPacketFrames) {
                Sample sample;
                uint64_t count = std::min<uint64_t>(kPacketFrames, frames - done);
                sample.offset = done * frameBytes;
                sample.size = uint32_t(count * frameBytes);
                sample.pts = sample.dts = int64_t(done);
                sample.duration = uint32_t(count);
                sample.sync = true;
                t.samples.push_back(sample);
            }
            return;
        }
        if (const uint32_t wma = waveAudio(s.tag)) {
            const uint64_t bytesPerSecond = s.extra.size() >= 12 ? le32(s.extra.data() + 8) : 0;
            if (t.channels < 1 || t.sampleRate < 1 || !bytesPerSecond) return;
            t.codec = wma;
            t.configType = fourcc("wfex");
            t.config = s.extra;
            t.timescale = uint32_t(t.sampleRate);
            uint64_t before = 0;
            for (const Chunk& c : s.chunks) {
                if (!c.size) continue;
                Sample sample;
                sample.offset = before;
                sample.size = c.size;
                sample.pts = sample.dts = int64_t(before * uint64_t(t.sampleRate) / bytesPerSecond);
                sample.sync = true;
                before += c.size;
                t.samples.push_back(sample);
            }
            for (size_t i = 0; i < t.samples.size(); ++i) {
                const int64_t next = i + 1 < t.samples.size() ? t.samples[i + 1].pts : int64_t(before * uint64_t(t.sampleRate) / bytesPerSecond);
                t.samples[i].duration = uint32_t(std::clamp<int64_t>(next - t.samples[i].pts, 0, 0x7FFFFFFF));
            }
            if (waveDecoded(wma)) {
                int64_t time = 0;
                for (Sample& sample : t.samples) {
                    sample.pts = sample.dts = time;
                    sample.duration = uint32_t(std::min<int64_t>(waveFrames(t, sample.size), 0x7FFFFFFF));
                    time += sample.duration;
                }
            }
            return;
        }
        const bool aac = s.tag == 0xFF || s.tag == 0x1600 || s.tag == 0x1610 || s.tag == 0x706D || s.tag == 0x4143 || s.tag == 0xA106;
        size_t described = s.extra.size() > 18 ? std::min<size_t>(le16(s.extra.data() + 16), s.extra.size() - 18) : 0;
        size_t configAt = 18;
        if (s.tag == 0x1610) {
            const bool raw = described > 12 && le16(s.extra.data() + 18) == 0;
            described = raw ? described - 12 : 0;
            configAt = raw ? 30 : 18;
        }
        if (aac && !described) {
            uint64_t at = 0, lost = 0;
            int64_t time = 0;
            uint8_t head[7];
            while (total - at >= 7 && lost < (1u << 20)) {
                if (!fetch(s, at, head, 7)) break;
                const uint32_t length = (uint32_t(head[3] & 3) << 11) | (uint32_t(head[4]) << 3) | (head[5] >> 5);
                const uint32_t header = head[1] & 1 ? 7 : 9;
                const int rate = (head[2] >> 2) & 15, channels = ((head[2] & 1) << 2) | (head[3] >> 6), profile = head[2] >> 6;
                static const int rates[13] = {96000, 88200, 64000, 48000, 44100, 32000, 24000, 22050, 16000, 12000, 11025, 8000, 7350};
                const bool good = head[0] == 0xFF && (head[1] & 0xF6) == 0xF0 && rate < 13 && length > header && length <= total - at &&
                                  (t.samples.empty() || rates[rate] == t.sampleRate);
                if (!good) {
                    ++at;
                    ++lost;
                    continue;
                }
                lost = 0;
                if (t.samples.empty()) {
                    t.codec = fourcc("mp4a");
                    t.configType = fourcc("ASC ");
                    t.config = {uint8_t(((profile + 1) << 3) | (rate >> 1)), uint8_t(((rate & 1) << 7) | (channels << 3))};
                    t.sampleRate = rates[rate];
                    if (channels) t.channels = channels == 7 ? 8 : channels;
                    t.timescale = uint32_t(t.sampleRate);
                    t.bits = 0;
                }
                const uint32_t frames = 1024u * ((head[6] & 3) + 1u);
                Sample sample;
                sample.offset = at + header;
                sample.size = length - header;
                sample.pts = sample.dts = time;
                sample.duration = frames;
                sample.sync = true;
                time += frames;
                t.samples.push_back(sample);
                at += length;
            }
            return;
        }
        if ((aac || s.tag == 0xF1AC) && described) {
            const size_t extra = described;
            FlacInfo info;
            if (s.tag == 0xF1AC && !(extra >= 34 && flacInfo(s.extra.data() + 18, 34, info))) return;
            t.codec = aac ? fourcc("mp4a") : fourcc("fLaC");
            t.configType = aac ? fourcc("ASC ") : fourcc("fLaC");
            t.config.assign(s.extra.begin() + long(configAt), s.extra.begin() + long(configAt + (aac ? extra : 34)));
            if (s.tag == 0xF1AC) t.bits = info.bits;
            t.timescale = uint32_t(t.sampleRate);
            uint64_t before = 0;
            int64_t time = 0;
            for (const Chunk& c : s.chunks) {
                if (!c.size) continue;
                Sample sample;
                sample.offset = before;
                sample.size = c.size;
                uint8_t head[16];
                FlacFrame f;
                const uint32_t frames = aac ? 1024 : fetch(s, before, head, std::min<size_t>(16, c.size)) && flacFrame(head, std::min<size_t>(16, c.size), f) ? f.blockSize : 0;
                sample.pts = sample.dts = time;
                sample.duration = frames;
                sample.sync = true;
                time += frames;
                before += c.size;
                t.samples.push_back(sample);
            }
            return;
        }
        const bool mpeg = s.tag == 0x55 || s.tag == 0x50;
        uint8_t sync[2] = {0, 0};
        const bool sniffed = s.tag == 0 && fetch(s, 0, sync, 2) && sync[0] == 0x0B && sync[1] == 0x77;
        const bool dts = s.tag == 0x2001;
        if (!mpeg && !dts && s.tag != 0x2000 && !sniffed) return;
        t.bits = 0;
        uint64_t at = 0;
        int64_t time = 0;
        uint64_t lost = 0;
        uint8_t head[16];
        const size_t need = mpeg ? 4 : dts ? 16 : 8;
        while (total - at >= need && lost < (1u << 20)) {
            if (!fetch(s, at, head, need)) break;
            uint32_t size = 0, samples = 0, codec = 0;
            int rate = 0, channels = 0;
            MpegAudio m;
            DolbyFrame d;
            if (mpeg && mpegAudio(head, m)) {
                size = uint32_t(m.size);
                samples = uint32_t(m.samples);
                rate = m.sampleRate;
                channels = m.channels;
                codec = m.layer == 3 ? fourcc(".mp3") : m.layer == 2 ? fourcc(".mp2") : fourcc(".mp1");
            } else if (!mpeg && (dts ? dtsFrame(head, d) : dolbyFrame(head, d) && !d.dependent)) {
                size = d.bytes;
                samples = d.samples;
                rate = d.rate;
                channels = d.channels;
                codec = d.codec;
            } else if (!mpeg && d.dependent && d.bytes >= need && d.bytes <= total - at && !t.samples.empty() && t.samples.back().offset + t.samples.back().size == at) {
                t.samples.back().size += d.bytes;
                at += d.bytes;
                lost = 0;
                continue;
            }
            if (size < need || size > total - at || rate < 1 || (!t.samples.empty() && (codec != t.codec || rate != t.sampleRate))) {
                ++at;
                ++lost;
                continue;
            }
            lost = 0;
            if (t.samples.empty()) {
                t.codec = codec;
                t.sampleRate = rate;
                t.channels = channels;
                t.timescale = uint32_t(rate);
            }
            Sample sample;
            sample.offset = at;
            sample.size = size;
            sample.pts = sample.dts = time;
            sample.duration = samples;
            sample.sync = true;
            time += samples;
            t.samples.push_back(sample);
            at += size;
        }
    }

    void picture(Stream& s) {
        Track& t = s.track;
        const bool plain = !s.tape && (s.compression == 0 || s.compression == 0x03000000);
        t.codec = plain ? fourcc("DIB ") : videoFourcc(s.handler, t.height);
        if (!t.codec) t.codec = s.handler;
        if (vc1Codec(t.codec) || msmpeg4Codec(t.codec) || ownVideoName(t.codec)) {
            t.configType = fourcc("bmih");
            t.config = s.bitmap;
        }
        if (t.width <= 0 || t.height <= 0 || t.width > 32768 || t.height > 32768) return;
        const uint32_t shared = std::gcd(s.scale, s.rate);
        const uint32_t step = s.scale / shared;
        t.timescale = s.rate / shared;
        const bool part2 = t.codec == fourcc("mp4v");
        const bool mpeg = t.codec == fourcc("mp1v") || t.codec == fourcc("mp2v");
        const bool avc = t.codec == fourcc("avc1");
        const bool hevc = t.codec == fourcc("hvc1");
        const bool nals = avc || hevc;
        s.hevc = hevc;
        const bool vc1 = vc1Codec(t.codec) && !t.config.empty();
        const bool ms = msmpeg4Codec(t.codec);
        const bool h263 = t.codec == fourcc("h263");
        const bool cinepak = t.codec == fourcc("cvid");
        std::vector<Shown> frames;
        frames.reserve(s.chunks.size());
        NalList vps, sps, pps;
        OrderFields fields;
        HevcFields order;
        bool orderSps = false;
        const size_t lengthSize = nals && s.extra.size() >= (hevc ? 23u : 7u) && s.extra[0] == 1 ? size_t(s.extra[hevc ? 21 : 4] & 3) + 1 : 0;
        int64_t count = -1;
        bool intra = false;
        auto parameterSets = [&](const uint8_t* nal, size_t size, size_t& packed, bool& key) {
            if (dropped(nal, size, hevc)) return;
            packed += size + 4;
            NalList* list = nullptr;
            if (hevc) {
                const int type = (nal[0] >> 1) & 63;
                if (type >= 16 && type <= 23) intra = true;
                if (type >= 16 && type <= 20) key = true;
                if (type == 33 && !orderSps) {
                    VideoInfo info = parseHevcSps(nal, size);
                    if (info.valid && info.orderBits >= 4 && info.orderBits <= 16) {
                        order.countBits = info.orderBits;
                        order.separate = info.separatePlanes;
                        orderSps = true;
                    }
                }
                if (type == 34 && pps.empty()) hevcPps(nal, size, order);
                if (type <= 21 && count < 0 && orderSps && order.usable) count = hevcCount(nal, size, order);
                list = type == 32 ? &vps : type == 33 ? &sps : type == 34 ? &pps : nullptr;
            } else {
                const int type = nal[0] & 31;
                if (type == 5) key = true;
                if (type == 7 && sps.empty()) fields = orderFields(std::vector<uint8_t>(nal, nal + size));
                if ((type == 1 || type == 5) && count < 0) {
                    std::vector<uint8_t> plain = unescape(nal, std::min<size_t>(size, 48));
                    BitReader r{plain.data() + 1, plain.size() - 1};
                    r.ue();
                    const uint32_t slice = r.ue() % 5;
                    if (r.ok && (slice == 2 || slice == 4)) intra = true;
                    r.ue();
                    if (fields.separate) r.skip(2);
                    r.skip(size_t(fields.numberBits));
                    if (!fields.framesOnly && r.bit()) r.skip(1);
                    if (type == 5) r.ue();
                    uint32_t low = r.bits(fields.countBits);
                    if (r.ok && fields.usable) count = low;
                }
                list = type == 7 ? &sps : type == 8 ? &pps : nullptr;
            }
            if (!list || list->size() >= 32 || size > 65535) return;
            std::vector<uint8_t> one(nal, nal + size);
            if (std::find(list->begin(), list->end(), one) == list->end()) list->push_back(std::move(one));
        };
        if (nals) {
            size_t packed = 0;
            bool key = false;
            auto each = [&](const uint8_t* nal, size_t size) { parameterSets(nal, size, packed, key); };
            if (lengthSize) configNals(s.extra, hevc, each);
            else annexNals(s.extra.data(), s.extra.size(), each);
        }
        std::vector<uint8_t> headers;
        bool packedStream = part2 && packedMark(s.extra.data(), s.extra.size());
        bool checked = false;
        int placeholders = 0;
        std::vector<uint8_t> bytes;
        for (const Chunk& c : s.chunks) {
            Shown f;
            f.at = c.at;
            f.size = f.raw = c.size;
            f.empty = c.size == 0;
            f.key = c.key;
            if (f.empty && placeholders > 0) {
                --placeholders;
                continue;
            }
            if (nals && !f.empty) {
                if (!source.load(c.at, c.size, bytes, kLargestChunk)) continue;
                size_t packed = 0;
                bool key = false;
                count = -1;
                intra = false;
                eachNal(bytes.data(), bytes.size(), lengthSize, [&](const uint8_t* nal, size_t size) { parameterSets(nal, size, packed, key); });
                f.size = lengthSize ? c.size : uint32_t(std::min<size_t>(packed, 0x7FFFFFFF));
                f.type = key ? 0 : 1;
                f.key = intra && (hevc || (c.key && indexed));
                f.count = count;
                f.empty = packed == 0;
            } else if (vc1 && !f.empty) {
                const size_t want = std::min<size_t>(c.size, kHead);
                const uint8_t* p = source.view(c.at, want);
                if (!p) continue;
                f.type = vc1PictureType(t.codec, t.config, p, want);
                if (f.type < 0) f.type = 1;
            } else if (ms && !f.empty) {
                const size_t want = std::min<size_t>(c.size, 8);
                const uint8_t* p = source.view(c.at, want);
                if (!p) continue;
                f.type = msmpeg4PictureType(t.codec, p, want);
                if (f.type < 0) f.type = 1;
            } else if (h263 && !f.empty) {
                const size_t want = std::min<size_t>(c.size, kHead);
                const uint8_t* p = source.view(c.at, want);
                if (!p) continue;
                f.type = h263PictureType(p, want);
                if (f.type < 0) f.type = 1;
            } else if (cinepak && !f.empty) {
                const size_t want = std::min<size_t>(c.size, 1 << 22);
                const uint8_t* p = source.view(c.at, want);
                if (!p) continue;
                f.type = cinepakType(p, want);
                if (f.type < 0) f.type = 1;
            } else if ((part2 || mpeg) && !f.empty) {
                const size_t want = packedStream ? c.size : std::min<size_t>(c.size, kHead);
                const uint8_t* p = source.view(c.at, want);
                if (!p) continue;
                if (mpeg) {
                    f.type = pictureType(p, want);
                } else {
                    size_t first = 0;
                    f.type = vopType(p, want, 0, &first);
                    if (headers.empty() && f.type >= 0 && first > 0) headers.assign(p, p + first);
                    if (!checked && !packedStream) {
                        packedStream = packedMark(p, want);
                        if (packedStream && want < c.size) {
                            p = source.view(c.at, c.size);
                            if (!p) continue;
                        }
                    }
                    checked = true;
                    if (packedStream && f.type >= 0) {
                        if (c.size - first <= 8) {
                            if (placeholders > 0) {
                                --placeholders;
                                continue;
                            }
                            f.empty = true;
                        }
                        size_t second = 0;
                        int next = vopType(p, c.size, first + 4, &second);
                        if (next >= 0) {
                            Shown head = f;
                            head.size = uint32_t(second);
                            frames.push_back(head);
                            f.at = c.at + second;
                            f.size = uint32_t(c.size - second);
                            f.type = next;
                            ++placeholders;
                        }
                    }
                }
                if (f.type < 0) f.type = 1;
            }
            frames.push_back(f);
        }
        if (nals) {
            t.configType = hevc ? fourcc("hvcC") : fourcc("avcC");
            t.config = lengthSize ? s.extra : hevc ? buildHevcConfig(vps, sps, pps) : buildAvcConfig(sps, pps);
            if (t.config.empty()) return;
        } else if (part2) {
            size_t first = 0;
            if (vopType(s.extra.data(), s.extra.size(), 0, &first) >= 0) s.extra.resize(first);
            bool described = false;
            for (size_t i = 0; i + 3 < s.extra.size(); ++i) described = described || (s.extra[i] == 0 && s.extra[i + 1] == 0 && s.extra[i + 2] == 1 && (s.extra[i + 3] & 0xF0) == 0x20);
            const std::vector<uint8_t>& chosen = described ? s.extra : headers;
            t.configType = fourcc("esds");
            t.config = mpeg4Descriptor(chosen.data(), chosen.size());
        }
        const bool typed = nals || part2 || mpeg || vc1 || ms || h263 || cinepak;
        bool reordered = false;
        if (part2 || mpeg || vc1) for (const Shown& f : frames) reordered = reordered || (!f.empty && f.type == 2);
        std::vector<int64_t> slots(frames.size(), 0);
        int64_t slot = 0;
        size_t held = frames.size();
        for (size_t i = 0; i < frames.size(); ++i) {
            if (reordered && !frames[i].empty && frames[i].type != 2) {
                if (held < frames.size()) slots[held] = slot++;
                held = i;
            } else {
                slots[i] = slot++;
            }
        }
        if (held < frames.size()) slots[held] = slot++;
        if ((avc && fields.usable) || (hevc && orderSps && order.usable)) {
            const int64_t span = int64_t(1) << (hevc ? order.countBits : fields.countBits);
            int64_t high = 0, before = 0;
            std::vector<size_t> group;
            auto close = [&] {
                const std::vector<size_t> places = group;
                std::stable_sort(group.begin(), group.end(), [&](size_t a, size_t b) { return frames[a].count < frames[b].count; });
                for (size_t k = 0; k < group.size(); ++k) slots[group[k]] = int64_t(places[k]);
                group.clear();
            };
            for (size_t i = 0; i < frames.size(); ++i) {
                Shown& f = frames[i];
                if (f.empty) continue;
                if (f.count < 0 || f.type == 0) {
                    close();
                    high = before = 0;
                    if (f.count < 0) continue;
                }
                if (f.count < before && before - f.count >= span / 2) high += span;
                else if (f.count > before && f.count - before > span / 2) high -= span;
                before = f.count;
                f.count += high;
                group.push_back(i);
            }
            close();
        }
        int64_t late = 0;
        for (size_t i = 0; i < frames.size(); ++i) late = std::max(late, int64_t(i) - slots[i]);
        std::vector<uint32_t> shown;
        for (size_t i = 0; i < frames.size(); ++i) {
            const Shown& f = frames[i];
            if (f.empty) continue;
            Sample sample;
            sample.offset = f.at;
            sample.size = f.size;
            sample.pts = slots[i] * step;
            sample.dts = (int64_t(i) - late) * step;
            sample.duration = step;
            sample.sync = typed ? f.type == 0 || (nals && f.key) : intraOnly(t.codec) || f.key;
            shown.push_back(uint32_t(t.samples.size()));
            t.samples.push_back(sample);
            if (nals && !lengthSize) s.raw.push_back(f.raw);
        }
        std::sort(shown.begin(), shown.end(), [&](uint32_t a, uint32_t b) { return t.samples[a].pts < t.samples[b].pts; });
        for (size_t i = 0; i + 1 < shown.size(); ++i) {
            int64_t gap = t.samples[shown[i + 1]].pts - t.samples[shown[i]].pts;
            t.samples[shown[i]].duration = uint32_t(std::clamp<int64_t>(gap, 1, int64_t(step) * 100000));
        }
        if (!shown.empty()) t.samples[shown.back()].duration = uint32_t(std::clamp<int64_t>(int64_t(frames.size()) * step - t.samples[shown.back()].pts, 1, int64_t(step) * 100000));
        if (!t.samples.empty()) t.mediaStart = t.samples[shown[0]].pts;
        t.delay = int64_t(s.start) * step;
    }
};

}

std::unique_ptr<Reader> openAvi(const std::string& path, std::string* error) {
    auto fail = [&](const char* text) {
        if (error) *error = text;
        return nullptr;
    };
    auto reader = std::make_unique<AviReader>();
    if (!reader->source.open(path)) return fail("cannot open file");
    reader->name = "avi";
    reader->outline();
    if (reader->streams.empty()) return fail("no stream descriptions in this AVI file");
    if (reader->movies.empty()) return fail("no picture or sound data in this AVI file");
    if (!reader->openIndexes() && !reader->oldIndex()) {
        for (Stream& s : reader->streams) s.chunks.clear();
        reader->walk();
    } else {
        reader->indexed = true;
        reader->rest();
    }
    if (!reader->streams.empty() && reader->streams[0].tape) reader->tapeSound();
    for (Stream& s : reader->streams) {
        if (s.video) reader->picture(s);
        else if (s.audio && !s.tape) reader->sound(s);
        if (s.track.samples.empty() || !s.track.timescale) continue;
        if (s.audio && s.rate && !s.tape) s.track.delay = int64_t(uint64_t(s.start) * s.scale * s.track.timescale / s.rate);
        indexTrack(s.track);
        reader->list.push_back(s.track);
    }
    if (reader->list.empty()) return fail("no supported picture or sound streams in this AVI file");
    return reader;
}


}
