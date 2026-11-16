#ifdef ABSENT_VAAPI
#include "vaapi.h"
#include "../nal.h"
#include <algorithm>
#include <cstring>
#include <memory>

namespace absent {

namespace {

const uint8_t kIntra[64] = {
    16, 16, 16, 16, 17, 18, 21, 24, 16, 16, 16, 16, 17, 19, 22, 25, 16, 16, 17, 18, 20, 22, 25, 29, 16, 16, 18, 21, 24, 27, 31, 36,
    17, 17, 20, 24, 30, 35, 41, 47, 18, 19, 22, 27, 35, 44, 54, 65, 21, 22, 25, 31, 41, 54, 70, 88, 24, 25, 29, 36, 47, 65, 88, 115,
};
const uint8_t kInter[64] = {
    16, 16, 16, 16, 17, 18, 20, 24, 16, 16, 16, 17, 18, 20, 24, 25, 16, 16, 17, 18, 20, 24, 25, 28, 16, 17, 18, 20, 24, 25, 28, 33,
    17, 18, 20, 24, 25, 28, 33, 41, 18, 20, 24, 25, 28, 33, 41, 54, 20, 24, 25, 28, 33, 41, 54, 71, 24, 25, 28, 33, 41, 54, 71, 91,
};

struct Scan {
    uint8_t four[16];
    uint8_t eight[64];
    Scan() {
        fill(four, 4);
        fill(eight, 8);
    }
    static void fill(uint8_t* out, int size) {
        int i = 0, x = 0, y = 0;
        while (i < size * size) {
            for (; y >= 0; --y, ++x) if (x < size && y < size) out[i++] = uint8_t(y * size + x);
            y = x;
            x = 0;
        }
    }
};
const Scan kScan;

int ceilLog2(int n) {
    int bits = 0;
    while ((1 << bits) < n) ++bits;
    return bits;
}

struct Lists {
    uint8_t sl[4][6][64];
    uint8_t dc[2][6];
};

void defaults(Lists& l) {
    for (int m = 0; m < 6; ++m) {
        memset(l.sl[0][m], 16, 64);
        l.dc[0][m] = l.dc[1][m] = 16;
        for (int s = 1; s < 4; ++s) memcpy(l.sl[s][m], m < 3 ? kIntra : kInter, 64);
    }
}

bool listData(BitReader& b, Lists& l) {
    for (int s = 0; s < 4; ++s) {
        for (int m = 0; m < 6; m += s == 3 ? 3 : 1) {
            if (!b.bit()) {
                uint32_t delta = b.ue();
                if (delta > 5) return false;
                delta *= s == 3 ? 3 : 1;
                if (!delta) continue;
                if (uint32_t(m) < delta) return false;
                memcpy(l.sl[s][m], l.sl[s][m - delta], 64);
                if (s > 1) l.dc[s - 2][m] = l.dc[s - 2][m - delta];
            } else {
                int next = 8;
                if (s > 1) {
                    const int dc = b.se();
                    if (dc < -7 || dc > 247) return false;
                    next = dc + 8;
                    l.dc[s - 2][m] = uint8_t(next);
                }
                for (int i = 0; i < (s ? 64 : 16); ++i) {
                    next = int((unsigned(next) + 256u + unsigned(b.se())) % 256u);
                    l.sl[s][m][s ? kScan.eight[i] : kScan.four[i]] = uint8_t(next);
                }
            }
        }
    }
    return b.ok;
}

struct ShortRps {
    int delta[32] = {};
    uint32_t used = 0;
    int negative = 0;
    int count = 0;
};

struct Sps {
    std::vector<uint8_t> raw;
    int chroma = 0;
    bool separate = false;
    int width = 0;
    int height = 0;
    int crop[4] = {};
    int bits = 8;
    int pocBits = 4;
    int maxDec = 1;
    int log2MinCb = 3;
    int log2DiffCb = 0;
    int log2MinTb = 2;
    int log2DiffTb = 0;
    int depthInter = 0;
    int depthIntra = 0;
    bool scaling = false;
    Lists list{};
    bool amp = false;
    bool sao = false;
    bool pcm = false;
    int pcmBits = 0;
    int pcmBitsChroma = 0;
    int pcmLog2Min = 0;
    int pcmLog2Max = 0;
    bool pcmNoFilter = false;
    int rpsCount = 0;
    ShortRps rps[64];
    bool longTerm = false;
    int longCount = 0;
    int ltLsb[32] = {};
    uint32_t ltUsed = 0;
    bool tmvp = false;
    bool strong = false;
    int ctbW = 0;
    int ctbH = 0;
};

struct Pps {
    std::vector<uint8_t> raw;
    std::shared_ptr<const Sps> sps;
    int spsId = 0;
    bool dependent = false;
    bool outputFlag = false;
    int extraBits = 0;
    bool signHiding = false;
    bool cabacInit = false;
    int l0 = 1;
    int l1 = 1;
    int initQp = 0;
    bool constrainedIntra = false;
    bool transformSkip = false;
    bool cuQpDelta = false;
    int cuQpDepth = 0;
    int cbOffset = 0;
    int crOffset = 0;
    bool sliceChroma = false;
    bool weighted = false;
    bool biWeighted = false;
    bool bypass = false;
    bool tiles = false;
    bool wpp = false;
    int columns = 1;
    int rows = 1;
    int colWidth[19] = {};
    int rowHeight[21] = {};
    bool tilesFilter = true;
    bool slicesFilter = false;
    bool dbControl = false;
    bool dbOverride = false;
    bool dbDisable = false;
    int beta = 0;
    int tc = 0;
    bool scaling = false;
    Lists list{};
    bool listsMod = false;
    int mergeLevel = 2;
    bool headerExt = false;
};

bool shortRps(BitReader& b, ShortRps& r, const Sps& s, int index, bool slice) {
    r = ShortRps();
    if ((slice || index) && s.rpsCount && b.bit()) {
        const ShortRps* ref = nullptr;
        if (slice) {
            const uint32_t d = b.ue();
            if (d >= uint32_t(s.rpsCount)) return false;
            ref = &s.rps[s.rpsCount - 1 - int(d)];
        } else {
            ref = &s.rps[index - 1];
        }
        const int sign = int(b.bit());
        const uint32_t magnitude = b.ue();
        if (magnitude >= 32768) return false;
        const int deltaRps = (1 - 2 * sign) * int(magnitude + 1);
        int delta[32] = {};
        uint8_t used[32] = {};
        int k = 0, negative = 0;
        for (int i = 0; i <= ref->count; ++i) {
            const int u = int(b.bit());
            const int keep = u ? 1 : int(b.bit());
            if (!keep) continue;
            if (k >= 32) return false;
            delta[k] = i < ref->count ? deltaRps + ref->delta[i] : deltaRps;
            used[k] = uint8_t(u);
            if (delta[k] < 0) ++negative;
            ++k;
        }
        if (k >= 32) return false;
        int order[32];
        for (int i = 0; i < k; ++i) order[i] = i;
        std::stable_sort(order, order + k, [&](int x, int y) { return delta[x] < delta[y]; });
        std::reverse(order, order + negative);
        for (int i = 0; i < k; ++i) {
            r.delta[i] = delta[order[i]];
            r.used |= uint32_t(used[order[i]]) << i;
        }
        r.count = k;
        r.negative = negative;
    } else {
        const uint32_t negative = b.ue(), positive = b.ue();
        if (negative >= 16 || positive >= 16) return false;
        r.negative = int(negative);
        r.count = int(negative + positive);
        int prev = 0;
        for (int i = 0; i < r.count; ++i) {
            if (i == r.negative) prev = 0;
            const uint32_t d = b.ue();
            if (d >= 32768) return false;
            prev += i < r.negative ? -int(d + 1) : int(d + 1);
            r.delta[i] = prev;
            r.used |= b.bit() << i;
        }
    }
    return b.ok;
}

std::shared_ptr<Sps> parseSps(const uint8_t* nal, size_t n, int& id) {
    std::vector<uint8_t> body = unescape(nal + 2, n - 2);
    BitReader b{body.data(), body.size()};
    auto s = std::make_shared<Sps>();
    s->raw.assign(nal, nal + n);
    b.skip(4);
    const int sub = int(b.bits(3)) + 1;
    if (sub > 7) return nullptr;
    b.skip(1 + 88 + 8);
    bool profile[8] = {}, level[8] = {};
    for (int i = 0; i < sub - 1; ++i) {
        profile[i] = b.bit();
        level[i] = b.bit();
    }
    if (sub > 1) b.skip(size_t(2 * (9 - sub)));
    for (int i = 0; i < sub - 1; ++i) b.skip((profile[i] ? 88 : 0) + (level[i] ? 8 : 0));
    const uint32_t spsId = b.ue();
    if (spsId >= 16) return nullptr;
    id = int(spsId);
    const uint32_t chroma = b.ue();
    if (chroma > 3) return nullptr;
    s->chroma = int(chroma);
    if (chroma == 3) s->separate = b.bit();
    if (s->separate) s->chroma = 0;
    const uint32_t width = b.ue(), height = b.ue();
    if (!width || !height || width > 16384 || height > 16384) return nullptr;
    s->width = int(width);
    s->height = int(height);
    if (b.bit()) {
        const int unitX = s->chroma == 1 || s->chroma == 2 ? 2 : 1, unitY = s->chroma == 1 ? 2 : 1;
        uint64_t window[4];
        for (uint64_t& v : window) v = b.ue();
        if ((window[0] + window[1]) * unitX >= width || (window[2] + window[3]) * unitY >= height) return nullptr;
        for (int i = 0; i < 4; ++i) s->crop[i] = int(window[i]) * (i < 2 ? unitX : unitY);
    }
    const uint32_t luma = b.ue(), chromaBits = b.ue();
    if (luma > 8 || chromaBits > 8 || (s->chroma && chromaBits != luma)) return nullptr;
    s->bits = int(luma) + 8;
    const uint32_t poc = b.ue();
    if (poc > 12) return nullptr;
    s->pocBits = int(poc) + 4;
    const bool ordering = b.bit();
    for (int i = ordering ? 0 : sub - 1; i < sub; ++i) {
        const uint32_t dec = b.ue(), reorder = b.ue();
        b.ue();
        if (dec > 15) return nullptr;
        s->maxDec = int(dec) + 1;
        if (reorder > dec) {
            if (reorder > 15) return nullptr;
            s->maxDec = int(reorder) + 1;
        }
    }
    const uint32_t minCb = b.ue(), diffCb = b.ue(), minTb = b.ue(), diffTb = b.ue();
    if (minCb > 3 || diffCb > 3 || minTb > 3 || diffTb > 3) return nullptr;
    s->log2MinCb = int(minCb) + 3;
    s->log2DiffCb = int(diffCb);
    s->log2MinTb = int(minTb) + 2;
    s->log2DiffTb = int(diffTb);
    const int log2Ctb = s->log2MinCb + s->log2DiffCb;
    if (log2Ctb < 4 || log2Ctb > 6 || s->log2MinTb >= s->log2MinCb || s->log2MinTb + s->log2DiffTb > std::min(log2Ctb, 5)) return nullptr;
    const uint32_t inter = b.ue(), intra = b.ue();
    if (inter > uint32_t(log2Ctb - s->log2MinTb) || intra > uint32_t(log2Ctb - s->log2MinTb)) return nullptr;
    s->depthInter = int(inter);
    s->depthIntra = int(intra);
    if ((s->scaling = b.bit())) {
        defaults(s->list);
        if (b.bit() && !listData(b, s->list)) return nullptr;
    }
    s->amp = b.bit();
    s->sao = b.bit();
    if ((s->pcm = b.bit())) {
        s->pcmBits = int(b.bits(4)) + 1;
        s->pcmBitsChroma = int(b.bits(4)) + 1;
        const uint32_t minPcm = b.ue(), diffPcm = b.ue();
        if (minPcm > 2 || diffPcm > 2) return nullptr;
        s->pcmLog2Min = int(minPcm) + 3;
        s->pcmLog2Max = s->pcmLog2Min + int(diffPcm);
        if (std::max(s->pcmBits, s->pcmBitsChroma) > s->bits) return nullptr;
        s->pcmNoFilter = b.bit();
    }
    const uint32_t sets = b.ue();
    if (sets > 64) return nullptr;
    s->rpsCount = int(sets);
    for (int i = 0; i < s->rpsCount; ++i) if (!shortRps(b, s->rps[i], *s, i, false)) return nullptr;
    if ((s->longTerm = b.bit())) {
        const uint32_t count = b.ue();
        if (count > 32) return nullptr;
        s->longCount = int(count);
        for (int i = 0; i < s->longCount; ++i) {
            s->ltLsb[i] = int(b.bits(s->pocBits));
            s->ltUsed |= b.bit() << i;
        }
    }
    s->tmvp = b.bit();
    s->strong = b.bit();
    if (!b.ok || s->width % (1 << s->log2MinCb) || s->height % (1 << s->log2MinCb)) return nullptr;
    s->ctbW = (s->width + (1 << log2Ctb) - 1) >> log2Ctb;
    s->ctbH = (s->height + (1 << log2Ctb) - 1) >> log2Ctb;
    return s;
}

std::shared_ptr<Pps> parsePps(const uint8_t* nal, size_t n, const std::shared_ptr<const Sps>* spsList, int& id) {
    std::vector<uint8_t> body = unescape(nal + 2, n - 2);
    BitReader b{body.data(), body.size()};
    auto p = std::make_shared<Pps>();
    p->raw.assign(nal, nal + n);
    const uint32_t ppsId = b.ue(), spsId = b.ue();
    if (ppsId >= 64 || spsId >= 16 || !spsList[spsId]) return nullptr;
    id = int(ppsId);
    p->spsId = int(spsId);
    p->sps = spsList[spsId];
    const Sps& s = *p->sps;
    p->dependent = b.bit();
    p->outputFlag = b.bit();
    p->extraBits = int(b.bits(3));
    p->signHiding = b.bit();
    p->cabacInit = b.bit();
    const uint32_t l0 = b.ue(), l1 = b.ue();
    if (l0 >= 15 || l1 >= 15) return nullptr;
    p->l0 = int(l0) + 1;
    p->l1 = int(l1) + 1;
    p->initQp = b.se();
    p->constrainedIntra = b.bit();
    p->transformSkip = b.bit();
    if ((p->cuQpDelta = b.bit())) {
        const uint32_t depth = b.ue();
        if (depth > uint32_t(s.log2DiffCb)) return nullptr;
        p->cuQpDepth = int(depth);
    }
    p->cbOffset = b.se();
    p->crOffset = b.se();
    if (p->cbOffset < -12 || p->cbOffset > 12 || p->crOffset < -12 || p->crOffset > 12) return nullptr;
    p->sliceChroma = b.bit();
    p->weighted = b.bit();
    p->biWeighted = b.bit();
    p->bypass = b.bit();
    p->tiles = b.bit();
    p->wpp = b.bit();
    if (p->tiles) {
        const uint32_t columns = b.ue(), rows = b.ue();
        if (columns >= uint32_t(s.ctbW) || rows >= uint32_t(s.ctbH) || columns >= 19 || rows >= 21) return nullptr;
        p->columns = int(columns) + 1;
        p->rows = int(rows) + 1;
        if (b.bit()) {
            for (int i = 0; i < p->columns; ++i) p->colWidth[i] = ((i + 1) * s.ctbW) / p->columns - (i * s.ctbW) / p->columns;
            for (int i = 0; i < p->rows; ++i) p->rowHeight[i] = ((i + 1) * s.ctbH) / p->rows - (i * s.ctbH) / p->rows;
        } else {
            uint64_t sum = 0;
            for (int i = 0; i < p->columns - 1; ++i) sum += p->colWidth[i] = int(std::min<uint32_t>(b.ue(), 65535)) + 1;
            if (sum >= uint64_t(s.ctbW)) return nullptr;
            p->colWidth[p->columns - 1] = s.ctbW - int(sum);
            sum = 0;
            for (int i = 0; i < p->rows - 1; ++i) sum += p->rowHeight[i] = int(std::min<uint32_t>(b.ue(), 65535)) + 1;
            if (sum >= uint64_t(s.ctbH)) return nullptr;
            p->rowHeight[p->rows - 1] = s.ctbH - int(sum);
        }
        p->tilesFilter = b.bit();
    }
    p->slicesFilter = b.bit();
    if ((p->dbControl = b.bit())) {
        p->dbOverride = b.bit();
        if (!(p->dbDisable = b.bit())) {
            const int beta = b.se(), tc = b.se();
            if (beta < -6 || beta > 6 || tc < -6 || tc > 6) return nullptr;
            p->beta = 2 * beta;
            p->tc = 2 * tc;
        }
    }
    if ((p->scaling = b.bit())) {
        defaults(p->list);
        if (!listData(b, p->list)) return nullptr;
    }
    p->listsMod = b.bit();
    const uint32_t merge = b.ue();
    if (merge > uint32_t(s.log2MinCb + s.log2DiffCb)) return nullptr;
    p->mergeLevel = int(merge) + 2;
    p->headerExt = b.bit();
    return b.ok ? p : nullptr;
}

int computePoc(int bits, int tid0, int lsb, int type) {
    const int most = 1 << bits, prevLsb = tid0 % most, prevMsb = tid0 - prevLsb;
    int msb = prevMsb;
    if (lsb < prevLsb && prevLsb - lsb >= most / 2) msb = prevMsb + most;
    else if (lsb > prevLsb && lsb - prevLsb > most / 2) msb = prevMsb - most;
    if (type >= 16 && type <= 18) msb = 0;
    return msb + lsb;
}

struct Slice {
    int pps = 0;
    bool dependent = false;
    int address = 0;
    int type = 2;
    bool output = true;
    int plane = 0;
    int pocLsb = 0;
    int poc = 0;
    bool rpsFromSps = false;
    bool hasRps = false;
    ShortRps rps;
    int rpsBits = 0;
    int ltCount = 0;
    int ltPoc[32] = {};
    bool ltUsed[32] = {};
    bool ltMsb[32] = {};
    bool tmvp = false;
    bool saoLuma = false;
    bool saoChroma = false;
    int refs[2] = {};
    bool mod[2] = {};
    int entry[2][16] = {};
    bool mvdZero = false;
    bool cabacInit = false;
    int colList = 0;
    int colIdx = 0;
    int lumaDenom = 0;
    int chromaDenom = 0;
    int lumaWeight[2][16] = {};
    int lumaOffset[2][16] = {};
    int chromaWeight[2][16][2] = {};
    int chromaOffset[2][16][2] = {};
    int mergeCand = 5;
    int qpDelta = 0;
    int cbOffset = 0;
    int crOffset = 0;
    bool dbDisable = false;
    int beta = 0;
    int tc = 0;
    bool lfAcross = false;
    int dataOffset = 0;
};

constexpr int kBefore = 0, kAfter = 1, kFollow = 2, kLong = 3, kLongFollow = 4;

struct Entry {
    VASurfaceID surface;
    int poc;
};

struct Ref {
    VASurfaceID surface;
    int poc;
    bool longTerm;
    bool missing;
};

struct HevcDecoder : SoftDecoder {
    VaSession session{&wanted};
    Color color;
    int lengthSize = 4;
    int pool = 0;
    std::shared_ptr<const Sps> spsList[16];
    std::shared_ptr<const Pps> ppsList[64];
    std::shared_ptr<const Sps> sps;
    std::shared_ptr<const Pps> pps;
    Slice sh;
    std::vector<Ref> dpb;
    std::vector<Entry> sets[5];
    bool eos = true;
    bool lastEos = false;
    bool skipping = false;
    bool sliced = false;
    bool noRasl = false;
    int pocTid0 = 0;
    bool started = false;
    bool shown = false;
    VASurfaceID target = VA_INVALID_SURFACE;
    int poc = 0;
    int firstType = 0;
    VAPictureParameterBufferHEVC pic{};
    VAIQMatrixBufferHEVC iq{};
    bool matrix = false;
    uint8_t refList[2][15];
    std::vector<VASliceParameterBufferHEVC> slices;
    std::vector<std::pair<const uint8_t*, size_t>> chunks;

