#ifdef ABSENT_VAAPI
#include "vaapi.h"
#include "../nal.h"
#include <algorithm>
#include <climits>
#include <cstring>

namespace absent {

namespace {

const uint8_t kScan4[16] = {0, 1, 4, 8, 5, 2, 3, 6, 9, 12, 13, 10, 7, 11, 14, 15};
const uint8_t kScan8[64] = {0, 1, 8, 16, 9, 2, 3, 10, 17, 24, 32, 25, 18, 11, 4, 5, 12, 19, 26, 33, 40, 48, 41, 34, 27, 20, 13, 6, 7, 14, 21, 28,
                            35, 42, 49, 56, 57, 50, 43, 36, 29, 22, 15, 23, 30, 37, 44, 51, 58, 59, 52, 45, 38, 31, 39, 46, 53, 60, 61, 54, 47, 55, 62, 63};
const uint8_t kDefault4[2][16] = {{6, 13, 20, 28, 13, 20, 28, 32, 20, 28, 32, 37, 28, 32, 37, 42}, {10, 14, 20, 24, 14, 20, 24, 27, 20, 24, 27, 30, 24, 27, 30, 34}};
const uint8_t kDefault8[2][64] = {
    {6, 10, 13, 16, 18, 23, 25, 27, 10, 11, 16, 18, 23, 25, 27, 29, 13, 16, 18, 23, 25, 27, 29, 31, 16, 18, 23, 25, 27, 29, 31, 33,
     18, 23, 25, 27, 29, 31, 33, 36, 23, 25, 27, 29, 31, 33, 36, 38, 25, 27, 29, 31, 33, 36, 38, 40, 27, 29, 31, 33, 36, 38, 40, 42},
    {9, 13, 15, 17, 19, 21, 22, 24, 13, 13, 17, 19, 21, 22, 24, 25, 15, 17, 19, 21, 22, 24, 25, 27, 17, 19, 21, 22, 24, 25, 27, 28,
     19, 21, 22, 24, 25, 27, 28, 30, 21, 22, 24, 25, 27, 28, 30, 32, 22, 24, 25, 27, 28, 30, 32, 33, 24, 25, 27, 28, 30, 32, 33, 35}};

const int kFrame = 3;
const int kTop = 1;
const int kBottom = 2;

struct Matrices {
    uint8_t four[6][16];
    uint8_t eight[6][64];
};

bool scalingList(BitReader& b, uint8_t* factors, int size, const uint8_t* preset, const uint8_t* fallback) {
    if (!b.bit()) {
        memcpy(factors, fallback, size_t(size));
        return true;
    }
    const uint8_t* scan = size == 16 ? kScan4 : kScan8;
    int last = 8, next = 8;
    for (int i = 0; i < size; ++i) {
        if (next) {
            const int v = b.se();
            if (v < -128 || v > 127) return false;
            next = (last + v) & 0xFF;
        }
        if (!i && !next) {
            memcpy(factors, preset, size_t(size));
            break;
        }
        last = factors[scan[i]] = uint8_t(next ? next : last);
    }
    return true;
}

bool scalingMatrices(BitReader& b, Matrices& m, const Matrices* sps, bool eight) {
    const uint8_t* fallback[4] = {sps ? sps->four[0] : kDefault4[0], sps ? sps->four[3] : kDefault4[1], sps ? sps->eight[0] : kDefault8[0], sps ? sps->eight[3] : kDefault8[1]};
    bool ok = scalingList(b, m.four[0], 16, kDefault4[0], fallback[0]);
    ok = ok && scalingList(b, m.four[1], 16, kDefault4[0], m.four[0]);
    ok = ok && scalingList(b, m.four[2], 16, kDefault4[0], m.four[1]);
    ok = ok && scalingList(b, m.four[3], 16, kDefault4[1], fallback[1]);
    ok = ok && scalingList(b, m.four[4], 16, kDefault4[1], m.four[3]);
    ok = ok && scalingList(b, m.four[5], 16, kDefault4[1], m.four[4]);
    if (eight) {
        ok = ok && scalingList(b, m.eight[0], 64, kDefault8[0], fallback[2]);
        ok = ok && scalingList(b, m.eight[3], 64, kDefault8[1], fallback[3]);
    }
    return ok;
}

struct Sps {
    bool valid = false;
    int profile = 0;
    int constraints = 0;
    int level = 0;
    int chroma = 1;
    int depth = 8;
    int depthChroma = 8;
    bool matrixPresent = false;
    Matrices matrix;
    int log2FrameNum = 4;
    int pocType = 0;
    int log2PocLsb = 4;
    bool deltaZero = false;
    int offsetNonRef = 0;
    int offsetTopBottom = 0;
    int cycle = 0;
    int offsetRef[256] = {};
    int refFrames = 0;
    bool gaps = false;
    int mbWidth = 0;
    int mbHeight = 0;
    bool frameMbsOnly = true;
    bool mbaff = false;
    bool direct8x8 = false;
    int crop[4] = {};
};

bool parseSps(BitReader& b, Sps& s, int& id) {
    s = Sps();
    s.profile = int(b.bits(8));
    s.constraints = int(b.bits(8));
    s.level = int(b.bits(8));
    id = int(b.ue());
    if (id >= 32) return false;
    memset(&s.matrix, 16, sizeof(s.matrix));
    static const int extended[] = {100, 110, 122, 244, 44, 83, 86, 118, 128, 138, 139, 134, 135};
    if (std::find(std::begin(extended), std::end(extended), s.profile) != std::end(extended)) {
        s.chroma = int(b.ue());
        if (s.chroma > 3) return false;
        if (s.chroma == 3 && b.bit()) return false;
        s.depth = int(b.ue()) + 8;
        s.depthChroma = int(b.ue()) + 8;
        b.bit();
        if (b.bit()) {
            if (!scalingMatrices(b, s.matrix, nullptr, true)) return false;
            s.matrixPresent = true;
        }
    }
    s.log2FrameNum = int(b.ue()) + 4;
    if (s.log2FrameNum > 16) return false;
    s.pocType = int(b.ue());
    if (s.pocType == 0) {
        s.log2PocLsb = int(b.ue()) + 4;
        if (s.log2PocLsb > 16) return false;
    } else if (s.pocType == 1) {
        s.deltaZero = b.bit();
        s.offsetNonRef = b.se();
        s.offsetTopBottom = b.se();
        s.cycle = int(b.ue());
        if (s.cycle > 255) return false;
        for (int i = 0; i < s.cycle; ++i) s.offsetRef[i] = b.se();
    } else if (s.pocType != 2) {
        return false;
    }
    s.refFrames = int(b.ue());
    if (s.refFrames > 16) return false;
    s.gaps = b.bit();
    s.mbWidth = int(b.ue()) + 1;
    const int mapUnits = int(b.ue()) + 1;
    s.frameMbsOnly = b.bit();
    if (!s.frameMbsOnly) s.mbaff = b.bit();
    s.mbHeight = mapUnits * (s.frameMbsOnly ? 1 : 2);
    s.direct8x8 = b.bit();
    if (b.bit()) {
        const int unitY = s.frameMbsOnly ? 2 : 4;
        s.crop[0] = int(b.ue()) * 2;
        s.crop[1] = int(b.ue()) * 2;
        s.crop[2] = int(b.ue()) * unitY;
        s.crop[3] = int(b.ue()) * unitY;
    }
    if (!b.ok || s.mbWidth > 1024 || s.mbHeight > 1024 || s.crop[0] + s.crop[1] >= s.mbWidth * 16 || s.crop[2] + s.crop[3] >= s.mbHeight * 16) return false;
    s.valid = true;
    return true;
}

struct Pps {
    int sps = 0;
    bool cabac = false;
    bool pocPresent = false;
    int refCount[2] = {1, 1};
    bool weighted = false;
    int bipred = 0;
    int qp = 26;
    int qs = 26;
    int chromaOffset[2] = {};
    bool deblockControl = false;
    bool constrainedIntra = false;
    bool redundant = false;
    bool transform8x8 = false;
    Matrices matrix;
};

size_t stopBit(const std::vector<uint8_t>& rbsp) {
    for (size_t i = rbsp.size(); i-- > 0;) {
        if (!rbsp[i]) continue;
        int bit = 0;
        while (!((rbsp[i] >> bit) & 1)) ++bit;
        return i * 8 + size_t(7 - bit);
    }
    return 0;
}

bool parsePps(const std::vector<uint8_t>& rbsp, const Sps* list, Pps& p) {
    BitReader b{rbsp.data(), rbsp.size()};
    b.skip(8);
    p = Pps();
    b.ue();
    p.sps = int(b.ue());
    if (p.sps >= 32 || !list[p.sps].valid) return false;
    const Sps& s = list[p.sps];
    p.cabac = b.bit();
    p.pocPresent = b.bit();
    if (b.ue() != 0) return false;
    p.refCount[0] = int(b.ue()) + 1;
    p.refCount[1] = int(b.ue()) + 1;
    if (p.refCount[0] > 32 || p.refCount[1] > 32) return false;
    p.weighted = b.bit();
    p.bipred = int(b.bits(2));
    p.qp = b.se() + 26;
    p.qs = b.se() + 26;
    p.chromaOffset[0] = p.chromaOffset[1] = b.se();
    p.deblockControl = b.bit();
    p.constrainedIntra = b.bit();
    p.redundant = b.bit();
    p.matrix = s.matrix;
    const bool restricted = (s.profile == 66 || s.profile == 77 || s.profile == 88) && (s.constraints & 0xE0);
    if (b.at < stopBit(rbsp) && !restricted) {
        p.transform8x8 = b.bit();
        if (b.bit() && !scalingMatrices(b, p.matrix, s.matrixPresent ? &s.matrix : nullptr, p.transform8x8)) return false;
        p.chromaOffset[1] = b.se();
    }
    return b.ok;
}

struct Mmco {
    int op = 0;
    int shortNum = 0;
    int longArg = 0;
};

struct Slice {
    int type = 0;
    int raw = 0;
    int firstMb = 0;
    int ppsId = 0;
    int frameNum = 0;
    int structure = kFrame;
    int currPicNum = 0;
    int maxPicNum = 0;
    int idrId = 0;
    int pocLsb = 0;
    int deltaBottom = 0;
    int deltaPoc[2] = {};
    int redundant = 0;
    bool directSpatial = false;
    int listCount = 0;
    int refCount[2] = {};
    int modCount[2] = {};
    int modOp[2][32] = {};
    unsigned modVal[2][32] = {};
    int lumaDenom = 0;
    int chromaDenom = 0;
    bool lumaFlag[2] = {};
    bool chromaFlag[2] = {};
    short lumaWeight[2][32][2] = {};
    short chromaWeight[2][32][2][2] = {};
    bool explicitMarking = false;
    int mmcoCount = 0;
    Mmco mmco[66];
    int cabacInit = 0;
    int qpDelta = 0;
    int deblock = 0;
    int alpha = 0;
    int beta = 0;
    size_t bitOffset = 0;
};

struct Frame {
    VASurfaceID surface = VA_INVALID_SURFACE;
    int frameNum = 0;
    int poc = 0;
    int fieldPoc[2] = {INT_MAX, INT_MAX};
    int reference = 0;
    bool longRef = false;
    int picId = 0;
    bool invalidGap = false;
};

struct Ref {
    int frame = -1;
    int reference = 0;
    int poc = 0;
    int picId = 0;
};

struct H264Decoder : SoftDecoder {
    VaSession session{&wanted};
    int lengthSize = 4;
    Color color;
    Sps sps[32];
    std::vector<uint8_t> ppsRaw[256];
    Sps active;
    Pps pps;
    Frame frames[40];
    std::vector<int> shortRefs;
    int longRefs[32];
    int longCount = 0;
    Ref defaults[2];
    Ref lists[2][32];
    int cur = -1;
    bool firstField = false;
    bool open = false;
    bool usable = false;
    int structure = kFrame;
    bool droppable = false;
    int frameNum = 0;
    int prevFrameNum = -1;
    int frameNumOffset = 0;
    int prevFrameNumOffset = 0;
    int pocLsb = 0;
    int pocMsb = 0;
    int prevPocLsb = -1;
    int prevPocMsb = 1 << 16;
    int deltaBottom = 0;
    int deltaPoc[2] = {};
    std::vector<Mmco> mmco;
    bool explicitMarking = false;
    int pending = -1;
    int64_t pendingStamp = 0;
    int64_t stamp = 0;
    bool idrCleared = false;
    bool skipping = false;
    std::vector<uint8_t> scratch;

