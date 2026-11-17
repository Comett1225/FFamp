#include "read_common.h"
#include "readers.h"
#include <algorithm>
#include <bit>
#include <cmath>
#include <cstring>

namespace absent {

namespace {

const uint32_t kEbml = 0x1A45DFA3, kDocType = 0x4282, kSegment = 0x18538067, kSeekHead = 0x114D9B74, kInfo = 0x1549A966, kTracks = 0x1654AE6B;
const uint32_t kCluster = 0x1F43B675, kCues = 0x1C53BB6B, kAttachments = 0x1941A469, kChapters = 0x1043A770, kTags = 0x1254C367;
const uint32_t kVoid = 0xEC, kChecksum = 0xBF, kTimestampScale = 0x2AD7B1, kTitle = 0x7BA9;
const uint32_t kTrackEntry = 0xAE, kTrackNumber = 0xD7, kTrackType = 0x83, kCodecId = 0x86, kCodecPrivate = 0x63A2, kDefaultDuration = 0x23E383;
const uint32_t kCodecDelay = 0x56AA, kLanguage = 0x22B59C, kVideo = 0xE0, kPixelWidth = 0xB0, kPixelHeight = 0xBA, kInterlaced = 0x9A, kColour = 0x55B0;
const uint32_t kMatrix = 0x55B1, kRange = 0x55B9, kTransfer = 0x55BA, kPrimaries = 0x55BB, kAudio = 0xE1, kSamplingFrequency = 0xB5, kChannels = 0x9F;
const uint32_t kBitDepth = 0x6264, kContentEncodings = 0x6D80, kContentEncoding = 0x6240, kContentCompression = 0x5034, kCompressionKind = 0x4254;
const uint32_t kCompressionSettings = 0x4255, kContentEncryption = 0x5035;
const uint32_t kTimestamp = 0xE7, kSimpleBlock = 0xA3, kBlockGroup = 0xA0, kBlock = 0xA1, kBlockDuration = 0x9B, kReferenceBlock = 0xFB;
const uint32_t kDiscardPadding = 0x75A2, kPreviousSize = 0xAB, kPosition = 0xA7, kSilentTracks = 0x5854, kBlockAdditions = 0x75A1, kCodecState = 0xA4;
const uint32_t kTag = 0x7373, kTargets = 0x63C0, kTargetTrack = 0x63C5, kSimpleTag = 0x67C8, kTagName = 0x45A3, kTagString = 0x4487;
const int64_t kUnknown = INT64_MIN;
const double kBillion = 1e9;
const uint32_t kSegmentDuration = 0x4489;

struct Element {
    uint32_t id = 0;
    uint64_t size = 0;
    size_t head = 0;
    bool open = false;
};

bool variable(const uint8_t* p, size_t n, uint64_t& value, size_t& length, bool& allOnes) {
    if (!n || !p[0]) return false;
    length = size_t(std::countl_zero(p[0])) + 1;
    if (length > n) return false;
    value = p[0] & (0xFFu >> length);
    allOnes = value == (0xFFu >> length);
    for (size_t i = 1; i < length; ++i) {
        value = (value << 8) | p[i];
        allOnes = allOnes && p[i] == 0xFF;
    }
    return true;
}

bool element(const uint8_t* p, size_t n, Element& e) {
    if (n < 2 || p[0] < 0x10) return false;
    size_t idLength = size_t(std::countl_zero(p[0])) + 1;
    if (n <= idLength) return false;
    e.id = 0;
    for (size_t i = 0; i < idLength; ++i) e.id = (e.id << 8) | p[i];
    size_t sizeLength = 0;
    if (!variable(p + idLength, n - idLength, e.size, sizeLength, e.open)) return false;
    e.head = idLength + sizeLength;
    return true;
}

template <class F>
void children(View v, F f) {
    while (v.left() >= 2) {
        Element e;
        if (!element(v.p + v.at, v.left(), e)) return;
        v.skip(e.head);
        if (e.open || e.size > v.left()) return;
        f(e.id, v.sub(size_t(e.size)));
    }
}

uint64_t number(View v) {
    uint64_t value = 0;
    for (size_t i = 0; i < v.n && i < 8; ++i) value = (value << 8) | v.p[i];
    return value;
}

int64_t signedNumber(View v) {
    if (!v.n || v.n > 8) return 0;
    uint64_t value = number(v);
    int shift = int(64 - 8 * v.n);
    return int64_t(value << shift) >> shift;
}

double real(View v) {
    if (v.n == 4) {
        uint32_t bits = be32(v.p);
        float value;
        memcpy(&value, &bits, 4);
        return value;
    }
    if (v.n == 8) {
        uint64_t bits = be64(v.p);
        double value;
        memcpy(&value, &bits, 8);
        return value;
    }
    return 0;
}

int projectionTurn(View projection) {
    uint64_t type = 0;
    double yaw = 0, pitch = 0, roll = 0;
    bool plain = true;
    children(projection, [&](uint32_t id, View value) {
        if (id == 0x7671) type = number(value);
        else if (id == 0x7672) plain = !value.n || value.p[0] == 0;
        else if (id == 0x7673) yaw = real(value);
        else if (id == 0x7674) pitch = real(value);
        else if (id == 0x7675) roll = real(value);
    });
    if (type != 0 || !plain || pitch != 0 || (yaw != 0 && yaw != 180 && yaw != -180) || !std::isfinite(roll)) return 0;
    const double radians = roll * 3.14159265358979323846 / 180.0;
    const bool flip = yaw != 0;
    const double angle = std::atan2(flip ? std::sin(radians) : -std::sin(radians), flip ? -std::cos(radians) : std::cos(radians)) * 180.0 / 3.14159265358979323846;
    return ((int(std::lround(angle / 90.0)) * 90) % 360 + 360) % 360;
}

std::string text(View v) {
    return std::string(reinterpret_cast<const char*>(v.p), strnlen(reinterpret_cast<const char*>(v.p), v.n));
}

bool topLevel(uint32_t id) {
    return id == kCluster || id == kCues || id == kTags || id == kSegment || id == kEbml || id == kSeekHead || id == kInfo || id == kTracks ||
        id == kAttachments || id == kChapters;
}

struct Lane {
    Track track;
    std::string codecId;
    uint64_t defaultDuration = 0;
    uint64_t codecDelay = 0;
    std::vector<uint8_t> prefix;
    bool frameAtom = false;
    bool usable = true;
    uint32_t fixedSamples = 0;
    std::vector<int64_t> times;
    std::vector<int64_t> spans;
    VorbisModes modes;
    int64_t discard = 0;
    std::vector<uint8_t> firstBytes;
    std::vector<std::pair<uint64_t, uint32_t>> masks;
};

bool starts(const std::string& value, const char* prefix) { return value.rfind(prefix, 0) == 0; }

std::vector<uint8_t> vpConfig(const Lane& lane, bool nine) {
    int profile = 0, depth = 8, chroma = 1, level = 0;
    bool full = lane.track.color.known && lane.track.color.full;
    int primaries = 2, transfer = 2, matrix = 2;
    const std::vector<uint8_t>& b = lane.firstBytes;
    if (nine && b.size() >= 8) {
        size_t at = 0;
        auto bit = [&]() {
            int value = at < b.size() * 8 ? (b[at >> 3] >> (7 - (at & 7))) & 1 : 0;
            ++at;
            return value;
        };
        auto bits = [&](int count) {
            int value = 0;
            for (int i = 0; i < count; ++i) value = (value << 1) | bit();
            return value;
        };
        if (bits(2) == 2) {
            profile = bit();
            profile |= bit() << 1;
            if (profile == 3) bit();
            if (!bit() && !bit()) {
                bits(2);
                if (bits(24) == 0x498342) {
                    if (profile >= 2) depth = bit() ? 12 : 10;
                    int space = bits(3);
                    if (space == 2) primaries = transfer = matrix = 1;
                    if (space != 7) {
                        full = bit();
                        if (profile & 1) {
                            int horizontal = bit(), vertical = bit();
                            chroma = horizontal && vertical ? 1 : horizontal ? 2 : 3;
                        }
                    } else {
                        full = true;
                        chroma = 3;
                        matrix = 0;
                    }
                }
            }
        }
        static const double limits[14][3] = {{10, 829440, 36864}, {11, 2764800, 73728}, {20, 4608000, 122880}, {21, 9216000, 245760},
            {30, 20736000, 552960}, {31, 36864000, 983040}, {40, 83558400, 2228224}, {41, 160432128, 2228224}, {50, 311951360, 8912896},
            {51, 588251136, 8912896}, {52, 1176502272, 8912896}, {60, 1176502272, 35651584}, {61, 2353004544.0, 35651584}, {62, 4706009088.0, 35651584}};
        double picture = double(lane.track.width) * lane.track.height;
        double rate = picture * lane.track.rate();
        level = 62;
        for (const auto& row : limits) {
            if (picture <= row[2] && rate <= row[1]) {
                level = int(row[0]);
                break;
            }
        }
    }
    if (lane.track.color.known) {
        primaries = lane.track.color.primaries;
        transfer = lane.track.color.transfer;
        matrix = lane.track.color.matrix;
    }
    return {1, 0, 0, 0, uint8_t(profile), uint8_t(level), uint8_t((depth << 4) | (chroma << 1) | (full ? 1 : 0)), uint8_t(primaries), uint8_t(transfer), uint8_t(matrix), 0, 0};
}

void describe(Lane& lane, std::vector<uint8_t>& secret, double outputRate) {
    Track& t = lane.track;
    const std::string& id = lane.codecId;
    auto set = [&](const char (&codec)[5], uint32_t type, bool keep) {
        t.codec = fourcc(codec);
        t.configType = type;
        if (keep) t.config = secret;
    };
    (void)outputRate;
    if (t.kind == Kind::Video) {
        if (id == "V_MPEG4/ISO/AVC") set("avc1", fourcc("avcC"), true);
        else if (id == "V_MPEGH/ISO/HEVC") set("hvc1", fourcc("hvcC"), true);
        else if (id == "V_AV1") set("av01", fourcc("av1C"), true);
        else if (id == "V_VP9") set("vp09", fourcc("vpcC"), false);
        else if (id == "V_VP8") set("vp08", fourcc("vpcC"), false);
        else if (id == "V_PRORES") {
            static const char* const kinds[] = {"apch", "apcn", "apcs", "apco", "ap4h", "ap4x"};
            t.codec = fourcc("apcn");
            for (const char* kind : kinds) if (secret.size() >= 4 && memcmp(secret.data(), kind, 4) == 0) t.codec = be32(secret.data());
            lane.frameAtom = true;
        } else if (id == "V_MPEG4/ISO/ASP" || id == "V_MPEG4/ISO/SP" || id == "V_MPEG4/ISO/AP") {
            set("mp4v", fourcc("esds"), false);
            t.config = mpeg4Descriptor(secret.data(), secret.size());
        } else if (id == "V_MPEG1") set("mp1v", 0, false);
        else if (id == "V_MPEG2") set("mp2v", 0, false);
        else if (id == "V_MJPEG") set("jpeg", 0, false);
        else if (id == "V_MPEG4/MS/V3") set("MP43", 0, false);
        else if (id == "V_MS/VFW/FOURCC" && secret.size() >= 40) {
            t.codec = videoFourcc(be32(secret.data() + 16), t.height);
            if (t.codec == fourcc("mp4v")) {
                t.configType = fourcc("esds");
                t.config = mpeg4Descriptor(secret.data() + 40, secret.size() - 40);
            }
            if (!t.codec || t.codec == fourcc("avc1") || t.codec == fourcc("hvc1")) {
                t.codec = be32(secret.data() + 16);
                t.configType = 0;
                t.config.clear();
            }
            if (vc1Codec(t.codec) || msmpeg4Codec(t.codec)) {
                t.configType = fourcc("bmih");
                t.config = secret;
            }
        } else {
            char name[4] = {' ', ' ', ' ', ' '};
            for (size_t i = 0; i < 4 && i + 2 < id.size(); ++i) name[i] = id[i + 2];
            t.codec = be32(reinterpret_cast<const uint8_t*>(name));
        }
        if ((t.codec == fourcc("avc1") || t.codec == fourcc("hvc1") || t.codec == fourcc("av01")) && t.config.empty()) lane.usable = false;
        return;
    }
    if (starts(id, "A_AAC")) {
        set("mp4a", fourcc("ASC "), true);
        if (t.config.size() < 2) {
            static const int rates[13] = {96000, 88200, 64000, 48000, 44100, 32000, 24000, 22050, 16000, 12000, 11025, 8000, 7350};
            int index = 4;
            for (int i = 0; i < 13; ++i) if (rates[i] == t.sampleRate) index = i;
            int object = id.find("MAIN") != std::string::npos ? 1 : id.find("SSR") != std::string::npos ? 3 : id.find("LTP") != std::string::npos ? 4 : 2;
            t.config = {uint8_t((object << 3) | (index >> 1)), uint8_t(((index & 1) << 7) | ((t.channels & 15) << 3))};
        }
        lane.fixedSamples = t.config.size() >= 2 && (t.config[0] >> 3) >= 1 && (t.config[0] >> 3) <= 4 && ((t.config[0] & 7) << 1 | t.config[1] >> 7) != 15 && (t.config[1] & 4) ? 960 : 1024;
    } else if (id == "A_OPUS") {
        set("Opus", fourcc("OpHd"), true);
        t.sampleRate = 48000;
        if (t.config.size() < 19 || memcmp(t.config.data(), "OpusHead", 8) != 0) lane.usable = false;
    } else if (id == "A_FLAC") {
        set("fLaC", fourcc("fLaC"), false);
        size_t at = secret.size() >= 4 && memcmp(secret.data(), "fLaC", 4) == 0 ? 8 : 0;
        if (secret.size() >= at + 34) t.config.assign(secret.begin() + at, secret.begin() + at + 34);
        FlacInfo info;
        if (flacInfo(t.config.data(), t.config.size(), info)) t.bits = info.bits;
        else lane.usable = false;
    } else if (id == "A_VORBIS") {
        set("vorb", fourcc("vorb"), true);
        std::vector<View> packets;
        if (!splitXiph(secret, packets) || packets.size() != 3 || !lane.modes.parse(packets[0].p, packets[0].n, packets[2].p, packets[2].n)) lane.usable = false;
    } else if (id == "A_MPEG/L3") set(".mp3", 0, false);
    else if (id == "A_MPEG/L2") set(".mp2", 0, false);
    else if (id == "A_MPEG/L1") set(".mp1", 0, false);
    else if (id == "A_PCM/INT/LIT" || id == "A_PCM/INT/BIG" || id == "A_PCM/FLOAT/IEEE") {
        set("lpcm", 0, false);
        t.floating = id == "A_PCM/FLOAT/IEEE";
        t.bigEndian = id == "A_PCM/INT/BIG" && t.bits > 8;
        const bool sized = t.floating ? t.bits == 32 || t.bits == 64 : t.bits == 8 || t.bits == 16 || t.bits == 24 || t.bits == 32;
        if (!sized || t.channels < 1) lane.usable = false;
    } else if (id == "A_EAC3") set("ec-3", 0, false);
    else if (starts(id, "A_AC3")) set("ac-3", 0, false);
    else if (id == "A_ALAC") set("alac", fourcc("alac"), true);
    else if (id == "A_TTA1") set("TTA1", fourcc("TTA1"), false);
    else if (id == "A_MS/ACM" && secret.size() >= 18) {
        uint32_t tag = extensibleTag(secret.data(), secret.size());
        t.codec = waveAudio(tag);
        t.configType = fourcc("wfex");
        t.config = secret;
        if (!t.codec) lane.usable = false;
    } else lane.usable = false;
}

uint32_t samplesIn(Lane& lane, const uint8_t* p, size_t have, uint32_t size) {
    const Track& t = lane.track;
    if (t.codec == fourcc("Opus")) return opusSamples(p, have);
    if (t.codec == fourcc("mp4a")) return lane.fixedSamples;
    if (t.codec == fourcc(".mp3") || t.codec == fourcc(".mp2") || t.codec == fourcc(".mp1")) {
        MpegAudio m;
        return have >= 4 && mpegAudio(p, m) ? uint32_t(m.samples) : 0;
    }
    if (t.codec == fourcc("fLaC")) {
        FlacFrame f;
        return flacFrame(p, have, f) ? f.blockSize : 0;
    }
    if (t.codec == fourcc("lpcm")) return size / uint32_t(t.channels * t.bits / 8);
    if (t.codec == fourcc("ac-3")) return 1536;
    if (t.codec == fourcc("ec-3")) {
        static const uint32_t blocks[4] = {256, 512, 768, 1536};
        return have >= 5 ? (p[4] >> 6) == 3 ? 1536 : blocks[(p[4] >> 4) & 3] : 0;
    }
    if (t.codec == fourcc("vorb")) return have && !(p[0] & 1) ? lane.modes.next(p, have) + 1 : 0;
    if (waveDecoded(t.codec)) return uint32_t(std::min<int64_t>(waveFrames(t, size), 0x7FFFFFFF));
    return 0;
}

class MatroskaReader : public FileReader {
public:
    std::vector<Lane> lanes;
    uint64_t scale = 1000000;
    double length = -1;
    bool haveTracks = false;
    uint64_t maskAt = 0;
    uint64_t maskSize = 0;