    const char* name() const override { return "vaapi"; }

    void reset() override {
        dpb.clear();
        slices.clear();
        chunks.clear();
        started = false;
        skipping = false;
        eos = true;
    }

    void param(const uint8_t* nal, size_t n) {
        if (n < 3 || (nal[0] & 1) || (nal[1] >> 3)) return;
        const int type = (nal[0] >> 1) & 63;
        int id = 0;
        if (type == 33) {
            std::shared_ptr<Sps> s = parseSps(nal, n, id);
            if (!s || (spsList[id] && spsList[id]->raw == s->raw)) return;
            for (auto& p : ppsList) if (p && p->spsId == id) p.reset();
            spsList[id] = std::move(s);
        } else if (type == 34) {
            std::shared_ptr<Pps> p = parsePps(nal, n, spsList, id);
            if (p && !(ppsList[id] && ppsList[id]->raw == p->raw)) ppsList[id] = std::move(p);
        }
    }

    bool longTerms(BitReader& b, const Sps& s) {
        sh.ltCount = 0;
        if (!s.longTerm) return true;
        const uint32_t fromSps = s.longCount > 0 ? b.ue() : 0, own = b.ue();
        if (fromSps > uint32_t(s.longCount) || uint64_t(fromSps) + own > 32) return false;
        const int count = int(fromSps + own);
        int64_t prevMsb = 0;
        for (int i = 0; i < count; ++i) {
            if (i < int(fromSps)) {
                const int index = s.longCount > 1 ? int(b.bits(ceilLog2(s.longCount))) : 0;
                sh.ltPoc[i] = s.ltLsb[index];
                sh.ltUsed[i] = (s.ltUsed >> index) & 1;
            } else {
                sh.ltPoc[i] = int(b.bits(s.pocBits));
                sh.ltUsed[i] = b.bit();
            }
            if ((sh.ltMsb[i] = b.bit())) {
                int64_t delta = b.ue();
                if (i && i != int(fromSps)) delta += prevMsb;
                const int64_t value = int64_t(sh.ltPoc[i]) + sh.poc - delta * (int64_t(1) << s.pocBits) - sh.pocLsb;
                if (value != int32_t(value)) return false;
                sh.ltPoc[i] = int(value);
                prevMsb = delta;
            }
        }
        sh.ltCount = count;
        return b.ok;
    }