    H264Decoder() {
        for (int& r : longRefs) r = -1;
    }
    const char* name() const override { return "vaapi"; }
    bool weavesFields() const override { return true; }

    void reset() override {
        removeAll();
        prevFrameNum = -1;
        prevFrameNumOffset = 0;
        prevPocMsb = 1 << 16;
        prevPocLsb = -1;
        cur = -1;
        firstField = false;
        open = false;
        skipping = false;
        pending = -1;
    }

    std::vector<VASurfaceID> busy() const {
        std::vector<VASurfaceID> out;
        for (int i : shortRefs) out.push_back(frames[i].surface);
        for (int i : longRefs) if (i >= 0) out.push_back(frames[i].surface);
        if (cur >= 0) out.push_back(frames[cur].surface);
        if (pending >= 0) out.push_back(frames[pending].surface);
        return out;
    }

    int newFrame() {
        for (int i = 0; i < 40; ++i) {
            bool used = i == cur || i == pending;
            for (int s : shortRefs) used = used || s == i;
            for (int l : longRefs) used = used || l == i;
            if (used) continue;
            frames[i] = Frame();
            frames[i].surface = session.spare(busy());
            return frames[i].surface == VA_INVALID_SURFACE ? -1 : i;
        }
        return -1;
    }

    bool unreference(int f, int mask) {
        frames[f].reference &= mask;
        return frames[f].reference == 0;
    }