    bool readAlpha(const Track& track, uint32_t index, std::vector<uint8_t>& out) override {
        for (const Lane& l : lanes) {
            if (l.track.id != track.id) continue;
            return index < l.masks.size() && l.masks[index].second && source.load(l.masks[index].first, l.masks[index].second, out, 64u << 20);
        }
        return false;
    }

    double statedSeconds() const override { return length > 0 ? length * double(scale) / kBillion : -1; }

    Lane* lane(uint64_t number) {
        for (Lane& l : lanes) if (l.track.id == number) return &l;
        return nullptr;
    }

    bool read(const Track& track, uint32_t index, uint8_t* out) override {
        if (index >= track.samples.size()) return false;
        const Sample& s = track.samples[index];
        const Lane* found = nullptr;
        for (const Lane& l : lanes) if (l.track.id == track.id) found = &l;
        if (!found) return false;
        size_t added = found->prefix.size() + (found->frameAtom ? 8 : 0);
        if (s.size < added) return false;
        if (found->frameAtom) {
            Bytes atom;
            atom.u32(s.size);
            atom.u32(fourcc("icpf"));
            memcpy(out, atom.d.data(), 8);
            out += 8;
        }
        if (!found->prefix.empty()) memcpy(out, found->prefix.data(), found->prefix.size());
        return source.read(s.offset, out + found->prefix.size(), s.size - added);
    }

