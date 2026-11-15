#include "absent/android.h"
#include "aac_config.h"
#include "readers.h"
#include "soft_video.h"
#include <algorithm>
#include <cstring>
#include <set>

namespace absent::android {

namespace {

const uint8_t kStart[4] = {0, 0, 0, 1};

void appendNal(std::vector<uint8_t>& out, const uint8_t* p, size_t n) {
    out.insert(out.end(), kStart, kStart + 4);
    out.insert(out.end(), p, p + n);
}

std::vector<std::vector<uint8_t>> avcConfig(const std::vector<uint8_t>& c) {
    std::vector<uint8_t> sps, pps;
    if (c.size() < 7) return {};
    size_t at = 5;
    for (int k = 0, count = c[at++] & 31; k < count && at + 2 <= c.size(); ++k) {
        const size_t n = (size_t(c[at]) << 8) | c[at + 1];
        at += 2;
        if (at + n > c.size()) return {};
        appendNal(sps, c.data() + at, n);
        at += n;
    }
    if (at >= c.size()) return {sps};
    for (int k = 0, count = c[at++]; k < count && at + 2 <= c.size(); ++k) {
        const size_t n = (size_t(c[at]) << 8) | c[at + 1];
        at += 2;
        if (at + n > c.size()) return {};
        appendNal(pps, c.data() + at, n);
        at += n;
    }
    return {sps, pps};
}

std::vector<std::vector<uint8_t>> hevcConfig(const std::vector<uint8_t>& c) {
    std::vector<uint8_t> all;
    if (c.size() < 23) return {};
    size_t at = 22;
    for (int a = 0, arrays = c[at++]; a < arrays && at + 3 <= c.size(); ++a) {
        at++;
        const int count = (c[at] << 8) | c[at + 1];
        at += 2;
        for (int k = 0; k < count && at + 2 <= c.size(); ++k) {
            const size_t n = (size_t(c[at]) << 8) | c[at + 1];
            at += 2;
            if (at + n > c.size()) return {};
            appendNal(all, c.data() + at, n);
            at += n;
        }
    }
    return {all};
}

std::vector<uint8_t> nanos(int64_t value) {
    std::vector<uint8_t> out(8);
    for (int i = 0; i < 8; ++i) out[i] = uint8_t(uint64_t(value) >> (8 * i));
    return out;
}

class Soft : public SoftVideo {
public:
    std::unique_ptr<SoftDecoder> codec;
    std::multiset<int64_t> pending;
    std::vector<SoftPicture> held;
    const char* name() const override { return codec->name(); }
    bool decode(const uint8_t* data, size_t size, int64_t stamp, std::vector<SoftPicture>& out, int64_t later) override {
        pending.insert(stamp);
        const bool ok = codec->decode(data, size, stamp, [&](const Picture& p) { keep(p); });
        while (pending.size() > 16) pending.erase(pending.begin());
        release(out, false, later);
        return ok;
    }
    void drain(std::vector<SoftPicture>& out) override {
        codec->drain([&](const Picture& p) { keep(p); });
        release(out, true, INT64_MAX);
    }
    void reset() override {
        codec->reset();
        pending.clear();
        held.clear();
    }
    void keep(const Picture& p) {
        if (auto found = pending.find(p.stamp); found != pending.end()) pending.erase(found);
        SoftPicture picture;
        picture.width = p.width;
        picture.height = p.height;
        picture.stamp = p.stamp;
        pictureToI420(p, picture.i420);
        held.push_back(std::move(picture));
    }
    void release(std::vector<SoftPicture>& out, bool all, int64_t later) {
        std::stable_sort(held.begin(), held.end(), [](const SoftPicture& a, const SoftPicture& b) { return a.stamp < b.stamp; });
        const int64_t limit = pending.empty() ? later : std::min(later, *pending.begin());
        size_t n = 0;
        while (n < held.size() && (all || held[n].stamp <= limit)) out.push_back(std::move(held[n++]));
        held.erase(held.begin(), held.begin() + long(n));
        if (all) pending.clear();
    }
};

}

std::string mime(const Track& t) {
    const uint32_t c = t.codec;
    if (t.kind == Kind::Video) {
        if (t.configType == fourcc("avcC")) return "video/avc";
        if (t.configType == fourcc("hvcC")) return "video/hevc";
        if (c == fourcc("vp09")) return "video/x-vnd.on2.vp9";
        if (c == fourcc("vp08")) return "video/x-vnd.on2.vp8";
        if (c == fourcc("av01")) return "video/av01";
        if (c == fourcc("mp4v")) return "video/mp4v-es";
        if (c == fourcc("h263") || c == fourcc("s263")) return "video/3gpp";
        if (c == fourcc("mp2v") || c == fourcc("mp1v")) return "video/mpeg2";
        if (vc1Codec(c) || c == fourcc("WMV1") || c == fourcc("WMV2")) return "video/x-ms-wmv";
        if (msmpeg4Codec(c)) return "video/x-msmpeg4";
        return "video/x-ffamp-" + fourccText(c);
    }
    if (t.kind == Kind::Audio) {
        if (c == fourcc("mp4a")) return "audio/mp4a-latm";
        if (c == fourcc(".mp3")) return "audio/mpeg";
        if (c == fourcc("Opus")) return "audio/opus";
        if (c == fourcc("fLaC")) return "audio/flac";
        if (c == fourcc("vorb")) return "audio/vorbis";
        if (c == fourcc("ac-3")) return "audio/ac3";
        if (c == fourcc("ec-3")) return "audio/eac3";
        if (c == fourcc("alac")) return "audio/alac";
        if (c == fourcc("lpcm")) return "audio/raw";
        if (c == fourcc("samr")) return "audio/3gpp";
        if (c == fourcc("sawb")) return "audio/amr-wb";
        if (c == fourcc("wma1") || c == fourcc("wma2") || c == fourcc("wmap") || c == fourcc("wmal") || c == fourcc("wmas")) return "audio/x-ms-wma";
        return "audio/x-ffamp-" + fourccText(c);
    }
    return "application/octet-stream";
}

std::vector<std::vector<uint8_t>> codecConfig(const Track& t) {
    const std::vector<uint8_t>& c = t.config;
    if (t.configType == fourcc("avcC")) return avcConfig(c);
    if (t.configType == fourcc("hvcC")) return hevcConfig(c);
    if (t.configType == fourcc("av1C") && !c.empty()) return {c};
    if (t.configType == fourcc("ASC ") && !c.empty()) return {c};
    if (t.configType == fourcc("esds")) {
        ElementaryStream stream;
        if (!parseElementaryStream(View{c.data(), c.size()}, stream) || !stream.specific.n) return {};
        return {std::vector<uint8_t>(stream.specific.p, stream.specific.p + stream.specific.n)};
    }
    if (t.configType == fourcc("OpHd") && c.size() >= 12) {
        const int preskip = c[10] | (c[11] << 8);
        return {c, nanos(int64_t(preskip) * 1000000000 / 48000), nanos(80000000)};
    }
    if (t.configType == fourcc("fLaC")) {
        if (c.size() >= 4 && memcmp(c.data(), "fLaC", 4) == 0) return {c};
        if (c.size() != 34) return {};
        std::vector<uint8_t> out = {'f', 'L', 'a', 'C', 0x80, 0, 0, 34};
        out.insert(out.end(), c.begin(), c.end());
        return {out};
    }
    return {};
}

int nalLengthSize(const Track& t) {
    if (t.configType == fourcc("avcC") && t.config.size() > 4) return (t.config[4] & 3) + 1;
    if (t.configType == fourcc("hvcC") && t.config.size() > 21) return (t.config[21] & 3) + 1;
    return 0;
}

int codecProfile(const Track& t) {
    const std::vector<uint8_t>& c = t.config;
    if (t.configType == fourcc("avcC") && c.size() > 3) {
        switch (c[1]) {
        case 66: return 0x01;
        case 77: return 0x02;
        case 88: return 0x04;
        case 100: return 0x08;
        case 110: return 0x10;
        case 122: return 0x20;
        case 244: return 0x40;
        default: return 0;
        }
    }
    if (t.configType == fourcc("hvcC") && c.size() > 12) {
        const int profile = c[1] & 31;
        if (profile == 1) return 0x01;
        if (profile == 2) return t.color.known && t.color.transfer == 16 ? 0x1000 : 0x02;
        if (profile == 3) return 0x04;
    }
    return 0;
}

int codecLevel(const Track& t) {
    const std::vector<uint8_t>& c = t.config;
    if (t.configType == fourcc("avcC") && c.size() > 3) {
        static const int idc[] = {10, 9, 11, 12, 13, 20, 21, 22, 30, 31, 32, 40, 41, 42, 50, 51, 52, 60, 61, 62};
        if (c[3] == 11 && (c[2] & 0x10) && c[1] != 100) return 0x02;
        for (int i = 0; i < int(sizeof(idc) / sizeof(idc[0])); ++i) if (idc[i] == c[3]) return 1 << i;
    }
    if (t.configType == fourcc("hvcC") && c.size() > 12) {
        static const int idc[] = {30, 60, 63, 90, 93, 120, 123, 150, 153, 156, 180, 183, 186};
        const bool high = (c[1] >> 5) & 1;
        for (int i = 0; i < int(sizeof(idc) / sizeof(idc[0])); ++i) if (idc[i] == c[12]) return 1 << (2 * i + (high ? 1 : 0));
    }
    return 0;
}

size_t annexB(const uint8_t* in, size_t size, int lengthSize, uint8_t* out, size_t capacity) {
    size_t at = 0, written = 0;
    while (at + size_t(lengthSize) <= size) {
        size_t n = 0;
        for (int i = 0; i < lengthSize; ++i) n = (n << 8) | in[at + i];
        at += size_t(lengthSize);
        if (n > size - at || written + 4 + n > capacity) return 0;
        memcpy(out + written, kStart, 4);
        memmove(out + written + 4, in + at, n);
        written += 4 + n;
        at += n;
    }
    return written;
}

std::unique_ptr<SoftVideo> SoftVideo::open(const Track& t, std::string* error) {
    std::unique_ptr<Soft> soft(new Soft);
    if (vc1Codec(t.codec) && t.configType == fourcc("bmih")) soft->codec = openVc1(t.codec, t.config, t.width, t.height, error);
    else if (msmpeg4Codec(t.codec)) soft->codec = openMsmpeg4(t.codec, t.config, t.width, t.height, error);
#ifdef ABSENT_XVID
    else if (t.codec == fourcc("mp4v") && t.configType == fourcc("esds")) soft->codec = openXvid(t.config, t.width, t.height);
#endif
    else if (error) *error = "no software decoder for this video codec";
    if (!soft->codec) return nullptr;
    return soft;
}

}