    int findShort(int num, int& at) {
        for (size_t i = 0; i < shortRefs.size(); ++i) {
            if (frames[shortRefs[i]].frameNum == num) {
                at = int(i);
                return shortRefs[i];
            }
        }
        return -1;
    }

    int removeShort(int num, int mask) {
        int at = 0;
        const int f = findShort(num, at);
        if (f >= 0 && unreference(f, mask)) shortRefs.erase(shortRefs.begin() + at);
        return f;
    }

    void removeLong(int i, int mask) {
        const int f = longRefs[i];
        if (f < 0 || !unreference(f, mask)) return;
        frames[f].longRef = false;
        longRefs[i] = -1;
        --longCount;
    }

    void removeAll() {
        for (int i = 0; i < 16; ++i) removeLong(i, 0);
        for (int f : shortRefs) frames[f].reference = 0;
        shortRefs.clear();
        defaults[0] = defaults[1] = Ref();
    }

    void idr() {
        removeAll();
        prevFrameNum = prevFrameNumOffset = 0;
        prevPocMsb = 1 << 16;
        prevPocLsb = -1;
    }

    int extract(int num, int& which) const {
        which = structure;
        if (structure != kFrame) {
            if (!(num & 1)) which ^= kFrame;
            num >>= 1;
        }
        return num;
    }

    void marking() {
        if (!explicitMarking) {
            mmco.clear();
            if (!shortRefs.empty() && longCount + int(shortRefs.size()) >= active.refFrames && !(structure != kFrame && !firstField && frames[cur].reference)) {
                Mmco m;
                m.op = 1;
                m.shortNum = frames[shortRefs.back()].frameNum;
                if (structure != kFrame) {
                    m.shortNum *= 2;
                    mmco.push_back(m);
                    m.shortNum += 1;
                }
                mmco.push_back(m);
            }
        }
        bool assigned = false;
        for (const Mmco& m : mmco) {
            int which = 0, num = 0, at = 0, f = -1;
            if (m.op == 1 || m.op == 3) {
                num = extract(m.shortNum, which);
                f = findShort(num, at);
                if (f < 0) continue;
            }
            if (m.op == 1) {
                removeShort(num, which ^ kFrame);
            } else if (m.op == 3) {
                if (longRefs[m.longArg] != f) removeLong(m.longArg, 0);
                shortRefs.erase(shortRefs.begin() + at);
                longRefs[m.longArg] = f;
                frames[f].longRef = true;
                ++longCount;
            } else if (m.op == 2) {
                const int j = extract(m.longArg, which);
                if (j < 32 && longRefs[j] >= 0) removeLong(j, which ^ kFrame);
            } else if (m.op == 6) {
                if (!shortRefs.empty() && shortRefs.front() == cur) shortRefs.erase(shortRefs.begin());
                if (frames[cur].longRef) for (int j = 0; j < 32; ++j) if (longRefs[j] == cur) removeLong(j, 0);
                if (longRefs[m.longArg] != cur) {
                    removeLong(m.longArg, 0);
                    longRefs[m.longArg] = cur;
                    frames[cur].longRef = true;
                    ++longCount;
                }
                frames[cur].reference |= structure;
                assigned = true;
            } else if (m.op == 4) {
                for (int j = m.longArg; j < 16; ++j) removeLong(j, 0);
            } else if (m.op == 5) {
                while (!shortRefs.empty()) removeShort(frames[shortRefs.front()].frameNum, 0);
                for (int j = 0; j < 16; ++j) removeLong(j, 0);
                frameNum = frames[cur].frameNum = 0;
            }
        }
        if (!assigned) {
            if (!shortRefs.empty() && shortRefs.front() == cur) {
                frames[cur].reference |= structure;
            } else if (!frames[cur].longRef) {
                removeShort(frames[cur].frameNum, 0);
                shortRefs.insert(shortRefs.begin(), cur);
                frames[cur].reference |= structure;
            }
        }
        if (longCount + int(shortRefs.size()) > std::max(active.refFrames, 1)) {
            if (longCount && shortRefs.empty()) {
                for (int i = 0; i < 16; ++i) {
                    if (longRefs[i] >= 0) {
                        removeLong(i, 0);
                        break;
                    }
                }
            } else {
                removeShort(frames[shortRefs.back()].frameNum, 0);
            }
        }
        for (size_t i = 0; i < shortRefs.size(); ++i) {
            const Frame& f = frames[shortRefs[i]];
            const int d = (frames[cur].frameNum - f.frameNum) & ((1 << active.log2FrameNum) - 1);
            if (f.invalidGap && d > active.refFrames) removeShort(f.frameNum, 0);
        }
    }