    void info(View v) {
        children(v, [&](uint32_t id, View body) {
            if (id == kTimestampScale && number(body)) scale = std::min<uint64_t>(number(body), 1000000000000ull);
            else if (id == kTitle && !text(body).empty()) found["title"] = text(body);
            else if (id == kSegmentDuration && std::isfinite(real(body))) length = real(body);
        });
    }

    void trackList(View v) {
        children(v, [&](uint32_t id, View entry) {
            if (id != kTrackEntry) return;
            Lane lane;
            Track& t = lane.track;
            uint64_t type = 0;
            std::vector<uint8_t> secret;
            double rate = 8000, outputRate = 0;
            t.channels = 1;
            children(entry, [&](uint32_t field, View body) {
                if (field == kTrackNumber) t.id = uint32_t(number(body));
                else if (field == kTrackType) type = number(body);
                else if (field == kCodecId) lane.codecId = text(body);
                else if (field == kCodecPrivate) secret.assign(body.p, body.p + body.n);
                else if (field == kDefaultDuration) lane.defaultDuration = number(body);
                else if (field == kCodecDelay) lane.codecDelay = number(body);
                else if (field == kLanguage) t.language = text(body);
                else if (field == kVideo) {
                    children(body, [&](uint32_t inner, View value) {
                        if (inner == kPixelWidth) t.width = int(std::min<uint64_t>(number(value), 65535));
                        else if (inner == kPixelHeight) t.height = int(std::min<uint64_t>(number(value), 65535));
                        else if (inner == kInterlaced) t.interlaced = number(value) == 1;
                        else if (inner == 0x7670) t.rotation = projectionTurn(value);
                        else if (inner == kColour) {
                            children(value, [&](uint32_t part, View item) {
                                int given = int(std::min<uint64_t>(number(item), 255));
                                if (part == kRange) t.color.full = given == 2;
                                if (given == 2 || given == 0) return;
                                if (part == kMatrix) t.color.matrix = given;
                                else if (part == kTransfer) t.color.transfer = given;
                                else if (part == kPrimaries) t.color.primaries = given;
                                else return;
                                t.color.known = true;
                            });
                        }
                    });
                } else if (field == kAudio) {
                    children(body, [&](uint32_t inner, View value) {
                        if (inner == kSamplingFrequency) rate = real(value);
                        else if (inner == 0x78B5) outputRate = real(value);
                        else if (inner == kChannels) t.channels = int(std::min<uint64_t>(number(value), 255));
                        else if (inner == kBitDepth) t.bits = int(std::min<uint64_t>(number(value), 64));
                    });
                } else if (field == kContentEncodings) {
                    children(body, [&](uint32_t inner, View encoding) {
                        if (inner != kContentEncoding) return;
                        children(encoding, [&](uint32_t part, View item) {
                            if (part == kContentEncryption) lane.usable = false;
                            if (part != kContentCompression) return;
                            uint64_t kind = 0;
                            std::vector<uint8_t> settings;
                            children(item, [&](uint32_t leaf, View value) {
                                if (leaf == kCompressionKind) kind = number(value);
                                else if (leaf == kCompressionSettings) settings.assign(value.p, value.p + value.n);
                            });
                            if (kind == 3) lane.prefix = settings;
                            else lane.usable = false;
                        });
                    });
                }
            });
            if (!t.id || this->lane(t.id)) return;
            t.kind = type == 1 ? Kind::Video : type == 2 ? Kind::Audio : Kind::Other;
            if (t.kind == Kind::Audio) {
                if (!(rate >= 1 && rate <= 16777216)) rate = 8000;
                t.sampleRate = int(std::lround(rate));
            }
            if (t.kind == Kind::Other) lane.usable = false;
            else describe(lane, secret, outputRate);
            if (t.codec != fourcc("lpcm") && t.codec != fourcc("fLaC") && t.codec != fourcc("alac") && t.codec != fourcc("TTA1")) t.bits = 0;
            if (!lane.usable) {
                t.codec = 0;
                t.configType = 0;
                t.config.clear();
            }
            if (t.kind == Kind::Audio) t.timescale = uint32_t(t.sampleRate);
            else t.timescale = uint64_t(kBillion) % scale == 0 ? uint32_t(uint64_t(kBillion) / scale) : uint32_t(kBillion);
            lanes.push_back(std::move(lane));
        });
        haveTracks = true;
    }