    bool weights(BitReader& b, const Sps& s) {
        const uint32_t luma = b.ue();
        if (luma > 7) return false;
        sh.lumaDenom = int(luma);
        if (s.chroma) {
            const int64_t c = int64_t(luma) + b.se();
            if (c < 0 || c > 7) return false;
            sh.chromaDenom = int(c);
        }
        for (int l = 0; l < (sh.type == 0 ? 2 : 1); ++l) {
            const int n = sh.refs[l];
            const uint32_t lumaFlags = b.bits(n), chromaFlags = s.chroma ? b.bits(n) : 0;
            for (int i = 0; i < n; ++i) {
                const uint32_t flag = 1u << (n - 1 - i);
                sh.lumaWeight[l][i] = 1 << sh.lumaDenom;
                sh.lumaOffset[l][i] = 0;
                if (lumaFlags & flag) {
                    const int d = b.se();
                    if (d < -128 || d > 127) return false;
                    sh.lumaWeight[l][i] += d;
                    sh.lumaOffset[l][i] = b.se();
                }
                for (int c = 0; c < 2; ++c) {
                    sh.chromaWeight[l][i][c] = 1 << sh.chromaDenom;
                    sh.chromaOffset[l][i][c] = 0;
                    if (!(chromaFlags & flag)) continue;
                    const int w = b.se(), o = b.se();
                    if (w < -128 || w > 127 || o < -(1 << 17) || o > (1 << 17)) return false;
                    sh.chromaWeight[l][i][c] += w;
                    sh.chromaOffset[l][i][c] = std::clamp(o - ((128 * sh.chromaWeight[l][i][c]) >> sh.chromaDenom) + 128, -128, 127);
                }
            }
        }
        return b.ok;
    }