    void initPoc() {
        const int maxFrameNum = 1 << active.log2FrameNum;
        frameNumOffset = prevFrameNumOffset;
        if (frameNum < prevFrameNum) frameNumOffset += maxFrameNum;
        int64_t field[2];
        const bool reference = !droppable;
        if (active.pocType == 0) {
            const int maxLsb = 1 << active.log2PocLsb;
            if (prevPocLsb < 0) prevPocLsb = pocLsb;
            if (pocLsb < prevPocLsb && prevPocLsb - pocLsb >= maxLsb / 2) pocMsb = prevPocMsb + maxLsb;
            else if (pocLsb > prevPocLsb && prevPocLsb - pocLsb < -maxLsb / 2) pocMsb = prevPocMsb - maxLsb;
            else pocMsb = prevPocMsb;
            field[0] = field[1] = int64_t(pocMsb) + pocLsb;
            if (structure == kFrame) field[1] += deltaBottom;
        } else if (active.pocType == 1) {
            int absolute = active.cycle ? frameNumOffset + frameNum : 0;
            if (!reference && absolute > 0) --absolute;
            int64_t perCycle = 0;
            for (int i = 0; i < active.cycle; ++i) perCycle += active.offsetRef[i];
            int64_t expected = 0;
            if (absolute > 0) {
                const int count = (absolute - 1) / active.cycle, in = (absolute - 1) % active.cycle;
                expected = int64_t(count) * perCycle;
                for (int i = 0; i <= in; ++i) expected += active.offsetRef[i];
            }
            if (!reference) expected += active.offsetNonRef;
            field[0] = expected + deltaPoc[0];
            field[1] = field[0] + active.offsetTopBottom;
            if (structure == kFrame) field[1] += deltaPoc[1];
        } else {
            int64_t poc = 2 * (int64_t(frameNumOffset) + frameNum);
            if (!reference) --poc;
            field[0] = field[1] = poc;
        }
        Frame& f = frames[cur];
        if (structure != kBottom) f.fieldPoc[0] = int(field[0]);
        if (structure != kTop) f.fieldPoc[1] = int(field[1]);
        f.poc = std::min(f.fieldPoc[0], f.fieldPoc[1]);
    }

    void vaPicture(VAPictureH264& v, int f, int which) const {
        const Frame& p = frames[f];
        which = (which ? which : p.reference) & kFrame;
        v.picture_id = p.surface;
        v.frame_idx = uint32_t(p.longRef ? p.picId : p.frameNum);
        v.flags = 0;
        if (which != kFrame) v.flags |= (which & kTop) ? VA_PICTURE_H264_TOP_FIELD : VA_PICTURE_H264_BOTTOM_FIELD;
        if (p.reference) v.flags |= p.longRef ? VA_PICTURE_H264_LONG_TERM_REFERENCE : VA_PICTURE_H264_SHORT_TERM_REFERENCE;
        v.TopFieldOrderCnt = p.fieldPoc[0] != INT_MAX ? p.fieldPoc[0] : 0;
        v.BottomFieldOrderCnt = p.fieldPoc[1] != INT_MAX ? p.fieldPoc[1] : 0;
    }

    static void empty(VAPictureH264& v) {
        v.picture_id = VA_INVALID_SURFACE;
        v.flags = VA_PICTURE_H264_INVALID;
        v.TopFieldOrderCnt = v.BottomFieldOrderCnt = 0;
        v.frame_idx = 0;
    }

    void dpbAdd(VAPictureParameterBufferH264& pic, int& size, int f) const {
        const Frame& p = frames[f];
        const uint32_t idx = uint32_t(p.longRef ? p.picId : p.frameNum);
        bool merged = false;
        for (int i = 0; i < size; ++i) {
            VAPictureH264& v = pic.ReferenceFrames[i];
            if (v.picture_id == p.surface && bool(v.flags & VA_PICTURE_H264_LONG_TERM_REFERENCE) == p.longRef && v.frame_idx == idx) {
                VAPictureH264 t;
                vaPicture(t, f, 0);
                const uint32_t fields = VA_PICTURE_H264_TOP_FIELD | VA_PICTURE_H264_BOTTOM_FIELD;
                if ((t.flags ^ v.flags) & fields) {
                    v.flags |= t.flags & fields;
                    if (t.flags & VA_PICTURE_H264_TOP_FIELD) v.TopFieldOrderCnt = t.TopFieldOrderCnt;
                    else v.BottomFieldOrderCnt = t.BottomFieldOrderCnt;
                    merged = true;
                }
            }
        }
        if (!merged && size < 16) vaPicture(pic.ReferenceFrames[size++], f, 0);
    }

    int sortedAdd(std::vector<int>& out, int limit, int dir) const {
        int count = 0;
        for (;;) {
            int best = dir ? INT_MIN : INT_MAX, pick = -1;
            for (int f : shortRefs) {
                const int poc = frames[f].poc;
                if (((poc > limit) ^ bool(dir)) && ((poc < best) ^ bool(dir))) {
                    best = poc;
                    pick = f;
                }
            }
            if (pick < 0) break;
            out.push_back(pick);
            ++count;
            limit = frames[pick].poc - dir;
        }
        return count;
    }

    void fieldCopy(Ref& r, int f, int parity, int add) const {
        r.frame = f;
        r.reference = frames[f].reference;
        r.poc = frames[f].poc;
        r.picId = frames[f].picId;
        if (parity != kFrame) {
            r.reference = parity;
            r.poc = frames[f].fieldPoc[parity == kBottom];
            r.picId = r.picId * 2 + add;
        }
    }

    int defaultList(Ref* out, int room, const std::vector<int>& in, bool isLong) {
        int i[2] = {0, 0}, index = 0;
        const int len = int(in.size()), sel = structure;
        while ((i[0] < len || i[1] < len) && index < room) {
            while (i[0] < len && !(in[i[0]] >= 0 && (frames[in[i[0]]].reference & sel))) ++i[0];
            while (i[1] < len && !(in[i[1]] >= 0 && (frames[in[i[1]]].reference & (sel ^ 3)))) ++i[1];
            if (i[0] < len && index < room) {
                Frame& f = frames[in[i[0]]];
                f.picId = isLong ? i[0] : f.frameNum;
                fieldCopy(out[index++], in[i[0]++], sel, 1);
            }
            if (i[1] < len && index < room) {
                Frame& f = frames[in[i[1]]];
                f.picId = isLong ? i[1] : f.frameNum;
                fieldCopy(out[index++], in[i[1]++], sel ^ 3, 0);
            }
        }
        return index;
    }