    void tagList(View v) {
        children(v, [&](uint32_t id, View tag) {
            if (id != kTag) return;
            bool global = true;
            children(tag, [&](uint32_t part, View body) {
                if (part == kTargets) children(body, [&](uint32_t inner, View value) { if (inner == kTargetTrack && number(value)) global = false; });
            });
            if (!global) return;
            children(tag, [&](uint32_t part, View body) {
                if (part != kSimpleTag) return;
                std::string name, value;
                children(body, [&](uint32_t inner, View item) {
                    if (inner == kTagName) name = text(item);
                    else if (inner == kTagString) value = text(item);
                });
                for (char& c : name) c = char(c >= 'A' && c <= 'Z' ? c + 32 : c);
                if (!name.empty() && !value.empty() && !found.count(name)) found[name] = value;
            });
        });
    }

    void frame(Lane& lane, uint64_t offset, uint64_t size, int64_t time, int64_t span, bool key) {
        if (!lane.usable || size > 0x7FFFFFF0u - lane.prefix.size() || !size) return;
        Track& t = lane.track;
        size_t have = size_t(std::min<uint64_t>(size, 16));
        const uint8_t* p = source.view(offset, have);
        if (!p) return;
        if (t.samples.empty()) {
            lane.firstBytes.assign(p, p + have);
            if (lane.frameAtom && have >= 8 && memcmp(p + 4, "icpf", 4) == 0) lane.frameAtom = false;
        }
        Sample s;
        s.offset = offset;
        s.size = uint32_t(size + lane.prefix.size() + (lane.frameAtom ? 8 : 0));
        s.sync = key || t.kind == Kind::Audio;
        if (t.kind == Kind::Audio) {
            if (lane.prefix.empty()) {
                s.duration = samplesIn(lane, p, have, uint32_t(size));
            } else {
                uint8_t joined[16];
                size_t fromPrefix = std::min<size_t>(lane.prefix.size(), 16);
                memcpy(joined, lane.prefix.data(), fromPrefix);
                size_t fromFile = std::min(have, 16 - fromPrefix);
                memcpy(joined + fromPrefix, p, fromFile);
                s.duration = samplesIn(lane, joined, fromPrefix + fromFile, s.size);
            }
        }
        t.samples.push_back(s);
        lane.times.push_back(time);
        lane.spans.push_back(span);
        if (maskSize && maskSize < (64u << 20) && (t.codec == fourcc("vp08") || t.codec == fourcc("vp09"))) {
            lane.masks.resize(t.samples.size());
            lane.masks.back() = {maskAt, uint32_t(maskSize)};
            t.alpha = true;
        }
        maskSize = 0;
    }