    int frameRefs() const {
        int count = 0;
        if (sh.hasRps) for (int i = 0; i < sh.rps.count; ++i) count += (sh.rps.used >> i) & 1;
        for (int i = 0; i < sh.ltCount; ++i) count += sh.ltUsed[i];
        return count;
    }

    bool header(BitReader& b, int type, bool first) {
        const bool irap = type >= 16 && type <= 23, idr = type == 19 || type == 20;
        b.skip(irap ? 2 : 1);
        const uint32_t id = b.ue();
        if (id >= 64 || !ppsList[id] || (!first && ppsList[id] != pps)) return false;
        const Pps& p = *ppsList[id];
        const Sps& s = *p.sps;
        sh.pps = int(id);
        sh.dependent = false;
        sh.address = 0;
        if (!first) {
            if (p.dependent) sh.dependent = b.bit();
            if (sh.dependent && !sliced) return false;
            sh.address = int(b.bits(ceilLog2(s.ctbW * s.ctbH)));
            if (sh.address >= s.ctbW * s.ctbH) return false;
        }
        if (!sh.dependent) {
            b.skip(size_t(p.extraBits));
            const uint32_t kind = b.ue();
            if (kind > 2 || (irap && kind != 2)) return false;
            sh.type = int(kind);
            sh.output = p.outputFlag ? b.bit() : true;
            if (s.separate) sh.plane = int(b.bits(2));
            if (!idr) {
                sh.pocLsb = int(b.bits(s.pocBits));
                const int value = computePoc(s.pocBits, pocTid0, sh.pocLsb, type);
                if (first) sh.poc = value;
                sh.rpsFromSps = b.bit();
                const size_t at = b.at;
                if (!sh.rpsFromSps) {
                    if (!shortRps(b, sh.rps, s, 0, true)) return false;
                } else {
                    if (!s.rpsCount) return false;
                    const int bits = ceilLog2(s.rpsCount);
                    const uint32_t index = bits ? b.bits(bits) : 0;
                    if (index >= uint32_t(s.rpsCount)) return false;
                    sh.rps = s.rps[index];
                }
                sh.rpsBits = int(b.at - at);
                sh.hasRps = true;
                if (!longTerms(b, s)) return false;
                sh.tmvp = s.tmvp && b.bit();
            } else {
                sh.poc = sh.pocLsb = 0;
                sh.rpsFromSps = sh.hasRps = sh.tmvp = false;
                sh.rpsBits = sh.ltCount = 0;
            }
            sh.saoLuma = s.sao && b.bit();
            sh.saoChroma = s.sao && s.chroma && b.bit();
            sh.refs[0] = sh.refs[1] = 0;
            if (sh.type != 2) {
                sh.refs[0] = p.l0;
                if (sh.type == 0) sh.refs[1] = p.l1;
                if (b.bit()) {
                    const uint32_t a = b.ue(), c = sh.type == 0 ? b.ue() : 0;
                    if (a >= 15 || c >= 15) return false;
                    sh.refs[0] = int(a) + 1;
                    if (sh.type == 0) sh.refs[1] = int(c) + 1;
                }
                sh.mod[0] = sh.mod[1] = false;
                const int total = frameRefs();
                if (!total) return false;
                if (p.listsMod && total > 1) {
                    const int bits = ceilLog2(total);
                    for (int l = 0; l < (sh.type == 0 ? 2 : 1); ++l) {
                        if (!(sh.mod[l] = b.bit())) continue;
                        for (int i = 0; i < sh.refs[l]; ++i) sh.entry[l][i] = int(b.bits(bits));
                    }
                }
                if (sh.type == 0) sh.mvdZero = b.bit();
                sh.cabacInit = p.cabacInit && b.bit();
                sh.colIdx = 0;
                if (sh.tmvp) {
                    sh.colList = sh.type == 0 ? !b.bit() : 0;
                    if (sh.refs[sh.colList] > 1) {
                        const uint32_t index = b.ue();
                        if (index >= uint32_t(sh.refs[sh.colList])) return false;
                        sh.colIdx = int(index);
                    }
                }
                if (((p.weighted && sh.type == 1) || (p.biWeighted && sh.type == 0)) && !weights(b, s)) return false;
                const uint32_t merge = b.ue();
                if (merge > 4) return false;
                sh.mergeCand = 5 - int(merge);
            }
            sh.qpDelta = b.se();
            sh.cbOffset = sh.crOffset = 0;
            if (p.sliceChroma) {
                sh.cbOffset = b.se();
                sh.crOffset = b.se();
                if (sh.cbOffset < -12 || sh.cbOffset > 12 || sh.crOffset < -12 || sh.crOffset > 12) return false;
            }
            if (p.dbControl) {
                if (p.dbOverride && b.bit()) {
                    if (!(sh.dbDisable = b.bit())) {
                        const int beta = b.se(), tc = b.se();
                        if (beta < -6 || beta > 6 || tc < -6 || tc > 6) return false;
                        sh.beta = 2 * beta;
                        sh.tc = 2 * tc;
                    }
                } else {
                    sh.dbDisable = p.dbDisable;
                    sh.beta = p.beta;
                    sh.tc = p.tc;
                }
            } else {
                sh.dbDisable = false;
                sh.beta = sh.tc = 0;
            }
            sh.lfAcross = p.slicesFilter && (sh.saoLuma || sh.saoChroma || !sh.dbDisable) ? b.bit() : p.slicesFilter;
        }
        if (p.tiles || p.wpp) {
            const uint32_t count = b.ue();
            if (count > b.left() || count > 65535) return false;
            if (count) {
                const uint32_t length = b.ue();
                if (length > 31) return false;
                b.skip(size_t(count) * (length + 1));
            }
        }
        if (p.headerExt) {
            const uint32_t length = b.ue();
            if (uint64_t(length) * 8 > b.left()) return false;
            b.skip(size_t(length) * 8);
        }
        if (!b.bit()) return false;
        sh.dataOffset = int((b.at + 7) / 8);
        const int qp = 26 + p.initQp + sh.qpDelta;
        return b.ok && qp <= 51 && qp >= -6 * (s.bits - 8) && !(sh.dependent && !sh.address);
    }