    bool buildLists(const Slice& s) {
        for (auto& list : lists) for (Ref& r : list) r = Ref();
        const std::vector<int> longs(longRefs, longRefs + 16);
        if (s.type == 1) {
            const int curPoc = structure != kFrame ? frames[cur].fieldPoc[structure == kBottom] : frames[cur].poc;
            int lens[2];
            for (int list = 0; list < 2; ++list) {
                std::vector<int> sorted;
                sortedAdd(sorted, curPoc, 1 ^ list);
                sortedAdd(sorted, curPoc, 0 ^ list);
                int len = defaultList(lists[list], 32, sorted, false);
                len += defaultList(lists[list] + len, 32 - len, longs, true);
                lens[list] = len;
            }
            if (lens[0] == lens[1] && lens[1] > 1) {
                int i = 0;
                while (i < lens[0] && frames[lists[0][i].frame].surface == frames[lists[1][i].frame].surface) ++i;
                if (i == lens[0]) std::swap(lists[1][0], lists[1][1]);
            }
        } else {
            int len = defaultList(lists[0], 32, shortRefs, false);
            defaultList(lists[0] + len, 32 - len, longs, true);
        }
        for (int i = 0; i < s.listCount; ++i) defaults[i] = lists[i][0];
        for (int list = 0; list < s.listCount; ++list) {
            int pred = s.currPicNum;
            for (int index = 0; index < s.modCount[list]; ++index) {
                const int op = s.modOp[list][index];
                const unsigned val = s.modVal[list][index];
                int which = 0, f = -1, picId = 0;
                if (op < 2) {
                    const unsigned diff = val + 1;
                    if (diff > unsigned(s.maxPicNum)) return false;
                    pred = op == 0 ? pred - int(diff) : pred + int(diff);
                    pred &= s.maxPicNum - 1;
                    const int num = extract(pred, which);
                    for (int i = int(shortRefs.size()) - 1; i >= 0; --i) {
                        const int g = shortRefs[size_t(i)];
                        if (frames[g].frameNum == num && (frames[g].reference & which)) {
                            f = g;
                            break;
                        }
                    }
                    picId = pred;
                } else {
                    picId = int(val);
                    const int index2 = extract(picId, which);
                    if (index2 > 31) return false;
                    const int g = longRefs[index2];
                    if (g >= 0 && (frames[g].reference & which)) f = g;
                }
                if (f < 0) {
                    lists[list][index] = Ref();
                    continue;
                }
                int i = index;
                for (; i + 1 < s.refCount[list]; ++i) {
                    const Ref& r = lists[list][i];
                    if (r.frame >= 0 && frames[f].longRef == frames[r.frame].longRef && picId == r.picId) break;
                }
                for (; i > index; --i) lists[list][i] = lists[list][i - 1];
                Ref& r = lists[list][index];
                r.frame = f;
                r.reference = frames[f].reference;
                r.poc = frames[f].poc;
                r.picId = frames[f].picId;
                if (structure != kFrame) {
                    r.reference = which;
                    r.poc = frames[f].fieldPoc[which == kBottom];
                }
            }
        }
        for (int list = 0; list < s.listCount; ++list) {
            for (int index = 0; index < s.refCount[list]; ++index) {
                Ref& r = lists[list][index];
                if (r.frame < 0 || (structure == kFrame && (r.reference & 3) != 3)) {
                    const Ref& d = defaults[list];
                    if (d.frame >= 0 && !(structure == kFrame && (d.reference & 3) != 3)) r = d;
                    else return false;
                }
            }
        }
        return true;
    }

    bool parseSlice(BitReader& b, int nalType, int refIdc, Slice& s) {
        b.skip(8);
        s.firstMb = int(b.ue());
        s.raw = int(b.ue());
        if (s.raw > 9) return false;
        s.type = s.raw % 5;
        const int nos = s.type == 3 ? 0 : s.type == 4 ? 2 : s.type;
        if (nalType == 5 && nos != 2) return false;
        s.ppsId = int(b.ue());
        if (s.ppsId > 255 || ppsRaw[s.ppsId].empty()) return false;
        Pps p;
        if (!parsePps(ppsRaw[s.ppsId], sps, p)) return false;
        const Sps& q = sps[p.sps];
        s.frameNum = int(b.bits(q.log2FrameNum));
        s.structure = kFrame;
        if (!q.frameMbsOnly && b.bit()) s.structure = kTop + int(b.bit());
        if (s.structure == kFrame) {
            s.currPicNum = s.frameNum;
            s.maxPicNum = 1 << q.log2FrameNum;
        } else {
            s.currPicNum = 2 * s.frameNum + 1;
            s.maxPicNum = 1 << (q.log2FrameNum + 1);
        }
        if (nalType == 5) s.idrId = int(b.ue());
        if (q.pocType == 0) {
            s.pocLsb = int(b.bits(q.log2PocLsb));
            if (p.pocPresent && s.structure == kFrame) s.deltaBottom = b.se();
        }
        if (q.pocType == 1 && !q.deltaZero) {
            s.deltaPoc[0] = b.se();
            if (p.pocPresent && s.structure == kFrame) s.deltaPoc[1] = b.se();
        }
        if (p.redundant) s.redundant = int(b.ue());
        if (nos == 1) s.directSpatial = b.bit();
        s.refCount[0] = p.refCount[0];
        s.refCount[1] = p.refCount[1];
        if (nos != 2) {
            const int top = s.structure == kFrame ? 16 : 32;
            if (b.bit()) {
                s.refCount[0] = int(b.ue()) + 1;
                s.refCount[1] = nos == 1 ? int(b.ue()) + 1 : 1;
            }
            s.listCount = nos == 1 ? 2 : 1;
            if (s.refCount[0] > top || (s.listCount == 2 && s.refCount[1] > top)) return false;
            if (s.refCount[1] > top) s.refCount[1] = 0;
            for (int list = 0; list < s.listCount; ++list) {
                if (!b.bit()) continue;
                for (int index = 0;; ++index) {
                    const unsigned op = b.ue();
                    if (op == 3) break;
                    if (index >= s.refCount[list] || op > 2 || !b.ok) return false;
                    s.modVal[list][index] = b.ue();
                    s.modOp[list][index] = int(op);
                    ++s.modCount[list];
                }
            }
        } else {
            s.refCount[0] = s.refCount[1] = 0;
        }
        if ((p.weighted && nos == 0) || (p.bipred == 1 && nos == 1)) {
            s.lumaDenom = int(b.ue());
            if (s.lumaDenom > 7) s.lumaDenom = 0;
            s.chromaDenom = int(b.ue());
            if (s.chromaDenom > 7) s.chromaDenom = 0;
            const int lumaDefault = 1 << s.lumaDenom, chromaDefault = 1 << s.chromaDenom;
            for (int list = 0; list < 2; ++list) {
                for (int i = 0; i < s.refCount[list]; ++i) {
                    short* luma = s.lumaWeight[list][i];
                    luma[0] = short(lumaDefault);
                    luma[1] = 0;
                    if (b.bit()) {
                        const int w = b.se(), o = b.se();
                        if (w < -128 || w > 127 || o < -128 || o > 127) return false;
                        luma[0] = short(w);
                        luma[1] = short(o);
                        if (w != lumaDefault || o != 0) s.lumaFlag[list] = true;
                    }
                    const bool chroma = b.bit();
                    for (int j = 0; j < 2; ++j) {
                        short* c = s.chromaWeight[list][i][j];
                        c[0] = short(chromaDefault);
                        c[1] = 0;
                        if (!chroma) continue;
                        const int w = b.se(), o = b.se();
                        if (w < -128 || w > 127 || o < -128 || o > 127) return false;
                        c[0] = short(w);
                        c[1] = short(o);
                        if (w != chromaDefault || o != 0) s.chromaFlag[list] = true;
                    }
                }
                if (nos != 1) break;
            }
        }
        if (refIdc) {
            if (nalType == 5) {
                b.bit();
                if (b.bit()) {
                    s.mmco[0].op = 6;
                    s.mmco[0].longArg = 0;
                    s.mmcoCount = 1;
                }
                s.explicitMarking = true;
            } else if ((s.explicitMarking = b.bit())) {
                int i = 0;
                for (; i < 66; ++i) {
                    const unsigned op = b.ue();
                    if (op > 6 || !b.ok) return false;
                    Mmco& m = s.mmco[i];
                    m.op = int(op);
                    if (op == 1 || op == 3) m.shortNum = int((unsigned(s.currPicNum) - b.ue() - 1) & unsigned(s.maxPicNum - 1));
                    if (op == 2 || op == 3 || op == 6 || op == 4) {
                        const unsigned arg = b.ue();
                        if (arg >= 32 || (arg >= 16 && !(op == 4 && arg == 16) && !(op == 2 && s.structure != kFrame))) return false;
                        m.longArg = int(arg);
                    }
                    if (op == 0) break;
                }
                s.mmcoCount = i;
            }
        }
        if (nos != 2 && p.cabac) {
            s.cabacInit = int(b.ue());
            if (s.cabacInit > 2) return false;
        }
        s.qpDelta = b.se();
        if (s.type == 3) b.bit();
        if (s.type == 3 || s.type == 4) b.se();
        if (p.deblockControl) {
            s.deblock = int(b.ue());
            if (s.deblock > 2) return false;
            if (s.deblock != 1) {
                s.alpha = b.se();
                s.beta = b.se();
            }
        }
        s.bitOffset = b.at;
        return b.ok;
    }