    void block(uint64_t at, uint64_t size, int64_t clusterTime, bool simple, bool key, int64_t span, int64_t discard) {
        if (size < 4) return;
        const uint8_t* p = source.view(at, size_t(std::min<uint64_t>(size, 12)));
        if (!p) return;
        uint64_t trackNumber = 0;
        size_t length = 0;
        bool ones = false;
        if (!variable(p, size_t(std::min<uint64_t>(size, 8)), trackNumber, length, ones) || size < length + 3) return;
        Lane* lane = this->lane(trackNumber);
        if (!lane || !lane->usable) return;
        int64_t ticks = clusterTime + int16_t(be16(p + length));
        uint8_t flags = p[length + 2];
        int64_t time = std::clamp<int64_t>(ticks, 0, (int64_t(1) << 62) / int64_t(scale)) * int64_t(scale);
        if (simple) key = flags & 0x80;
        if (discard) lane->discard = discard;
        size_t head = length + 3;
        int lacing = (flags >> 1) & 3;
        if (!lacing) {
            frame(*lane, at + head, size - head, time, span, key);
            return;
        }
        std::vector<uint8_t> whole;
        if (size < head + 1 || !source.load(at, size, whole, 64u << 20)) return;
        size_t count = size_t(whole[head]) + 1;
        size_t pos = head + 1;
        std::vector<uint64_t> sizes;
        uint64_t used = 0;
        if (lacing == 1) {
            for (size_t i = 0; i + 1 < count; ++i) {
                uint64_t value = 0;
                for (;;) {
                    if (pos >= whole.size()) return;
                    uint8_t b = whole[pos++];
                    value += b;
                    if (b != 255) break;
                }
                sizes.push_back(value);
                used += value;
            }
        } else if (lacing == 3) {
            int64_t last = 0;
            for (size_t i = 0; i + 1 < count; ++i) {
                uint64_t value = 0;
                size_t bytes = 0;
                if (pos >= whole.size() || !variable(whole.data() + pos, whole.size() - pos, value, bytes, ones)) return;
                pos += bytes;
                last = i == 0 ? int64_t(value) : last + int64_t(value) - ((int64_t(1) << (7 * bytes - 1)) - 1);
                if (last < 0 || uint64_t(last) > whole.size() - pos) return;
                sizes.push_back(uint64_t(last));
                used += uint64_t(last);
            }
        } else {
            uint64_t each = (whole.size() - pos) / count;
            sizes.assign(count - 1, each);
            used = each * (count - 1);
        }
        if (used > whole.size() - pos) return;
        sizes.push_back(whole.size() - pos - used);
        uint64_t offset = at + pos;
        for (size_t i = 0; i < sizes.size(); ++i) {
            frame(*lane, offset, sizes[i], i == 0 ? time : kUnknown, -1, key);
            offset += sizes[i];
        }
    }