    bool add(int list, int value, bool longTerm, bool msb, bool irap, std::vector<char>& keep) {
        if (msb && value == poc) return false;
        const int mask = msb ? -1 : (1 << sps->pocBits) - 1;
        int found = -1;
        for (size_t i = 0; i < dpb.size() && found < 0; ++i) if ((dpb[i].poc & mask) == value && (msb || dpb[i].poc != poc)) found = int(i);
        if (sets[list].size() >= 16) return false;
        if (!irap && list != kFollow && list != kLongFollow && (found < 0 || dpb[size_t(found)].missing)) return false;
        if (found < 0) {
            std::vector<VASurfaceID> busy;
            for (const Ref& r : dpb) busy.push_back(r.surface);
            const VASurfaceID stand = session.spare(busy);
            if (stand == VA_INVALID_SURFACE) return false;
            dpb.push_back({stand, value, false, true});
            keep.push_back(0);
            found = int(dpb.size()) - 1;
        }
        Ref& r = dpb[size_t(found)];
        r.longTerm = longTerm;
        keep[size_t(found)] = 1;
        sets[list].push_back({r.surface, r.poc});
        return true;
    }

    bool frameRps(bool irap) {
        dpb.erase(std::remove_if(dpb.begin(), dpb.end(), [](const Ref& r) { return r.missing; }), dpb.end());
        for (auto& set : sets) set.clear();
        std::vector<char> keep(dpb.size(), 0);
        bool ok = true;
        if (sh.hasRps) {
            for (int i = 0; i < sh.rps.count && ok; ++i) {
                const int list = !((sh.rps.used >> i) & 1) ? kFollow : i < sh.rps.negative ? kBefore : kAfter;
                ok = add(list, poc + sh.rps.delta[i], false, true, irap, keep);
            }
        }
        for (int i = 0; i < sh.ltCount && ok; ++i) ok = add(sh.ltUsed[i] ? kLong : kLongFollow, sh.ltPoc[i], true, sh.ltMsb[i], irap, keep);
        size_t kept = 0;
        for (size_t i = 0; i < dpb.size(); ++i) if (keep[i]) dpb[kept++] = dpb[i];
        dpb.resize(kept);
        return ok;
    }