    bool frameStart() {
        cur = newFrame();
        if (cur < 0) return false;
        Frame& f = frames[cur];
        f.reference = droppable ? 0 : structure;
        f.frameNum = frameNum;
        return true;
    }

    bool fieldStart(const Slice& s, int nalType, int refIdc, const PictureSink& sink) {
        const Pps& p = pps;
        const Sps& q = sps[p.sps];
        const int width = q.mbWidth * 16, height = q.mbHeight * 16;
        if (q.chroma != 1 || q.depth != 8 || q.depthChroma != 8) return false;
        if (!session.ready() || width != session.width() || height != session.height()) {
            if (nalType != 5) return false;
            reset();
            const int count = 18;
            if (!session.open(VAProfileH264High, width, height, false, count) && !session.open(VAProfileH264Main, width, height, false, count)) return false;
        }
        active = q;
        const int lastStructure = structure;
        droppable = refIdc == 0;
        structure = s.structure;
        frameNum = s.frameNum;
        pocLsb = s.pocLsb;
        deltaBottom = s.deltaBottom;
        deltaPoc[0] = s.deltaPoc[0];
        deltaPoc[1] = s.deltaPoc[1];
        const int maxFrameNum = 1 << q.log2FrameNum;
        if (frameNum != prevFrameNum) {
            int unwrapped = prevFrameNum;
            if (unwrapped > frameNum) unwrapped -= maxFrameNum;
            if (frameNum - unwrapped > q.refFrames) {
                unwrapped = frameNum - q.refFrames - 1;
                if (unwrapped < 0) unwrapped += maxFrameNum;
                prevFrameNum = unwrapped;
            }
        }
        while (frameNum != prevFrameNum && !firstField && frameNum != (prevFrameNum + 1) % maxFrameNum) {
            const int prev = shortRefs.empty() ? -1 : shortRefs.front();
            if (!frameStart()) return false;
            prevFrameNum = (prevFrameNum + 1) % maxFrameNum;
            frames[cur].frameNum = prevFrameNum;
            frames[cur].invalidGap = !q.gaps;
            explicitMarking = false;
            marking();
            if (!shortRefs.empty()) {
                Frame& g = frames[shortRefs.front()];
                if (prev >= 0) {
                    g.surface = frames[prev].surface;
                    g.poc = frames[prev].poc + 2;
                }
                g.frameNum = prevFrameNum;
            }
            cur = -1;
        }
        if (firstField) {
            if (structure == kFrame || structure == lastStructure || frames[cur].frameNum != frameNum) {
                if (pending >= 0) flushPending(sink);
                cur = -1;
                firstField = structure != kFrame;
            } else {
                firstField = false;
            }
        } else {
            firstField = structure != kFrame;
        }
        if (structure == kFrame || firstField) {
            if (pending >= 0) flushPending(sink);
            cur = -1;
            if (!frameStart()) return false;
        }
        initPoc();
        mmco.assign(s.mmco, s.mmco + s.mmcoCount);
        explicitMarking = s.explicitMarking;

        VAPictureParameterBufferH264 pic{};
        pic.picture_width_in_mbs_minus1 = uint16_t(q.mbWidth - 1);
        pic.picture_height_in_mbs_minus1 = uint16_t(q.mbHeight - 1);
        pic.bit_depth_luma_minus8 = uint8_t(q.depth - 8);
        pic.bit_depth_chroma_minus8 = uint8_t(q.depthChroma - 8);
        pic.num_ref_frames = uint8_t(q.refFrames);
        auto& sf = pic.seq_fields.bits;
        sf.chroma_format_idc = uint32_t(q.chroma);
        sf.gaps_in_frame_num_value_allowed_flag = q.gaps;
        sf.frame_mbs_only_flag = q.frameMbsOnly;
        sf.mb_adaptive_frame_field_flag = q.mbaff;
        sf.direct_8x8_inference_flag = q.direct8x8;
        sf.MinLumaBiPredSize8x8 = q.level >= 31;
        sf.log2_max_frame_num_minus4 = uint32_t(q.log2FrameNum - 4);
        sf.pic_order_cnt_type = uint32_t(q.pocType);
        sf.log2_max_pic_order_cnt_lsb_minus4 = uint32_t(q.log2PocLsb - 4);
        sf.delta_pic_order_always_zero_flag = q.deltaZero;
        pic.pic_init_qp_minus26 = int8_t(p.qp - 26);
        pic.pic_init_qs_minus26 = int8_t(p.qs - 26);
        pic.chroma_qp_index_offset = int8_t(p.chromaOffset[0]);
        pic.second_chroma_qp_index_offset = int8_t(p.chromaOffset[1]);
        auto& pf = pic.pic_fields.bits;
        pf.entropy_coding_mode_flag = p.cabac;
        pf.weighted_pred_flag = p.weighted;
        pf.weighted_bipred_idc = uint32_t(p.bipred);
        pf.transform_8x8_mode_flag = p.transform8x8;
        pf.field_pic_flag = structure != kFrame;
        pf.constrained_intra_pred_flag = p.constrainedIntra;
        pf.pic_order_present_flag = p.pocPresent;
        pf.deblocking_filter_control_present_flag = p.deblockControl;
        pf.redundant_pic_cnt_present_flag = p.redundant;
        pf.reference_pic_flag = refIdc != 0;
        pic.frame_num = uint16_t(frameNum);
        vaPicture(pic.CurrPic, cur, structure);
        for (VAPictureH264& v : pic.ReferenceFrames) empty(v);
        int size = 0;
        for (int f : shortRefs) if (frames[f].reference) dpbAdd(pic, size, f);
        for (int i = 0; i < 16; ++i) if (longRefs[i] >= 0 && frames[longRefs[i]].reference) dpbAdd(pic, size, longRefs[i]);
        VAIQMatrixBufferH264 iq{};
        memcpy(iq.ScalingList4x4, p.matrix.four, sizeof(iq.ScalingList4x4));
        memcpy(iq.ScalingList8x8[0], p.matrix.eight[0], 64);
        memcpy(iq.ScalingList8x8[1], p.matrix.eight[3], 64);
        open = session.param(VAPictureParameterBufferType, &pic, sizeof(pic)) && session.param(VAIQMatrixBufferType, &iq, sizeof(iq));
        usable = open;
        return open;
    }