    void additions(uint64_t at, uint64_t end, int depth) {
        uint64_t kind = 1, foundAt = 0, foundSize = 0;
        while (end - at >= 2) {
            size_t have = size_t(std::min<uint64_t>(end - at, 12));
            const uint8_t* p = source.view(at, have);
            Element e;
            if (!p || !element(p, have, e)) break;
            uint64_t body = at + e.head;
            if (e.open || e.size > end - body) break;
            if (e.id == 0xA6 && depth == 0) {
                additions(body, body + e.size, 1);
            } else if (e.id == 0xEE && e.size <= 8 && depth == 1) {
                const uint8_t* value = source.view(body, size_t(e.size));
                if (value) kind = number(View{value, size_t(e.size)});
            } else if (e.id == 0xA5 && depth == 1) {
                foundAt = body;
                foundSize = e.size;
            }
            at = body + e.size;
        }
        if (depth == 1 && kind == 1 && foundSize) {
            maskAt = foundAt;
            maskSize = foundSize;
        }
    }

    void group(uint64_t at, uint64_t end, int64_t clusterTime) {
        uint64_t blockAt = 0, blockSize = 0;
        bool reference = false;
        int64_t span = -1, discard = 0;
        while (end - at >= 2) {
            size_t have = size_t(std::min<uint64_t>(end - at, 12));
            const uint8_t* p = source.view(at, have);
            Element e;
            if (!p || !element(p, have, e)) break;
            uint64_t body = at + e.head;
            if (e.open || e.size > end - body) break;
            if (e.id == kBlock) {
                blockAt = body;
                blockSize = e.size;
            } else if (e.id == kReferenceBlock) {
                reference = true;
            } else if ((e.id == kBlockDuration || e.id == kDiscardPadding) && e.size <= 8) {
                const uint8_t* value = source.view(body, size_t(e.size));
                if (!value) break;
                View v{value, size_t(e.size)};
                if (e.id == kBlockDuration) span = int64_t(std::min<uint64_t>({number(v), 1ull << 40, uint64_t((int64_t(1) << 62) / int64_t(scale))})) * int64_t(scale);
                else discard = signedNumber(v);
            } else if (e.id == kBlockAdditions) {
                additions(body, body + e.size, 0);
            }
            at = body + e.size;
        }
        if (blockAt) block(blockAt, blockSize, clusterTime, false, !reference, span, discard);
        maskSize = 0;
    }

    uint64_t cluster(uint64_t at, uint64_t end, bool open) {
        int64_t time = 0;
        while (end - at >= 2) {
            size_t have = size_t(std::min<uint64_t>(end - at, 12));
            const uint8_t* p = source.view(at, have);
            Element e;
            if (!p || !element(p, have, e)) return at;
            if (open && topLevel(e.id)) return at;
            const bool known = e.id == kTimestamp || e.id == kSimpleBlock || e.id == kBlockGroup || e.id == kPreviousSize || e.id == kPosition || e.id == kVoid ||
                e.id == kChecksum || e.id == kSilentTracks || e.id == kBlockAdditions || e.id == kCodecState;
            uint64_t body = at + e.head;
            if (e.open || e.size > end - body || (open && !known)) return at;
            if (e.id == kTimestamp && e.size <= 8) {
                const uint8_t* value = source.view(body, size_t(e.size));
                if (!value) return at;
                time = int64_t(std::min<uint64_t>(number(View{value, size_t(e.size)}), 1ull << 62) );
            } else if (e.id == kSimpleBlock) {
                block(body, e.size, time, true, false, -1, 0);
            } else if (e.id == kBlockGroup) {
                group(body, body + e.size, time);
            }
            at = body + e.size;
        }
        return end;
    }