    int rpsType(VASurfaceID surface) const {
        static const int kinds[3][2] = {{kBefore, VA_PICTURE_HEVC_RPS_ST_CURR_BEFORE}, {kAfter, VA_PICTURE_HEVC_RPS_ST_CURR_AFTER}, {kLong, VA_PICTURE_HEVC_RPS_LT_CURR}};
        for (const auto& kind : kinds) for (const Entry& e : sets[kind[0]]) if (e.surface == surface) return kind[1];
        return 0;
    }

    bool begin(int type, int tid) {
        const std::shared_ptr<const Pps>& p = ppsList[sh.pps];
        if (p->sps != sps) {
            dpb.clear();
            sps = p->sps;
        }
        pps = p;
        const Sps& s = *sps;
        if (s.chroma != 1 || (s.bits != 8 && s.bits != 10)) return false;
        const bool deep = s.bits > 8;
        const int count = std::max(17, s.maxDec + 2);
        if (!session.ready() || s.width != session.width() || s.height != session.height() || deep != session.tenBit() || count > pool) {
            dpb.clear();
            pool = 0;
            if (!session.open(deep ? VAProfileHEVCMain10 : VAProfileHEVCMain, s.width, s.height, deep, count)) return false;
            pool = count;
        }
        const bool irap = type >= 16 && type <= 23;
        if (type == 19 || type == 20) dpb.clear();
        poc = sh.poc;
        firstType = type;
        if (irap) noRasl = type <= 20 || lastEos;
        if (tid == 0 && !(type <= 9 && (type % 2 == 0 || type >= 6))) pocTid0 = poc;
        for (const Ref& r : dpb) if (r.poc == poc) return false;
        if (!frameRps(irap)) return false;
        std::vector<VASurfaceID> busy;
        for (const Ref& r : dpb) busy.push_back(r.surface);
        target = session.spare(busy);
        if (target == VA_INVALID_SURFACE) return false;

        pic = VAPictureParameterBufferHEVC{};
        pic.CurrPic.picture_id = target;
        pic.CurrPic.pic_order_cnt = poc;
        for (size_t i = 0; i < 15; ++i) {
            VAPictureHEVC& f = pic.ReferenceFrames[i];
            f.picture_id = VA_INVALID_ID;
            f.flags = VA_PICTURE_HEVC_INVALID;
            if (i >= dpb.size()) continue;
            f.picture_id = dpb[i].surface;
            f.pic_order_cnt = dpb[i].poc;
            f.flags = uint32_t(rpsType(dpb[i].surface)) | (dpb[i].longTerm ? VA_PICTURE_HEVC_LONG_TERM_REFERENCE : 0);
        }
        pic.pic_width_in_luma_samples = uint16_t(s.width);
        pic.pic_height_in_luma_samples = uint16_t(s.height);
        auto& f = pic.pic_fields.bits;
        f.chroma_format_idc = uint32_t(s.chroma);
        f.separate_colour_plane_flag = s.separate;
        f.pcm_enabled_flag = s.pcm;
        f.scaling_list_enabled_flag = s.scaling;
        f.transform_skip_enabled_flag = p->transformSkip;
        f.amp_enabled_flag = s.amp;
        f.strong_intra_smoothing_enabled_flag = s.strong;
        f.sign_data_hiding_enabled_flag = p->signHiding;
        f.constrained_intra_pred_flag = p->constrainedIntra;
        f.cu_qp_delta_enabled_flag = p->cuQpDelta;
        f.weighted_pred_flag = p->weighted;
        f.weighted_bipred_flag = p->biWeighted;
        f.transquant_bypass_enabled_flag = p->bypass;
        f.tiles_enabled_flag = p->tiles;
        f.entropy_coding_sync_enabled_flag = p->wpp;
        f.pps_loop_filter_across_slices_enabled_flag = p->slicesFilter;
        f.loop_filter_across_tiles_enabled_flag = p->tilesFilter;
        f.pcm_loop_filter_disabled_flag = s.pcmNoFilter;
        pic.sps_max_dec_pic_buffering_minus1 = uint8_t(s.maxDec - 1);
        pic.bit_depth_luma_minus8 = uint8_t(s.bits - 8);
        pic.bit_depth_chroma_minus8 = uint8_t(s.bits - 8);
        pic.pcm_sample_bit_depth_luma_minus1 = uint8_t(s.pcmBits - 1);
        pic.pcm_sample_bit_depth_chroma_minus1 = uint8_t(s.pcmBitsChroma - 1);
        pic.log2_min_luma_coding_block_size_minus3 = uint8_t(s.log2MinCb - 3);
        pic.log2_diff_max_min_luma_coding_block_size = uint8_t(s.log2DiffCb);
        pic.log2_min_transform_block_size_minus2 = uint8_t(s.log2MinTb - 2);
        pic.log2_diff_max_min_transform_block_size = uint8_t(s.log2DiffTb);
        pic.log2_min_pcm_luma_coding_block_size_minus3 = uint8_t(s.pcmLog2Min - 3);
        pic.log2_diff_max_min_pcm_luma_coding_block_size = uint8_t(s.pcmLog2Max - s.pcmLog2Min);
        pic.max_transform_hierarchy_depth_intra = uint8_t(s.depthIntra);
        pic.max_transform_hierarchy_depth_inter = uint8_t(s.depthInter);
        pic.init_qp_minus26 = int8_t(p->initQp);
        pic.diff_cu_qp_delta_depth = uint8_t(p->cuQpDepth);
        pic.pps_cb_qp_offset = int8_t(p->cbOffset);
        pic.pps_cr_qp_offset = int8_t(p->crOffset);
        pic.log2_parallel_merge_level_minus2 = uint8_t(p->mergeLevel - 2);
        if (p->tiles) {
            pic.num_tile_columns_minus1 = uint8_t(p->columns - 1);
            pic.num_tile_rows_minus1 = uint8_t(p->rows - 1);
            for (int i = 0; i < p->columns; ++i) pic.column_width_minus1[i] = uint16_t(p->colWidth[i] - 1);
            for (int i = 0; i < p->rows; ++i) pic.row_height_minus1[i] = uint16_t(p->rowHeight[i] - 1);
        }
        auto& g = pic.slice_parsing_fields.bits;
        g.lists_modification_present_flag = p->listsMod;
        g.long_term_ref_pics_present_flag = s.longTerm;
        g.sps_temporal_mvp_enabled_flag = s.tmvp;
        g.cabac_init_present_flag = p->cabacInit;
        g.output_flag_present_flag = p->outputFlag;
        g.dependent_slice_segments_enabled_flag = p->dependent;
        g.pps_slice_chroma_qp_offsets_present_flag = p->sliceChroma;
        g.sample_adaptive_offset_enabled_flag = s.sao;
        g.deblocking_filter_override_enabled_flag = p->dbOverride;
        g.pps_disable_deblocking_filter_flag = p->dbDisable;
        g.slice_segment_header_extension_present_flag = p->headerExt;
        g.RapPicFlag = irap;
        g.IdrPicFlag = type == 19 || type == 20;
        g.IntraPicFlag = irap;
        pic.log2_max_pic_order_cnt_lsb_minus4 = uint8_t(s.pocBits - 4);
        pic.num_short_term_ref_pic_sets = uint8_t(s.rpsCount);
        pic.num_long_term_ref_pic_sps = uint8_t(s.longCount);
        pic.num_ref_idx_l0_default_active_minus1 = uint8_t(p->l0 - 1);
        pic.num_ref_idx_l1_default_active_minus1 = uint8_t(p->l1 - 1);
        pic.pps_beta_offset_div2 = int8_t(p->beta / 2);
        pic.pps_tc_offset_div2 = int8_t(p->tc / 2);
        pic.num_extra_slice_header_bits = uint8_t(p->extraBits);
        pic.st_rps_bits = !sh.rpsFromSps && sh.hasRps ? uint32_t(sh.rpsBits) : 0;

        const Lists* l = p->scaling ? &p->list : s.scaling ? &s.list : nullptr;
        matrix = l != nullptr;
        if (l) {
            for (int i = 0; i < 6; ++i) {
                memcpy(iq.ScalingList4x4[i], l->sl[0][i], 16);
                memcpy(iq.ScalingList8x8[i], l->sl[1][i], 64);
                memcpy(iq.ScalingList16x16[i], l->sl[2][i], 64);
                iq.ScalingListDC16x16[i] = l->dc[0][i];
                if (i >= 2) continue;
                memcpy(iq.ScalingList32x32[i], l->sl[3][i * 3], 64);
                iq.ScalingListDC32x32[i] = l->dc[1][i * 3];
            }
        }
        shown = sh.output;
        started = true;
        return true;
    }

