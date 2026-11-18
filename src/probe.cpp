#include "absent/probe.h"
#include "absent/media.h"
#include "bytes.h"
#include "nal.h"
#include "read_common.h"
#include "readers.h"
#include <algorithm>
#include <cinttypes>
#include <cmath>
#include <cstdio>
#include <map>
#include <numeric>
#include "portable.h"

namespace absent {

namespace {

struct Json {
    std::string text;
    std::vector<bool> first;

    void comma() {
        if (!first.empty()) {
            if (!first.back()) text += ',';
            first.back() = false;
        }
    }
    static size_t sequence(const std::string& value, size_t at) {
        unsigned char c = static_cast<unsigned char>(value[at]);
        size_t length = c < 0x80 ? 1 : (c >> 5) == 6 ? 2 : (c >> 4) == 14 ? 3 : (c >> 3) == 30 ? 4 : 0;
        if (!length || at + length > value.size()) return 0;
        uint32_t code = length == 1 ? c : c & (0xFF >> (length + 1));
        for (size_t i = 1; i < length; ++i) {
            unsigned char next = static_cast<unsigned char>(value[at + i]);
            if ((next & 0xC0) != 0x80) return 0;
            code = (code << 6) | (next & 0x3F);
        }
        static const uint32_t least[5] = {0, 0, 0x80, 0x800, 0x10000};
        if (code < least[length] || code > 0x10FFFF || (code >= 0xD800 && code <= 0xDFFF)) return 0;
        return length;
    }
    void quoted(const std::string& value) {
        text += '"';
        for (size_t at = 0; at < value.size();) {
            unsigned char c = static_cast<unsigned char>(value[at]);
            size_t length = sequence(value, at);
            if (!length) {
                text += "\\ufffd";
                ++at;
            } else if (length > 1) {
                text.append(value, at, length);
                at += length;
            } else {
                ++at;
                if (c == '"' || c == '\\') {
                    text += '\\';
                    text += char(c);
                } else if (c < 32 || c == 127) {
                    char code[8];
                    snprintf(code, sizeof code, "\\u%04x", c);
                    text += code;
                } else {
                    text += char(c);
                }
            }
        }
        text += '"';
    }
    void key(const char* name) {
        comma();
        quoted(name);
        text += ':';
    }
    void open(char bracket) {
        text += bracket;
        first.push_back(true);
    }
    void close(char bracket) {
        text += bracket;
        first.pop_back();
    }
    void object(const char* name) {
        key(name);
        open('{');
    }
    void array(const char* name) {
        key(name);
        open('[');
    }
    void item() {
        comma();
        open('{');
    }
    void string(const char* name, const std::string& value) {
        key(name);
        quoted(value);
    }
    void number(const char* name, int64_t value) {
        key(name);
        text += std::to_string(value);
    }
    void seconds(const char* name, double value) {
        char number[64];
        snprintf(number, sizeof number, "%.6f", value);
        string(name, number);
    }
};

struct Sound {
    int object = 0;
    bool known = false;
    bool replicated = false;
    bool parametric = false;
};

uint32_t objectType(BitReader& r) {
    uint32_t type = r.bits(5);
    return type == 31 ? 32 + r.bits(6) : type;
}

void rateField(BitReader& r) {
    if (r.bits(4) == 15) r.skip(24);
}

Sound soundOf(const uint8_t* p, size_t n) {
    Sound sound;
    BitReader r{p, n};
    sound.object = int(objectType(r));
    rateField(r);
    uint32_t layout = r.bits(4);
    sound.known = r.ok;
    int core = sound.object;
    if (core == 5 || core == 29) {
        sound.replicated = true;
        sound.parametric = core == 29;
        rateField(r);
        core = int(objectType(r));
        if (core == 22) r.skip(4);
    }
    if (sound.replicated || !(core == 1 || core == 2 || core == 3 || core == 4 || core == 6 || core == 7)) return sound;
    r.skip(1);
    if (r.bit()) r.skip(14);
    r.skip(1);
    if (layout == 0 || !r.ok) return sound;
    if (r.left() >= 16 && r.bits(11) == 0x2B7 && objectType(r) == 5 && r.bit()) {
        sound.replicated = true;
        rateField(r);
        if (r.left() >= 12 && r.bits(11) == 0x548 && r.bit()) sound.parametric = true;
    }
    return sound;
}

struct Descriptors {
    int object = 0;
    std::vector<uint8_t> specific;
};

Descriptors descriptorsOf(const std::vector<uint8_t>& esds) {
    Descriptors out;
    View v{esds.data(), esds.size()};
    v.skip(4);
    auto header = [](View& from, uint32_t& tag) {
        tag = from.u8();
        uint32_t size = 0;
        for (int i = 0; i < 4; ++i) {
            uint8_t b = from.u8();
            size = (size << 7) | (b & 0x7F);
            if (!(b & 0x80)) break;
        }
        return from.sub(std::min<size_t>(size, from.left()));
    };
    uint32_t tag = 0;
    View stream = header(v, tag);
    if (!v.ok || tag != 3) return out;
    stream.skip(2);
    uint8_t flags = stream.u8();
    if (flags & 0x80) stream.skip(2);
    if (flags & 0x40) stream.skip(stream.u8());
    if (flags & 0x20) stream.skip(2);
    View decoder = header(stream, tag);
    if (!stream.ok || tag != 4) return out;
    out.object = decoder.u8();
    decoder.skip(12);
    View specific = header(decoder, tag);
    if (decoder.ok && tag == 5) out.specific.assign(specific.p, specific.p + specific.n);
    return out;
}

struct Look {
    std::string codec;
    std::string profile;
    std::string pixels;
    std::string sampleFormat;
    bool rangeKnown = false;
    bool full = false;
    int primaries = 2;
    int transfer = 2;
    int matrix = 2;
};

std::string pixelName(int chroma, int depth, bool full, bool alpha = false) {
    std::string name = chroma == 0 ? "gray" : chroma == 1 ? "420p" : chroma == 2 ? "422p" : "444p";
    if (chroma != 0) name = (alpha ? "yuva" : full && depth == 8 ? "yuvj" : "yuv") + name;
    if (depth > 8) name += std::to_string(depth) + "le";
    return name;
}

const char* avcProfile(int profile, int flags) {
    bool intra = flags & 0x10;
    switch (profile) {
        case 66: return (flags & 0x40) ? "Constrained Baseline" : "Baseline";
        case 77: return "Main";
        case 88: return "Extended";
        case 100: return "High";
        case 110: return intra ? "High 10 Intra" : "High 10";
        case 122: return intra ? "High 4:2:2 Intra" : "High 4:2:2";
        case 244: return intra ? "High 4:4:4 Intra" : "High 4:4:4 Predictive";
        case 44: return "CAVLC 4:4:4";
        default: return "";
    }
}

const char* hevcProfile(int profile) {
    switch (profile) {
        case 1: return "Main";
        case 2: return "Main 10";
        case 3: return "Main Still Picture";
        case 4: return "Rext";
        default: return "";
    }
}

std::vector<uint8_t> firstPacket(Reader& reader, const Track& track) {
    std::vector<uint8_t> data;
    if (track.samples.empty() || track.samples[0].size > (64u << 20)) return data;
    data.resize(track.samples[0].size);
    if (!reader.read(track, 0, data.data())) data.clear();
    return data;
}

std::string trimmed(uint32_t code) {
    std::string text = fourccText(code);
    while (!text.empty() && (text.back() == ' ' || text.back() == '.')) text.pop_back();
    for (char& c : text) if (c >= 'A' && c <= 'Z') c = char(c + 32);
    return text;
}

void applyVideoInfo(const VideoInfo& info, Look& look) {
    if (!info.valid) return;
    if (info.rangeKnown) {
        look.rangeKnown = true;
        look.full = info.full;
    }
    if (info.colorKnown) {
        if (look.primaries == 2) look.primaries = info.primaries;
        if (look.transfer == 2) look.transfer = info.transfer;
        if (look.matrix == 2) look.matrix = info.matrix;
    }
    look.pixels = pixelName(info.chroma, info.bitDepth, look.rangeKnown && look.full);
}

Look lookOf(Reader& reader, const Track& t) {
    Look look;
    if (t.color.known) {
        look.rangeKnown = true;
        look.full = t.color.full;
        look.primaries = t.color.primaries;
        look.transfer = t.color.transfer;
        look.matrix = t.color.matrix;
    }
    uint32_t c = t.codec;
    if (c == fourcc("avc1")) {
        look.codec = "h264";
        VideoInfo info = videoInfo(fourcc("avcC"), t.config);
        if (info.valid) look.profile = avcProfile(info.profile, info.compatibility);
        look.pixels = "yuv420p";
        applyVideoInfo(info, look);
    } else if (c == fourcc("hvc1")) {
        look.codec = "hevc";
        VideoInfo info = videoInfo(fourcc("hvcC"), t.config);
        if (info.valid) look.profile = hevcProfile(info.profile);
        look.pixels = "yuv420p";
        applyVideoInfo(info, look);
    } else if (c == fourcc("av01")) {
        look.codec = "av1";
        look.pixels = "yuv420p";
        if (t.config.size() >= 4) {
            int profile = t.config[1] >> 5;
            look.profile = profile == 0 ? "Main" : profile == 1 ? "High" : "Professional";
            bool high = t.config[2] & 0x40, twelve = t.config[2] & 0x20, mono = t.config[2] & 0x10;
            bool halfX = t.config[2] & 0x08, halfY = t.config[2] & 0x04;
            look.pixels = pixelName(mono ? 0 : halfX && halfY ? 1 : halfX ? 2 : 3, high ? (twelve ? 12 : 10) : 8, false);
        }
    } else if (c == fourcc("apv1")) {
        look.codec = "apv";
        const bool described = t.configType == fourcc("apvC") && t.config.size() >= 22;
        look.pixels = pixelName(described && t.config[20] >> 4 == 3 ? 3 : 2, described ? 8 + (t.config[20] & 15) : 10, false);
    } else if (c == fourcc("vp09")) {
        look.codec = "vp9";
        look.pixels = "yuv420p";
        look.profile = "Profile 0";
        std::vector<uint8_t> packet = firstPacket(reader, t);
        BitReader r{packet.data(), packet.size()};
        if (r.bits(2) == 2) {
            int profile = int(r.bit());
            profile |= int(r.bit()) << 1;
            if (profile == 3) r.skip(1);
            bool existing = r.bit();
            bool inter = r.bit();
            r.skip(2);
            if (r.ok && !existing && !inter && r.bits(24) == 0x498342) {
                int depth = profile >= 2 ? (r.bit() ? 12 : 10) : 8;
                int space = int(r.bits(3));
                int chroma = 1;
                if (space != 7) {
                    look.full = r.bit();
                    if (profile & 1) {
                        bool halfX = r.bit(), halfY = r.bit();
                        chroma = halfX && halfY ? 1 : halfX ? 2 : 3;
                    }
                } else {
                    look.full = true;
                    chroma = 3;
                }
                if (r.ok) {
                    static const int spaces[8] = {2, 5, 1, 6, 7, 9, 2, 0};
                    look.rangeKnown = true;
                    if (spaces[space] != 2) look.matrix = spaces[space];
                    look.profile = "Profile " + std::to_string(profile);
                    look.pixels = space == 7 ? (depth == 8 ? "gbrp" : "gbrp" + std::to_string(depth) + "le") : pixelName(chroma, depth, false, t.alpha);
                }
            }
        } else if (t.config.size() >= 7) {
            look.profile = "Profile " + std::to_string(t.config[4]);
            int sub = (t.config[6] >> 1) & 7;
            look.pixels = pixelName(sub <= 1 ? 1 : sub == 2 ? 2 : 3, t.config[6] >> 4, false);
        }
    } else if (c == fourcc("vp08")) {
        look.codec = "vp8";
        look.pixels = t.alpha ? "yuva420p" : "yuv420p";
        std::vector<uint8_t> packet = firstPacket(reader, t);
        if (!packet.empty()) look.profile = std::to_string((packet[0] >> 1) & 7);
    } else if ((c >> 16) == 0x6170) {
        look.codec = "prores";
        look.profile = c == fourcc("apco") ? "Proxy" : c == fourcc("apcs") ? "LT" : c == fourcc("apcn") ? "Standard" : c == fourcc("apch") ? "HQ" : c == fourcc("ap4h") ? "4444" : c == fourcc("ap4x") ? "XQ" : "";
        bool four = c == fourcc("ap4h") || c == fourcc("ap4x");
        bool alpha = false;
        std::vector<uint8_t> packet = firstPacket(reader, t);
        if (packet.size() >= 26) {
            four = (packet[20] >> 6) == 3;
            alpha = packet[25] & 15;
        }
        look.pixels = pixelName(four ? 3 : 2, c == fourcc("ap4h") || c == fourcc("ap4x") ? 12 : 10, false, alpha);
    } else if (c == fourcc("mp4v")) {
        look.codec = "mpeg4";
        look.pixels = "yuv420p";
        std::vector<uint8_t> specific = descriptorsOf(t.config).specific;
        for (size_t i = 0; i + 4 < specific.size(); ++i) {
            if (specific[i] == 0 && specific[i + 1] == 0 && specific[i + 2] == 1 && specific[i + 3] == 0xB0) {
                int level = specific[i + 4];
                look.profile = (level >> 4) == 0 ? "Simple Profile" : (level >> 4) == 15 ? "Advanced Simple Profile" : "";
                break;
            }
        }
    } else if (c == fourcc("jpeg")) {
        look.codec = "mjpeg";
        look.profile = "Baseline";
        look.pixels = "yuvj420p";
        look.rangeKnown = true;
        look.full = true;
        if (look.matrix == 2) look.matrix = 5;
        std::vector<uint8_t> first = firstPacket(reader, t);
        for (size_t i = 2; i + 3 < first.size() && first[i] == 0xFF;) {
            if (first[i + 1] >= 0xC0 && first[i + 1] <= 0xC2 && i + 14 < first.size() && first[i + 9] >= 3) {
                const bool halfX = (first[i + 11] >> 4) == 2 * (first[i + 14] >> 4), halfY = (first[i + 11] & 15) == 2 * (first[i + 14] & 15);
                look.pixels = halfX && halfY ? "yuvj420p" : halfX ? "yuvj422p" : "yuvj444p";
                if (first[i + 1] == 0xC2) look.profile = "Progressive";
                break;
            }
            if (first[i + 1] == 0xDA) break;
            i += 2 + ((size_t(first[i + 2]) << 8) | first[i + 3]);
        }
    } else if (c == fourcc("mp1v") || c == fourcc("mp2v") || c == fourcc("h263")) {
        look.codec = c == fourcc("mp1v") ? "mpeg1video" : c == fourcc("mp2v") ? "mpeg2video" : "h263";
        look.pixels = "yuv420p";
        look.rangeKnown = look.rangeKnown || c != fourcc("h263");
    } else if (const char* own = ownVideoName(c)) {
        look.codec = own;
        if (c == fourcc("v210")) look.pixels = "yuv422p10le";
        else if (c == fourcc("Hap1") || c == fourcc("HapY")) look.pixels = "rgb0";
        else if (c == fourcc("Hap5") || c == fourcc("HapM")) look.pixels = "rgba";
        else if (c == fourcc("HapA")) look.pixels = "gray";
    } else if ((c >> 16) == 0x6476) {
        look.codec = "dvvideo";
        look.pixels = c == fourcc("dvcp") ? "yuv420p" : c == fourcc("dvc ") || c == fourcc("dvpp") ? "yuv411p" : "yuv422p";
    } else if (c == fourcc("samr") || c == fourcc("sawb")) {
        look.codec = c == fourcc("samr") ? "amr_nb" : "amr_wb";
        look.sampleFormat = "fltp";
    } else if (c == fourcc("mp4a")) {
        look.codec = "aac";
        look.sampleFormat = "fltp";
        std::vector<uint8_t> specific = t.config;
        if (t.configType == fourcc("esds")) {
            Descriptors d = descriptorsOf(t.config);
            specific = d.specific;
            if (d.object == 0x69 || d.object == 0x6B) {
                look.codec = "mp3";
                return look;
            }
        }
        Sound sound = soundOf(specific.data(), specific.size());
        if (sound.known) {
            static const std::map<int, const char*> names = {{1, "Main"}, {2, "LC"}, {3, "SSR"}, {4, "LTP"}, {5, "HE-AAC"}, {23, "LD"}, {29, "HE-AACv2"}, {39, "ELD"}};
            auto found = names.find(sound.object);
            if (found != names.end()) look.profile = found->second;
            if (sound.replicated) look.profile = sound.parametric ? "HE-AACv2" : "HE-AAC";
        }
    } else if (c == fourcc(".mp3") || c == fourcc(".mp2") || c == fourcc(".mp1")) {
        look.codec = c == fourcc(".mp3") ? "mp3" : c == fourcc(".mp2") ? "mp2" : "mp1";
        look.sampleFormat = "fltp";
    } else if (c == fourcc("Opus")) {
        look.codec = "opus";
        look.sampleFormat = "fltp";
    } else if (c == fourcc("fLaC")) {
        look.codec = "flac";
        look.sampleFormat = t.bits > 16 ? "s32" : "s16";
    } else if (c == fourcc("alac")) {
        look.codec = "alac";
        look.sampleFormat = t.bits > 16 ? "s32p" : "s16p";
    } else if (c == fourcc("vorb")) {
        look.codec = "vorbis";
        look.sampleFormat = "fltp";
    } else if (c == fourcc("ac-3") || c == fourcc("ec-3")) {
        look.codec = c == fourcc("ac-3") ? "ac3" : "eac3";
        look.sampleFormat = "fltp";
    } else if (c == fourcc("wma1") || c == fourcc("wma2") || c == fourcc("wmap") || c == fourcc("wmal") || c == fourcc("wmas")) {
        look.codec = c == fourcc("wma1") ? "wmav1" : c == fourcc("wma2") ? "wmav2" : c == fourcc("wmap") ? "wmapro" : c == fourcc("wmal") ? "wmalossless" : "wmavoice";
        look.sampleFormat = c == fourcc("wmas") ? "flt" : c != fourcc("wmal") ? "fltp" : t.bits > 16 ? "s32p" : "s16p";
    } else if (c == fourcc("WMV1") || c == fourcc("WMV2") || c == fourcc("WMV3") || c == fourcc("WVC1") || c == fourcc("WMVA")) {
        look.codec = c == fourcc("WMV1") ? "wmv1" : c == fourcc("WMV2") ? "wmv2" : c == fourcc("WMV3") ? "wmv3" : "vc1";
        look.pixels = "yuv420p";
        if (c == fourcc("WMV3") && t.config.size() >= 41) look.profile = (t.config[40] >> 6) == 0 ? "Simple" : (t.config[40] >> 6) == 1 ? "Main" : "Complex";
        if (c != fourcc("WMV3") && c != fourcc("WMV1") && c != fourcc("WMV2")) look.profile = "Advanced";
    } else if (c == fourcc("MPG4") || c == fourcc("MP42") || c == fourcc("MP43")) {
        look.codec = c == fourcc("MPG4") ? "msmpeg4v1" : c == fourcc("MP42") ? "msmpeg4v2" : "msmpeg4v3";
        look.pixels = "yuv420p";
    } else if (waveDecoded(c)) {
        static const std::map<uint32_t, const char*> names = {{fourcc("alaw"), "pcm_alaw"}, {fourcc("ulaw"), "pcm_mulaw"}, {fourcc("ima4"), "adpcm_ima_qt"},
            {0x6D730002, "adpcm_ms"}, {0x6D730011, "adpcm_ima_wav"}, {0x6D730020, "adpcm_yamaha"}, {0x6D730061, "adpcm_ima_dk4"}, {0x6D730062, "adpcm_ima_dk3"},
            {0x6D730014, "g726"}, {0x6D730040, "g726"}, {0x6D730045, "g726"}, {0x6D730064, "g726"}, {0x6D73028F, "g722"}};
        look.codec = names.at(c);
        look.sampleFormat = c == fourcc("ima4") || c == 0x6D730011 || (c == 0x6D730002 && t.channels > 2) ? "s16p" : "s16";
    } else if (c == fourcc("wvpk")) {
        look.codec = "wavpack";
        look.sampleFormat = t.bits > 16 ? "s32p" : "s16p";
    } else if (c == fourcc("dts ")) {
        look.codec = "dts";
        look.sampleFormat = "fltp";
    } else if (c == fourcc("mlpa")) {
        look.codec = "truehd";
        look.sampleFormat = "s32";
    } else if (c == fourcc("TTA1")) {
        look.codec = "tta";
        look.sampleFormat = t.bits <= 8 ? "u8" : t.bits <= 16 ? "s16" : "s32";
    } else if (c == fourcc("lpcm")) {
        int bits = t.bits ? t.bits : 16;
        std::string order = bits == 8 ? "" : t.bigEndian ? "be" : "le";
        look.codec = "pcm_" + std::string(t.floating ? "f" : bits == 8 && !t.bigEndian ? "u" : "s") + std::to_string(bits) + order;
        look.sampleFormat = t.floating ? (bits == 64 ? "dbl" : "flt") : bits == 8 ? (t.bigEndian ? "s8" : "u8") : bits == 16 ? "s16" : bits == 64 ? "s64" : "s32";
    } else {
        look.codec = trimmed(c);
    }
    return look;
}

const char* primariesName(int value) {
    static const std::map<int, const char*> names = {{1, "bt709"}, {4, "bt470m"}, {5, "bt470bg"}, {6, "smpte170m"}, {7, "smpte240m"}, {8, "film"}, {9, "bt2020"},
        {10, "smpte428"}, {11, "smpte431"}, {12, "smpte432"}, {22, "ebu3213"}};
    auto found = names.find(value);
    return found == names.end() ? nullptr : found->second;
}

const char* transferName(int value) {
    static const std::map<int, const char*> names = {{1, "bt709"}, {4, "bt470m"}, {5, "bt470bg"}, {6, "smpte170m"}, {7, "smpte240m"}, {8, "linear"}, {9, "log100"},
        {10, "log316"}, {11, "iec61966-2-4"}, {12, "bt1361e"}, {13, "iec61966-2-1"}, {14, "bt2020-10"}, {15, "bt2020-12"}, {16, "smpte2084"}, {17, "smpte428"},
        {18, "arib-std-b67"}};
    auto found = names.find(value);
    return found == names.end() ? nullptr : found->second;
}

const char* matrixName(int value) {
    static const std::map<int, const char*> names = {{0, "gbr"}, {1, "bt709"}, {4, "fcc"}, {5, "bt470bg"}, {6, "smpte170m"}, {7, "smpte240m"}, {8, "ycgco"},
        {9, "bt2020nc"}, {10, "bt2020c"}, {11, "smpte2085"}, {12, "chroma-derived-nc"}, {13, "chroma-derived-c"}, {14, "ictcp"}};
    auto found = names.find(value);
    return found == names.end() ? nullptr : found->second;
}

const char* layoutName(int channels) {
    static const char* names[9] = {"", "mono", "stereo", "3.0", "4.0", "5.0", "5.1", "6.1", "7.1"};
    return channels >= 1 && channels <= 8 ? names[channels] : "";
}

const char* maskName(uint32_t mask) {
    static const std::map<uint32_t, const char*> names = {{0x4, "mono"}, {0x3, "stereo"}, {0xB, "2.1"}, {0x7, "3.0"}, {0x103, "3.0(back)"},
        {0x107, "4.0"}, {0x33, "quad"}, {0x603, "quad(side)"}, {0xF, "3.1"}, {0x37, "5.0"}, {0x607, "5.0(side)"}, {0x10F, "4.1"}, {0x3F, "5.1"},
        {0x60F, "5.1(side)"}, {0x707, "6.0"}, {0x137, "hexagonal"}, {0x70F, "6.1"}, {0x13F, "6.1(back)"}, {0x637, "7.0"}, {0x63F, "7.1"},
        {0xFF, "7.1(wide)"}, {0x6CF, "7.1(wide-side)"}, {0x737, "octagonal"}};
    auto found = names.find(mask);
    return found == names.end() ? nullptr : found->second;
}

uint32_t codecMask(const Track& t) {
    if ((t.codec != fourcc("wmap") && t.codec != fourcc("wmal")) || t.configType != fourcc("wfex") || t.config.size() < 18) return 0;
    size_t at = 18;
    if (le16(t.config.data()) == 0xFFFE) at += 22;
    return t.config.size() >= at + 6 ? le32(t.config.data() + at + 2) : 0;
}

int64_t whole(double value) {
    if (!(value == value)) return 0;
    return value >= 9.2e18 ? INT64_MAX : value <= -9.2e18 ? INT64_MIN : int64_t(std::llround(value));
}

int64_t floored(double value) {
    return whole(std::floor(value));
}

std::string fraction(int64_t top, int64_t bottom) {
    if (top <= 0 || bottom <= 0) return "0/0";
    int64_t common = std::gcd(top, bottom);
    top /= common;
    bottom /= common;
    double value = double(top) / double(bottom);
    for (int thousandths = 0; thousandths < 2 && value <= 1e9; ++thousandths) {
        int64_t under = thousandths ? 1001 : 1;
        int64_t over = whole(value * double(under) / (thousandths ? 1000.0 : 1.0)) * (thousandths ? 1000 : 1);
        if (over <= 0) continue;
        if (std::fabs(double(over) / double(under) - value) <= value * 2e-5 && (bottom > under || thousandths == 0)) {
            int64_t shared = std::gcd(over, under);
            return std::to_string(over / shared) + "/" + std::to_string(under / shared);
        }
    }
    while (top > INT32_MAX || bottom > INT32_MAX) {
        top /= 2;
        bottom /= 2;
    }
    return std::to_string(top) + "/" + std::to_string(std::max<int64_t>(bottom, 1));
}

int64_t shown(const Track& t, int64_t stamp) {
    return int64_t(uint64_t(stamp) - uint64_t(t.mediaStart) + uint64_t(t.delay));
}

struct Timing {
    double start = 0;
    double duration = 0;
    int64_t bytes = 0;
    int64_t ticks = 0;
};

Timing timingOf(const Track& t) {
    Timing timing;
    if (!t.timescale || t.samples.empty()) return timing;
    int64_t end = INT64_MIN, first = INT64_MAX;
    for (const Sample& s : t.samples) {
        timing.bytes += s.size;
        timing.ticks += s.duration;
        int64_t finish = int64_t(uint64_t(s.pts) + s.duration);
        end = std::max(end, finish);
        if (finish > t.mediaStart) first = std::min(first, std::max(s.pts, t.mediaStart));
    }
    if (first == INT64_MAX) first = t.mediaStart;
    end = std::min(end, int64_t(uint64_t(first) + uint64_t(timing.ticks)));
    if (t.mediaEnd >= 0) end = std::min(end, t.mediaEnd);
    timing.start = double(shown(t, first)) / t.timescale;
    timing.duration = std::max<double>(0, double(int64_t(uint64_t(end) - uint64_t(first))) / t.timescale);
    return timing;
}

bool onGrid(const Track& t, int64_t top, int64_t bottom, int64_t& budget) {
    double step = double(t.timescale) * double(bottom) / double(top);
    int64_t origin = t.samples[0].pts;
    double slack = t.timescale >= 10000 ? 1.0 : 0.5001;
    if (step < 2 * slack + 1) return false;
    for (const Sample& s : t.samples) {
        if (--budget < 0) return false;
        double at = double(int64_t(uint64_t(s.pts) - uint64_t(origin)));
        if (std::fabs(at - std::round(at / step) * step) > slack) return false;
    }
    return true;
}

std::string baseRate(const Track& t, const Timing& timing) {
    if (t.kind != Kind::Video || timing.ticks <= 0 || t.samples.empty()) return "0/0";
    int64_t common = 0;
    bool even = true;
    for (const Sample& s : t.samples) {
        common = std::gcd(common, int64_t(s.duration));
        even = even && s.duration == t.samples[0].duration;
    }
    if (even) return fraction(t.timescale, t.samples[0].duration);
    double average = double(t.samples.size()) * t.timescale / double(timing.ticks);
    int64_t budget = std::max<int64_t>(int64_t(t.samples.size()) * 8, int64_t(32) << 20);
    for (int64_t whole = 1; whole <= 1000 && budget > 0; ++whole) {
        if (double(whole) < average * 0.99) continue;
        if (double(whole) * 1000 / 1001 >= average * 0.99 && onGrid(t, whole * 1000, 1001, budget)) return fraction(whole * 1000, 1001);
        if (onGrid(t, whole, 1, budget)) return fraction(whole, 1);
    }
    return fraction(t.timescale, std::max<int64_t>(common, 1));
}

const char* formatName(const std::string& name) {
    static const std::pair<const char*, const char*> names[] = {
        {"mp4", "mov,mp4,m4a,3gp,3g2,mj2"}, {"mov", "mov,mp4,m4a,3gp,3g2,mj2"}, {"m4a", "mov,mp4,m4a,3gp,3g2,mj2"},
        {"matroska", "matroska,webm"}, {"mkv", "matroska,webm"}, {"webm", "matroska,webm"},
        {"ogg", "ogg"}, {"opus", "ogg"}, {"wav", "wav"}, {"wave", "wav"}, {"aiff", "aiff"}, {"aifc", "aiff"},
        {"mp3", "mp3"}, {"mp2", "mp3"}, {"mp1", "mp3"}, {"flac", "flac"}, {"aac", "aac"}, {"adts", "aac"}, {"mpegts", "mpegts"}, {"ts", "mpegts"}, {"m2ts", "mpegts"},
        {"avi", "avi"}, {"caf", "caf"}, {"asf", "asf"}, {"amr", "amr"}, {"ac3", "ac3"}, {"eac3", "eac3"},
    };
    for (auto& entry : names) if (name == entry.first) return entry.second;
    return nullptr;
}

}

std::string probeJson(const std::string& path, bool packets, std::string* error) {
    auto reader = Reader::open(path, error);
    if (!reader) return "";
    int64_t fileSize = fileBytes(path);
    const std::vector<Track>& tracks = reader->tracks();
    const char* name = formatName(reader->format());
    const std::string family = name ? name : reader->format();
    const bool matroska = family == "matroska,webm", windowsMedia = family == "asf", mpegAudio = family == "mp3", adts = family == "aac", wave = family == "wav", ogg = family == "ogg";
    const bool counted = family == "aiff" || family == "mov,mp4,m4a,3gp,3g2,mj2";
    Json j;
    j.open('{');
    j.array("streams");

    double begin = 1e300, finish = -1e300;
    for (size_t index = 0; index < tracks.size(); ++index) {
        const Track& t = tracks[index];
        Look look = lookOf(*reader, t);
        Timing timing = timingOf(t);
        int64_t clock = matroska || windowsMedia ? 1000 : mpegAudio ? 14112000 : adts ? 28224000 : t.timescale;
        if (t.timescale && !t.samples.empty()) {
            if (mpegAudio && t.mediaStart > 0) timing.start += double(t.mediaStart) / t.timescale;
            if (ogg && t.codec == fourcc("Opus") && t.mediaStart > 0) timing.duration += double(t.mediaStart) / t.timescale;
        }
        if (look.codec == "av1" || look.codec == "prores" || look.codec == "vp8") look.rangeKnown = true;
        j.item();
        j.number("index", int64_t(index));
        j.string("codec_name", look.codec);
        if (!look.profile.empty()) j.string("profile", look.profile);
        j.string("codec_type", t.kind == Kind::Video ? "video" : t.kind == Kind::Audio ? "audio" : "data");
        if (t.kind == Kind::Video) {
            j.number("width", t.width);
            j.number("height", t.height);
            if (!look.pixels.empty()) j.string("pix_fmt", look.pixels);
            if (look.rangeKnown) j.string("color_range", look.full ? "pc" : "tv");
            if (matrixName(look.matrix)) j.string("color_space", matrixName(look.matrix));
            if (transferName(look.transfer)) j.string("color_transfer", transferName(look.transfer));
            if (primariesName(look.primaries)) j.string("color_primaries", primariesName(look.primaries));
            if (!t.interlaced) j.string("field_order", "progressive");
        } else if (t.kind == Kind::Audio) {
            if (!look.sampleFormat.empty()) j.string("sample_fmt", look.sampleFormat);
            j.string("sample_rate", std::to_string(t.sampleRate));
            j.number("channels", t.channels);
            bool dolby = t.codec == fourcc("ac-3") || t.codec == fourcc("ec-3");
            const char* named = maskName(codecMask(t));
            if (named) j.string("channel_layout", named);
            else if (*layoutName(t.channels)) j.string("channel_layout", dolby && t.channels == 6 ? "5.1(side)" : layoutName(t.channels));
        }
        char id[32];
        snprintf(id, sizeof id, "0x%x", t.id);
        j.string("id", id);
        bool video = t.kind == Kind::Video && timing.ticks > 0;
        j.string("r_frame_rate", baseRate(t, timing));
        j.string("avg_frame_rate", !video ? "0/0" : t.frameStep ? fraction(t.timescale, t.frameStep) : fraction(int64_t(t.samples.size()) * t.timescale, timing.ticks));
        j.string("time_base", "1/" + std::to_string(clock));
        if (!t.samples.empty() && t.timescale) {
            if (!wave) {
                j.number("start_pts", whole(timing.start * double(clock)));
                j.seconds("start_time", timing.start);
            }
            j.number("duration_ts", whole(timing.duration * double(clock)));
            j.seconds("duration", timing.duration);
            if (timing.ticks > 0) j.string("bit_rate", std::to_string(floored(double(timing.bytes) * 8 * t.timescale / double(timing.ticks))));
            bool frames = counted && t.codec == fourcc("lpcm") && t.sampleRate > 0;
            j.string("nb_frames", std::to_string(frames ? whole(double(timing.ticks) * t.sampleRate / t.timescale) : int64_t(t.samples.size())));
            begin = std::min(begin, timing.start);
            finish = std::max(finish, timing.start + timing.duration);
        }
        if (t.kind == Kind::Video && t.rotation) {
            j.array("side_data_list");
            j.item();
            j.string("side_data_type", "Display Matrix");
            j.number("rotation", t.rotation > 180 ? 360 - t.rotation : -t.rotation);
            j.close('}');
            j.close(']');
        }
        std::string language = matroska ? (t.language.empty() ? "eng" : t.language == "und" ? "" : t.language) : t.language;
        if (!language.empty()) {
            j.object("tags");
            j.string("language", language);
            j.close('}');
        }
        j.close('}');
    }
    j.close(']');
    if (packets) {
        j.array("packets");
        for (size_t index = 0; index < tracks.size(); ++index) {
            const Track& t = tracks[index];
            if (!t.timescale) continue;
            const char* kind = t.kind == Kind::Video ? "video" : t.kind == Kind::Audio ? "audio" : "data";
            double scale = double(t.timescale);
            for (const Sample& s : t.samples) {
                int64_t pts = shown(t, s.pts);
                int64_t dts = shown(t, s.dts);
                j.item();
                j.string("codec_type", kind);
                j.number("stream_index", int64_t(index));
                j.number("pts", pts);
                j.seconds("pts_time", double(pts) / scale);
                j.number("dts", dts);
                j.seconds("dts_time", double(dts) / scale);
                j.number("duration", s.duration);
                j.seconds("duration_time", double(s.duration) / scale);
                j.string("size", std::to_string(s.size));
                j.string("pos", std::to_string(s.offset));
                j.string("flags", s.sync ? "K__" : "___");
                j.close('}');
            }
        }
        j.close(']');
    }
    j.object("format");
    j.string("filename", path);
    j.number("nb_streams", int64_t(tracks.size()));
    j.string("format_name", family);
    if (finish > begin && reader->statedSeconds() > 0) finish = begin + reader->statedSeconds();
    if (finish > begin) {
        if (!wave && !adts) j.seconds("start_time", begin);
        j.seconds("duration", finish - begin);
        j.string("size", std::to_string(fileSize));
        j.string("bit_rate", std::to_string(floored(double(fileSize) * 8 / (finish - begin))));
    } else {
        j.string("size", std::to_string(fileSize));
    }
    if (!reader->tags().empty()) {
        j.object("tags");
        for (auto& tag : reader->tags()) j.string(tag.first.c_str(), tag.second);
        j.close('}');
    }
    j.close('}');
    j.close('}');
    j.text += '\n';
    return j.text;
}

}