    bool master(uint64_t at, uint64_t size, std::vector<uint8_t>& out) { return source.load(at, size, out, 64u << 20); }

    void finish(Lane& lane);
};

void MatroskaReader::finish(Lane& lane) {
    if (lane.track.codec == fourcc("TTA1")) {
        Track& t = lane.track;
        const double nanoseconds = length > 0 ? length * double(scale) : 0;
        const int64_t whole = nanoseconds < 9.2e18 ? int64_t(nanoseconds) : 0;
        const uint32_t frames = uint32_t((__int128(whole) * t.sampleRate + 500000000) / 1000000000);
        const uint8_t head[22] = {'T', 'T', 'A', '1', 1, 0, uint8_t(t.channels), uint8_t(t.channels >> 8), uint8_t(t.bits), uint8_t(t.bits >> 8), uint8_t(t.sampleRate),
            uint8_t(t.sampleRate >> 8), uint8_t(t.sampleRate >> 16), uint8_t(t.sampleRate >> 24), uint8_t(frames), uint8_t(frames >> 8), uint8_t(frames >> 16),
            uint8_t(frames >> 24), 0, 0, 0, 0};
        t.config.assign(head, head + 22);
    }
    Track& t = lane.track;
    std::vector<int64_t>& times = lane.times;
    const size_t count = t.samples.size();
    if (!count) return;
    const double rate = t.timescale;
    if (times[0] == kUnknown) times[0] = 0;
    if (t.kind == Kind::Audio && t.codec == fourcc("mp4a") && count > 2) {
        size_t probe = std::min<size_t>(count - 1, 50);
        while (probe > 1 && times[probe] == kUnknown) --probe;
        double each = times[probe] != kUnknown ? double(times[probe] - times[0]) / double(probe) * rate / kBillion : 0;
        if (each > lane.fixedSamples * 1.5) {
            lane.fixedSamples *= 2;
            for (Sample& s : t.samples) s.duration = lane.fixedSamples;
        }
    }
    for (size_t i = 0; i < count;) {
        if (times[i] != kUnknown) {
            ++i;
            continue;
        }
        size_t first = i - 1;
        size_t next = i;
        while (next < count && times[next] == kUnknown) ++next;
        int64_t step = 0;
        if (lane.defaultDuration) step = int64_t(std::min<uint64_t>(lane.defaultDuration, 1ull << 40));
        else if (t.kind == Kind::Audio && t.samples[first].duration) step = -1;
        else if (next < count) step = (times[next] - times[first]) / int64_t(next - first);
        for (size_t k = i; k < next; ++k) {
            times[k] = step >= 0 ? times[k - 1] + step : times[k - 1] + std::llround(t.samples[k - 1].duration * kBillion / rate);
        }
        i = next;
    }
    auto ticks = [&](int64_t time) { return std::llround(double(time) * rate / kBillion); };
    if (t.kind == Kind::Audio) {
        const int64_t slack = std::max<int64_t>(1, ticks(int64_t(scale) * 2));
        const bool vorbis = t.codec == fourcc("vorb");
        const int64_t joined = vorbis ? std::max<int64_t>(slack, lane.modes.blocks[1] / 4) : slack;
        std::vector<bool> known(count);
        for (size_t i = 0; i < count; ++i) {
            known[i] = t.samples[i].duration != 0;
            if (vorbis && known[i]) --t.samples[i].duration;
        }
        int64_t expected = -1;
        for (size_t i = 0; i < count; ++i) {
            Sample& s = t.samples[i];
            int64_t stated = ticks(times[i]);
            s.pts = expected >= 0 && std::llabs(stated - expected) <= (i >= 2 ? joined : slack) ? expected : stated;
            expected = known[i] ? s.pts + s.duration : -1;
        }
        for (size_t i = 0; i < count; ++i) {
            Sample& s = t.samples[i];
            s.dts = s.pts;
            if (known[i]) continue;
            if (lane.spans[i] > 0) s.duration = uint32_t(std::min<int64_t>(ticks(lane.spans[i]), 0x7FFFFFFF));
            else if (i + 1 < count) s.duration = uint32_t(std::clamp<int64_t>(t.samples[i + 1].pts - s.pts, 0, 0x7FFFFFFF));
            else if (lane.defaultDuration) s.duration = uint32_t(std::min<int64_t>(ticks(int64_t(std::min<uint64_t>(lane.defaultDuration, 1ull << 40))), 0x7FFFFFFF));
            else if (i > 0) s.duration = t.samples[i - 1].duration;
        }
        int64_t end = 0;
        for (const Sample& s : t.samples) end = std::max(end, s.pts + int64_t(s.duration));
        t.mediaStart = std::min<int64_t>(end, ticks(int64_t(std::min<uint64_t>(lane.codecDelay, 1ull << 40))));
        if (lane.discard > 0) {
            int64_t cut = end - ticks(lane.discard);
            if (cut > t.mediaStart && cut < end) t.mediaEnd = cut;
        }
    } else {
        bool sorted = true;
        for (size_t i = 0; i < count; ++i) {
            t.samples[i].pts = ticks(times[i]);
            sorted = sorted && (i == 0 || t.samples[i].pts >= t.samples[i - 1].pts);
        }
        std::vector<uint32_t> byTime(count);
        for (uint32_t i = 0; i < count; ++i) byTime[i] = i;
        std::stable_sort(byTime.begin(), byTime.end(), [&](uint32_t a, uint32_t b) { return t.samples[a].pts < t.samples[b].pts; });
        int64_t usual = lane.defaultDuration ? ticks(int64_t(std::min<uint64_t>(lane.defaultDuration, 1ull << 40))) : 0;
        if (!usual && count > 1) usual = (t.samples[byTime.back()].pts - t.samples[byTime.front()].pts) / int64_t(count - 1);
        usual = std::clamp<int64_t>(usual, 1, 0x7FFFFFFF);
        for (size_t k = 0; k < count; ++k) {
            Sample& s = t.samples[byTime[k]];
            int64_t span = lane.spans[byTime[k]] > 0 ? ticks(lane.spans[byTime[k]]) : k + 1 < count ? t.samples[byTime[k + 1]].pts - s.pts : usual;
            s.duration = uint32_t(std::clamp<int64_t>(span, 0, 0x7FFFFFFF));
        }
        size_t delay = 0;
        if (!sorted) for (size_t k = 0; k < count; ++k) if (byTime[k] > k) delay = std::max(delay, size_t(byTime[k] - k));
        for (size_t i = 0; i < count; ++i) {
            Sample& s = t.samples[i];
            if (sorted) s.dts = s.pts;
            else if (i >= delay) s.dts = t.samples[byTime[i - delay]].pts;
            else s.dts = t.samples[byTime[0]].pts - int64_t(delay - i) * usual;
        }
        if (t.codec == fourcc("vp09") || t.codec == fourcc("vp08")) {
            indexTrack(t);
            t.config = vpConfig(lane, t.codec == fourcc("vp09"));
        }
    }
    indexTrack(t);
}

}

std::unique_ptr<Reader> openMatroska(const std::string& path, std::string* error) {
    auto fail = [&](const char* text) {
        if (error) *error = text;
        return nullptr;
    };
    auto reader = std::make_unique<MatroskaReader>();
    Source& source = reader->source;
    if (!source.open(path)) return fail("cannot open file");
    const uint64_t total = source.size();
    reader->name = "mkv";
    static const uint8_t clusterMark[4] = {0x1F, 0x43, 0xB6, 0x75};
    std::vector<uint8_t> held;
    uint64_t at = 0;
    uint64_t end = total;
    bool inside = false;
    while (end - at >= 2) {
        size_t have = size_t(std::min<uint64_t>(end - at, 12));
        const uint8_t* p = source.view(at, have);
        Element e;
        bool parsed = p && element(p, have, e);
        uint64_t body = at + e.head;
        if (parsed && !inside) {
            if (e.id == kEbml && !e.open && e.size <= total - body) {
                if (reader->master(body, e.size, held)) {
                    children(View{held.data(), held.size()}, [&](uint32_t id, View value) { if (id == kDocType && text(value) == "webm") reader->name = "webm"; });
                }
                at = body + e.size;
                continue;
            }
            if (e.id == kSegment) {
                inside = true;
                if (!e.open && e.size < total - body) end = body + e.size;
                at = body;
                continue;
            }
            if ((e.id == kVoid || e.id == kChecksum) && !e.open && e.size <= total - body) {
                at = body + e.size;
                continue;
            }
            return fail("not a Matroska or WebM file");
        }
        const bool fits = parsed && !e.open && e.size <= end - body;
        if (parsed && e.id == kCluster && reader->haveTracks) {
            uint64_t stop = e.open ? end : std::min(end, body + e.size);
            at = reader->cluster(body, stop, e.open || !fits);
            if (!e.open && fits && at < stop) at = stop;
            continue;
        }
        if (fits && (e.id == kInfo || e.id == kTags || (e.id == kTracks && !reader->haveTracks))) {
            if (reader->master(body, e.size, held)) {
                View v{held.data(), held.size()};
                if (e.id == kInfo) reader->info(v);
                else if (e.id == kTags) reader->tagList(v);
                else reader->trackList(v);
            }
            at = body + e.size;
            continue;
        }
        if (fits && (e.id == kSeekHead || e.id == kCues || e.id == kAttachments || e.id == kChapters || e.id == kVoid || e.id == kChecksum || e.id == kTracks || e.id == kCluster)) {
            at = body + e.size;
            continue;
        }
        at = source.find(at + 1, end, clusterMark, 4);
    }
    if (!inside) return fail("not a Matroska or WebM file");
    if (!reader->haveTracks) return fail("no track list in this Matroska file");
    for (Lane& lane : reader->lanes) {
        reader->finish(lane);
        reader->list.push_back(lane.track);
    }
    for (Lane& lane : reader->lanes) {
        lane.track.samples.clear();
        lane.track.samples.shrink_to_fit();
        lane.times = {};
        lane.spans = {};
    }
    if (reader->list.empty()) return fail("no tracks");
    return reader;
}

}