    uint8_t indexOf(const Entry& e) const {
        for (int i = 0; i < 15; ++i) {
            const VAPictureHEVC& f = pic.ReferenceFrames[i];
            if (f.picture_id != VA_INVALID_ID && f.picture_id == e.surface && f.pic_order_cnt == e.poc) return uint8_t(i);
        }
        return 0xFF;
    }

    bool lists() {
        if (sets[kBefore].empty() && sets[kAfter].empty() && sets[kLong].empty()) return false;
        memset(refList, 0xFF, sizeof(refList));
        for (int l = 0; l < (sh.type == 0 ? 2 : 1); ++l) {
            Entry built[16];
            int count = 0;
            const int order[3] = {l ? kAfter : kBefore, l ? kBefore : kAfter, kLong};
            while (count < sh.refs[l]) for (int o : order) for (const Entry& e : sets[o]) if (count < 16) built[count++] = e;
            for (int i = 0; i < sh.refs[l]; ++i) {
                const int index = sh.mod[l] ? sh.entry[l][i] : i;
                if (index >= count) return false;
                refList[l][i] = indexOf(built[index]);
            }
        }
        return true;
    }

    void queue(const uint8_t* nal, size_t length) {
        VASliceParameterBufferHEVC v{};
        v.slice_data_size = uint32_t(length);
        v.slice_data_flag = VA_SLICE_DATA_FLAG_ALL;
        v.slice_data_byte_offset = uint32_t(sh.dataOffset);
        v.slice_segment_address = uint32_t(sh.address);
        v.slice_qp_delta = int8_t(sh.qpDelta);
        v.slice_cb_qp_offset = int8_t(sh.cbOffset);
        v.slice_cr_qp_offset = int8_t(sh.crOffset);
        v.slice_beta_offset_div2 = int8_t(sh.beta / 2);
        v.slice_tc_offset_div2 = int8_t(sh.tc / 2);
        v.collocated_ref_idx = sh.tmvp ? uint8_t(sh.colIdx) : 0xFF;
        v.five_minus_max_num_merge_cand = sh.type == 2 ? 0 : uint8_t(5 - sh.mergeCand);
        v.num_ref_idx_l0_active_minus1 = uint8_t(sh.refs[0] ? sh.refs[0] - 1 : 0);
        v.num_ref_idx_l1_active_minus1 = uint8_t(sh.refs[1] ? sh.refs[1] - 1 : 0);
        auto& f = v.LongSliceFlags.fields;
        f.dependent_slice_segment_flag = sh.dependent;
        f.slice_type = uint32_t(sh.type);
        f.color_plane_id = uint32_t(sh.plane);
        f.mvd_l1_zero_flag = sh.mvdZero;
        f.cabac_init_flag = sh.cabacInit;
        f.slice_temporal_mvp_enabled_flag = sh.tmvp;
        f.slice_deblocking_filter_disabled_flag = sh.dbDisable;
        f.collocated_from_l0_flag = sh.colList == 0;
        f.slice_loop_filter_across_slices_enabled_flag = sh.lfAcross;
        f.slice_sao_luma_flag = sh.saoLuma;
        f.slice_sao_chroma_flag = sh.saoChroma;
        memset(v.RefPicList, 0xFF, sizeof(v.RefPicList));
        if (sh.type != 2) memcpy(v.RefPicList, refList, sizeof(refList));
        const bool weighted = (sh.type == 1 && pps->weighted) || (sh.type == 0 && pps->biWeighted);
        if (weighted) {
            v.luma_log2_weight_denom = uint8_t(sh.lumaDenom);
            if (sps->chroma) v.delta_chroma_log2_weight_denom = int8_t(sh.chromaDenom - sh.lumaDenom);
            for (int i = 0; i < 15 && i < sh.refs[0]; ++i) {
                v.delta_luma_weight_l0[i] = int8_t(sh.lumaWeight[0][i] - (1 << sh.lumaDenom));
                v.luma_offset_l0[i] = int8_t(sh.lumaOffset[0][i]);
                for (int c = 0; c < 2; ++c) {
                    v.delta_chroma_weight_l0[i][c] = int8_t(sh.chromaWeight[0][i][c] - (1 << sh.chromaDenom));
                    v.ChromaOffsetL0[i][c] = int8_t(sh.chromaOffset[0][i][c]);
                }
            }
            for (int i = 0; sh.type == 0 && i < 15 && i < sh.refs[1]; ++i) {
                v.delta_luma_weight_l1[i] = int8_t(sh.lumaWeight[1][i] - (1 << sh.lumaDenom));
                v.luma_offset_l1[i] = int8_t(sh.lumaOffset[1][i]);
                for (int c = 0; c < 2; ++c) {
                    v.delta_chroma_weight_l1[i][c] = int8_t(sh.chromaWeight[1][i][c] - (1 << sh.chromaDenom));
                    v.ChromaOffsetL1[i][c] = int8_t(sh.chromaOffset[1][i][c]);
                }
            }
        }
        slices.push_back(v);
        chunks.emplace_back(nal, length);
    }