    void addSlice(const Slice& s, const uint8_t* nal, size_t size) {
        if (s.type != 2 && s.type != 4 && !buildLists(s)) return;
        VASliceParameterBufferH264 v{};
        v.slice_data_size = uint32_t(size);
        v.slice_data_flag = VA_SLICE_DATA_FLAG_ALL;
        v.slice_data_bit_offset = uint16_t(s.bitOffset);
        v.first_mb_in_slice = uint16_t(s.firstMb);
        v.slice_type = uint8_t(s.type);
        v.direct_spatial_mv_pred_flag = s.type == 1 ? s.directSpatial : 0;
        v.num_ref_idx_l0_active_minus1 = uint8_t(s.listCount > 0 ? s.refCount[0] - 1 : 0);
        v.num_ref_idx_l1_active_minus1 = uint8_t(s.listCount > 1 ? s.refCount[1] - 1 : 0);
        v.cabac_init_idc = uint8_t(s.cabacInit);
        v.slice_qp_delta = int8_t(s.qpDelta);
        v.disable_deblocking_filter_idc = uint8_t(s.deblock);
        v.slice_alpha_c0_offset_div2 = int8_t(s.alpha);
        v.slice_beta_offset_div2 = int8_t(s.beta);
        v.luma_log2_weight_denom = uint8_t(s.lumaDenom);
        v.chroma_log2_weight_denom = uint8_t(s.chromaDenom);
        VAPictureH264* out[2] = {v.RefPicList0, v.RefPicList1};
        for (int list = 0; list < 2; ++list) {
            int n = 0;
            const int count = s.listCount > list ? s.refCount[list] : 0;
            for (int i = 0; i < count; ++i) if (lists[list][i].reference) vaPicture(out[list][n++], lists[list][i].frame, lists[list][i].reference);
            for (; n < 32; ++n) empty(out[list][n]);
        }
        const int lumaDefault = 1 << s.lumaDenom, chromaDefault = 1 << s.chromaDenom;
        v.luma_weight_l0_flag = s.lumaFlag[0];
        v.chroma_weight_l0_flag = s.chromaFlag[0];
        v.luma_weight_l1_flag = s.lumaFlag[1];
        v.chroma_weight_l1_flag = s.chromaFlag[1];
        for (int i = 0; i < s.refCount[0] && i < 32; ++i) {
            v.luma_weight_l0[i] = s.lumaFlag[0] ? s.lumaWeight[0][i][0] : short(lumaDefault);
            v.luma_offset_l0[i] = s.lumaFlag[0] ? s.lumaWeight[0][i][1] : 0;
            for (int j = 0; j < 2; ++j) {
                v.chroma_weight_l0[i][j] = s.chromaFlag[0] ? s.chromaWeight[0][i][j][0] : short(chromaDefault);
                v.chroma_offset_l0[i][j] = s.chromaFlag[0] ? s.chromaWeight[0][i][j][1] : 0;
            }
        }
        for (int i = 0; i < s.refCount[1] && i < 32; ++i) {
            v.luma_weight_l1[i] = s.lumaFlag[1] ? s.lumaWeight[1][i][0] : short(lumaDefault);
            v.luma_offset_l1[i] = s.lumaFlag[1] ? s.lumaWeight[1][i][1] : 0;
            for (int j = 0; j < 2; ++j) {
                v.chroma_weight_l1[i][j] = s.chromaFlag[1] ? s.chromaWeight[1][i][j][0] : short(chromaDefault);
                v.chroma_offset_l1[i][j] = s.chromaFlag[1] ? s.chromaWeight[1][i][j][1] : 0;
            }
        }
        usable = session.slice(&v, sizeof(v), nal, size) && usable;
    }

    bool show(int f, int64_t when, const PictureSink& sink) {
        const int left = active.crop[0], top = active.crop[2];
        const int w = active.mbWidth * 16 - left - active.crop[1], h = active.mbHeight * 16 - top - active.crop[3];
        return session.hand(frames[f].surface, w, h, when, color, sink, left, top);
    }

    void flushPending(const PictureSink& sink) {
        const int f = pending;
        pending = -1;
        show(f, pendingStamp, sink);
    }

    bool fieldEnd(const PictureSink& sink) {
        open = false;
        if (!droppable) {
            marking();
            prevPocMsb = pocMsb;
            prevPocLsb = pocLsb;
        }
        prevFrameNumOffset = frameNumOffset;
        prevFrameNum = frameNum;
        const bool ok = session.submit(frames[cur].surface) && usable;
        if (structure != kFrame && firstField) {
            pending = cur;
            pendingStamp = stamp;
            return ok;
        }
        if (structure != kFrame && pending == cur) {
            pending = -1;
            return show(cur, pendingStamp, sink) && ok;
        }
        return ok && show(cur, stamp, sink);
    }

    bool unit(const uint8_t* nal, size_t size, const PictureSink& sink) {
        if (!size) return true;
        const int type = nal[0] & 31, refIdc = (nal[0] >> 5) & 3;
        if (type == 7) {
            std::vector<uint8_t> rbsp = unescape(nal, std::min<size_t>(size, 4096));
            BitReader b{rbsp.data(), rbsp.size()};
            b.skip(8);
            Sps s;
            int id = 0;
            if (parseSps(b, s, id)) sps[id] = s;
            return true;
        }
        if (type == 8) {
            std::vector<uint8_t> rbsp = unescape(nal, std::min<size_t>(size, 4096));
            BitReader b{rbsp.data(), rbsp.size()};
            b.skip(8);
            const unsigned id = b.ue();
            if (b.ok && id < 256) ppsRaw[id] = std::move(rbsp);
            return true;
        }
        if (type != 1 && type != 5) return true;
        if (type == 5 && size > 1 && (nal[1] & 0xFC) == 0x98) return false;
        scratch = unescape(nal, std::min<size_t>(size, 4096));
        BitReader b{scratch.data(), scratch.size()};
        Slice s;
        if (!parseSlice(b, type, refIdc, s)) return false;
        if (s.redundant > 0) return true;
        bool ok = true;
        if (s.firstMb == 0) {
            skipping = false;
            if (open) ok = fieldEnd(sink);
        }
        if (skipping) return false;
        if (type == 5 && !idrCleared && !open) {
            idr();
            idrCleared = true;
        }
        if (!open) {
            if (!parsePps(ppsRaw[s.ppsId], sps, pps)) return false;
            if (!fieldStart(s, type, refIdc, sink)) {
                open = false;
                skipping = true;
                return false;
            }
        } else if (s.structure != structure || droppable != (refIdc == 0)) {
            return false;
        }
        addSlice(s, nal, size);
        return ok;
    }

    bool decode(const uint8_t* data, size_t size, int64_t when, const PictureSink& sink) override {
        stamp = when;
        idrCleared = false;
        bool ok = true;
        for (size_t at = 0; at + size_t(lengthSize) <= size;) {
            size_t length = 0;
            for (int k = 0; k < lengthSize; ++k) length = (length << 8) | data[at + size_t(k)];
            at += size_t(lengthSize);
            if (length > size - at) {
                ok = false;
                break;
            }
            ok = unit(data + at, length, sink) && ok;
            at += length;
        }
        if (open) ok = fieldEnd(sink) && ok;
        return ok;
    }

    bool drain(const PictureSink& sink) override {
        if (pending >= 0) flushPending(sink);
        return true;
    }
};

}

std::unique_ptr<SoftDecoder> openVaapiH264(const std::vector<uint8_t>& config, std::string* error) {
    auto fail = [&](const char* why) {
        if (error) *error = why;
        return std::unique_ptr<SoftDecoder>();
    };
    if (config.size() < 7) return fail("the H.264 configuration is not readable");
    auto decoder = std::make_unique<H264Decoder>();
    decoder->lengthSize = (config[4] & 3) + 1;
    size_t at = 5;
    const int spsCount = config[at++] & 31;
    std::vector<std::pair<const uint8_t*, size_t>> units;
    for (int pass = 0; pass < 2; ++pass) {
        const int count = pass == 0 ? spsCount : (at < config.size() ? config[at++] : 0);
        for (int i = 0; i < count && at + 2 <= config.size(); ++i) {
            const size_t length = (size_t(config[at]) << 8) | config[at + 1];
            at += 2;
            if (length > config.size() - at) return fail("the H.264 configuration is not readable");
            units.emplace_back(config.data() + at, length);
            at += length;
        }
    }
    for (const auto& u : units) decoder->unit(u.first, u.second, [](const Picture&) {});
    const Sps* first = nullptr;
    for (const Sps& s : decoder->sps) if (s.valid && !first) first = &s;
    if (!first) return fail("the H.264 configuration has no readable SPS");
    if (first->chroma != 1 || first->depth != 8 || first->depthChroma != 8) return fail("only 8-bit 4:2:0 H.264 is decoded on the GPU");
    if (vaapi() && vaapi()->d3d12) {
        for (const auto& raw : decoder->ppsRaw) {
            Pps p;
            if (raw.empty() || !parsePps(raw, decoder->sps, p)) continue;
            const uint8_t* m = &p.matrix.four[0][0];
            if (std::any_of(m, m + sizeof(p.matrix), [](uint8_t v) { return v != 16; })) return fail("the d3d12 driver ignores H.264 scaling matrices");
        }
    }
    const VideoInfo info = videoInfo(fourcc("avcC"), config);
    if (info.colorKnown || info.rangeKnown) {
        decoder->color.known = info.colorKnown;
        decoder->color.primaries = info.primaries;
        decoder->color.transfer = info.transfer;
        decoder->color.matrix = info.matrix;
        decoder->color.full = info.full;
    }
    const VAProfile profile = vaDecodes(VAProfileH264High) ? VAProfileH264High : VAProfileH264Main;
    if (!vaDecodes(profile) || !decoder->session.open(profile, first->mbWidth * 16, first->mbHeight * 16, false, 18)) return fail("the GPU does not decode H.264");
    return decoder;
}

}
#endif