    bool show(int64_t stamp, const PictureSink& sink) {
        const Sps& s = *sps;
        const int left = s.crop[0], top = s.crop[2];
        const int w = s.width - left - s.crop[1], h = s.height - top - s.crop[3];
        return session.hand(target, w, h, stamp, color, sink, left, top);
    }

    bool finish(int64_t stamp, const PictureSink& sink) {
        if (!started) return true;
        started = false;
        bool ok = !slices.empty();
        if (ok) {
            slices.back().LongSliceFlags.fields.LastSliceOfPic = 1;
            ok = session.param(VAPictureParameterBufferType, &pic, sizeof(pic)) && (!matrix || session.param(VAIQMatrixBufferType, &iq, sizeof(iq)));
            for (size_t i = 0; ok && i < slices.size(); ++i) ok = session.slice(&slices[i], sizeof(slices[i]), chunks[i].first, chunks[i].second);
            ok = session.submit(target) && ok;
        }
        slices.clear();
        chunks.clear();
        dpb.push_back({target, poc, false, false});
        return ok && (!shown || show(stamp, sink));
    }

    bool slice(const uint8_t* nal, size_t length, int type, int tid, int64_t stamp, const PictureSink& sink) {
        if (length < 3) return false;
        const bool first = nal[2] & 0x80;
        if (skipping && !first) return true;
        std::vector<uint8_t> body = unescape(nal, length);
        BitReader b{body.data(), body.size()};
        b.skip(16);
        const bool parsed = header(b, type, first);
        sliced = false;
        if (!parsed) {
            if (first) {
                finish(stamp, sink);
                skipping = true;
            }
            return false;
        }
        bool ok = true;
        if (first) {
            skipping = (type == 8 || type == 9) && noRasl;
            if (skipping) return true;
            ok = finish(stamp, sink);
            if (!begin(type, tid)) {
                skipping = true;
                return false;
            }
        } else if (!started || type != firstType) {
            return false;
        }
        if (!sh.dependent && sh.type != 2 && !lists()) return false;
        queue(nal, length);
        sliced = true;
        return ok;
    }

    bool decode(const uint8_t* data, size_t size, int64_t stamp, const PictureSink& sink) override {
        lastEos = eos;
        eos = false;
        sliced = false;
        bool any = false, ok = true;
        for (size_t at = 0; at + size_t(lengthSize) <= size;) {
            size_t length = 0;
            for (int k = 0; k < lengthSize; ++k) length = (length << 8) | data[at + size_t(k)];
            at += size_t(lengthSize);
            if (length > size - at) {
                ok = false;
                break;
            }
            const uint8_t* nal = data + at;
            at += length;
            if (length < 2 || (nal[0] & 0x80) || !(nal[1] & 7)) continue;
            const int type = (nal[0] >> 1) & 63;
            if (type == 36 || type == 37) {
                (any ? eos : lastEos) = true;
                continue;
            }
            any = true;
            if ((nal[0] & 1) || (nal[1] >> 3)) continue;
            if (type == 33 || type == 34) param(nal, length);
            else if (type <= 9 || (type >= 16 && type <= 21)) ok = slice(nal, length, type, (nal[1] & 7) - 1, stamp, sink) && ok;
        }
        return finish(stamp, sink) && ok;
    }
};

}

std::unique_ptr<SoftDecoder> openVaapiHevc(const std::vector<uint8_t>& config, std::string* error) {
    auto fail = [&](const char* text) -> std::unique_ptr<SoftDecoder> {
        if (error) *error = text;
        return nullptr;
    };
    const VideoInfo info = videoInfo(fourcc("hvcC"), config);
    if (!info.valid || config.size() < 23) return fail("the HEVC configuration is not readable");
    const bool main = (info.profile >= 1 && info.profile <= 3) || (!info.profile && (info.compatibilityFlags & 0x70000000u));
    if (!main || info.chroma != 1 || (info.bitDepth != 8 && info.bitDepth != 10) || info.bitDepthChroma != info.bitDepth) {
        return fail("only HEVC Main and Main 10 at 4:2:0 are decoded on the GPU");
    }
    auto decoder = std::make_unique<HevcDecoder>();
    decoder->lengthSize = (config[21] & 3) + 1;
    if (info.colorKnown || info.rangeKnown) {
        decoder->color.known = info.colorKnown;
        decoder->color.primaries = info.primaries;
        decoder->color.transfer = info.transfer;
        decoder->color.matrix = info.matrix;
        decoder->color.full = info.full;
    }
    size_t at = 23;
    for (int n = 0; n < config[22] && at + 3 <= config.size(); ++n) {
        const size_t count = (size_t(config[at + 1]) << 8) | config[at + 2];
        at += 3;
        for (size_t i = 0; i < count && at + 2 <= config.size(); ++i) {
            const size_t length = (size_t(config[at]) << 8) | config[at + 1];
            at += 2;
            if (length > config.size() - at) break;
            decoder->param(config.data() + at, length);
            at += length;
        }
    }
    const Sps* sps = nullptr;
    for (const auto& s : decoder->spsList) if (s && !sps) sps = s.get();
    if (!sps || sps->chroma != 1 || sps->bits != info.bitDepth) return fail("the HEVC configuration is not readable");
    const bool deep = sps->bits > 8;
    const VAProfile profile = deep ? VAProfileHEVCMain10 : VAProfileHEVCMain;
    const int count = std::max(17, sps->maxDec + 2);
    if (!vaDecodes(profile) || !decoder->session.open(profile, sps->width, sps->height, deep, count)) return fail("the GPU does not decode this HEVC profile");
    decoder->pool = count;
    return decoder;
}

}
#endif
