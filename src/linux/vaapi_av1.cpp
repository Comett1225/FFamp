#ifdef ABSENT_VAAPI
#include "vaapi.h"
#include "../nal.h"
#include <algorithm>
#include <cstdlib>
#include <cstring>

namespace absent {

namespace {

enum { kKey = 0, kInter = 1, kIntraOnly = 2, kSwitch = 3, kNoRef = 7 };

int tileLog2(int block, int target) {
    int k = 0;
    while ((block << k) < target) ++k;
    return k;
}

int floorLog2(uint32_t v) {
    int n = 0;
    while (v >>= 1) ++n;
    return n;
}

int su(BitReader& b, int n) {
    const int v = int(b.bits(n));
    return v & (1 << (n - 1)) ? v - (1 << n) : v;
}

uint32_t ns(BitReader& b, uint32_t n) {
    const int w = floorLog2(n) + 1;
    const uint32_t m = (1u << w) - n;
    const uint32_t v = w > 1 ? b.bits(w - 1) : 0;
    return v < m ? v : (v << 1) - m + b.bit();
}

uint32_t increment(BitReader& b, uint32_t low, uint32_t high) {
    uint32_t v = low;
    while (v < high && b.bit()) ++v;
    return v;
}

uint32_t subexp(BitReader& b, uint32_t range) {
    const uint32_t most = uint32_t(floorLog2(range - 1) - 3);
    const uint32_t len = increment(b, 0, most);
    const int bits = len ? int(len) + 2 : 3;
    const uint32_t offset = len ? 1u << bits : 0;
    return offset + (len < most ? b.bits(bits) : ns(b, range - offset));
}

int deltaQ(BitReader& b) { return b.bit() ? su(b, 7) : 0; }

uint32_t recenter(int r, uint32_t v) {
    if (v > uint32_t(2 * r)) return v;
    return v & 1 ? uint32_t(r) - ((v + 1) >> 1) : uint32_t(r) + (v >> 1);
}

int32_t gmRead(BitReader& b, int type, int idx, int32_t prev, bool precise) {
    int absBits = 12, precBits = 15;
    if (idx < 2) {
        absBits = type == 1 ? 9 - !precise : 12;
        precBits = type == 1 ? 3 - !precise : 6;
    }
    const uint32_t raw = subexp(b, 2 * (1u << absBits) + 1);
    const int precDiff = 16 - precBits;
    const int round = idx % 3 == 2 ? 1 << 16 : 0;
    const int sub = idx % 3 == 2 ? 1 << precBits : 0;
    const int mx = 1 << absBits, low = -mx, range = 2 * mx + 1;
    const int r = (prev >> precDiff) - sub - low;
    const uint32_t x = (r << 1) <= range ? recenter(r, raw) : uint32_t(range - 1) - recenter(range - 1 - r, raw);
    return ((int32_t(x) + low) * (1 << precDiff)) + round;
}

int64_t roundTwo(int64_t x, int n) {
    if (!n) return x;
    const int64_t half = int64_t(1) << (n - 1);
    return x < 0 ? -((-x + half) >> n) : (x + half) >> n;
}

int16_t clip16(int64_t v) { return int16_t(std::clamp<int64_t>(v, -32768, 32767)); }

bool shearValid(const int32_t* p) {
    if (p[2] <= 0) return false;
    int16_t alpha = clip16(p[2] - (1 << 16)), beta = clip16(p[3]);
    int shift = floorLog2(uint32_t(p[2]));
    const int64_t e = int64_t(p[2]) - (int64_t(1) << shift);
    const int64_t f = shift > 8 ? roundTwo(e, shift - 8) : e << (8 - shift);
    shift += 14;
    const int64_t divf = (4194304 + (256 + f) / 2) / (256 + f);
    int16_t gamma = clip16(int(roundTwo(int64_t(p[4]) * 65536 * divf, shift)));
    int16_t delta = clip16(int64_t(p[5]) - int(roundTwo(int64_t(p[3]) * p[4] * divf, shift)) - 65536);
    alpha = int16_t(roundTwo(alpha, 6) * 64);
    beta = int16_t(roundTwo(beta, 6) * 64);
    gamma = int16_t(roundTwo(gamma, 6) * 64);
    delta = int16_t(roundTwo(delta, 6) * 64);
    return 4 * std::abs(alpha) + 7 * std::abs(beta) < 65536 && 4 * std::abs(gamma) + 4 * std::abs(delta) < 65536;
}

struct Sequence {
    bool valid = false;
    int profile = 0;
    bool still = false;
    bool reduced = false;
    bool decoderModel = false;
    bool equalInterval = false;
    int presentationBits = 0;
    int removalBits = 0;
    int opCount = 1;
    uint32_t opIdc[32] = {};
    bool opModel[32] = {};
    int widthBits = 1;
    int heightBits = 1;
    int maxWidth = 1;
    int maxHeight = 1;
    bool frameIds = false;
    int deltaIdBits = 0;
    int idBits = 0;
    bool sb128 = false;
    bool filterIntra = false;
    bool intraEdge = false;
    bool interintra = false;
    bool masked = false;
    bool warped = false;
    bool dualFilter = false;
    bool orderHint = false;
    bool jnt = false;
    bool refMvs = false;
    int screenContent = 2;
    int integerMv = 2;
    int orderBits = 0;
    bool superres = false;
    bool cdef = false;
    bool restoration = false;
    int bits = 8;
    bool mono = false;
    int subX = 1;
    int subY = 1;
    bool separateUv = false;
    int matrix = 2;
    bool grain = false;
    Color color;
};

bool parseSequence(BitReader& b, Sequence& s) {
    s.profile = int(b.bits(3));
    s.still = b.bit();
    s.reduced = b.bit();
    if (s.reduced) {
        b.skip(5);
    } else {
        int delayBits = 0;
        if (b.bit()) {
            b.skip(64);
            s.equalInterval = b.bit();
            if (s.equalInterval) {
                int zeros = 0;
                while (b.ok && !b.bit()) if (++zeros >= 32) return false;
                b.skip(size_t(zeros));
            }
            s.decoderModel = b.bit();
            if (s.decoderModel) {
                delayBits = int(b.bits(5)) + 1;
                b.skip(32);
                s.removalBits = int(b.bits(5)) + 1;
                s.presentationBits = int(b.bits(5)) + 1;
            }
        }
        const bool displayDelay = b.bit();
        s.opCount = int(b.bits(5)) + 1;
        for (int i = 0; i < s.opCount; ++i) {
            s.opIdc[i] = b.bits(12);
            if (b.bits(5) > 7) b.skip(1);
            if (s.decoderModel && (s.opModel[i] = b.bit())) b.skip(size_t(2 * delayBits + 1));
            if (displayDelay && b.bit()) b.skip(4);
        }
    }
    s.widthBits = int(b.bits(4)) + 1;
    s.heightBits = int(b.bits(4)) + 1;
    s.maxWidth = int(b.bits(s.widthBits)) + 1;
    s.maxHeight = int(b.bits(s.heightBits)) + 1;
    s.frameIds = !s.reduced && b.bit();
    if (s.frameIds) {
        s.deltaIdBits = int(b.bits(4)) + 2;
        s.idBits = int(b.bits(3)) + 1 + s.deltaIdBits;
    }
    s.sb128 = b.bit();
    s.filterIntra = b.bit();
    s.intraEdge = b.bit();
    if (!s.reduced) {
        s.interintra = b.bit();
        s.masked = b.bit();
        s.warped = b.bit();
        s.dualFilter = b.bit();
        s.orderHint = b.bit();
        if (s.orderHint) {
            s.jnt = b.bit();
            s.refMvs = b.bit();
        }
        s.screenContent = b.bit() ? 2 : int(b.bit());
        s.integerMv = s.screenContent > 0 ? (b.bit() ? 2 : int(b.bit())) : 2;
        if (s.orderHint) s.orderBits = int(b.bits(3)) + 1;
    }
    s.superres = b.bit();
    s.cdef = b.bit();
    s.restoration = b.bit();
    const bool high = b.bit();
    s.bits = s.profile == 2 && high ? (b.bit() ? 12 : 10) : high ? 10 : 8;
    s.mono = s.profile != 1 && b.bit();
    int primaries = 2, transfer = 2;
    s.color.known = b.bit();
    if (s.color.known) {
        primaries = int(b.bits(8));
        transfer = int(b.bits(8));
        s.matrix = int(b.bits(8));
    }
    if (s.mono) {
        s.color.full = b.bit();
    } else if (primaries == 1 && transfer == 13 && s.matrix == 0) {
        s.color.full = true;
        s.subX = s.subY = 0;
        s.separateUv = b.bit();
    } else {
        s.color.full = b.bit();
        if (s.profile == 1) {
            s.subX = s.subY = 0;
        } else if (s.profile == 2) {
            s.subX = s.bits == 12 ? int(b.bit()) : 1;
            s.subY = s.bits == 12 && s.subX ? int(b.bit()) : 0;
        }
        if (s.subX && s.subY) b.skip(2);
        s.separateUv = b.bit();
    }
    s.grain = b.bit();
    s.color.primaries = primaries;
    s.color.transfer = transfer;
    s.color.matrix = s.matrix;
    s.valid = b.ok;
    return s.valid;
}

bool supported(const Sequence& s) {
    return s.profile == 0 && !s.mono && s.bits <= 10 && s.subX && s.subY && s.opIdc[0] == 0;
}

struct Slot {
    bool valid = false;
    int frameId = 0;
    int upscaled = 0;
    int width = 0;
    int height = 0;
    int renderWidth = 0;
    int renderHeight = 0;
    int type = 0;
    int orderHint = 0;
    int8_t refDeltas[8] = {1, 0, 0, 0, -1, 0, -1, -1};
    int8_t modeDeltas[2] = {};
    uint8_t features[8] = {};
    int16_t values[8][8] = {};
    int32_t gm[8][6] = {};
    VASurfaceID surface = VA_INVALID_SURFACE;
};

struct Tile {
    const uint8_t* data;
    uint32_t size;
    int row;
    int col;
};

struct Av1Decoder : SoftDecoder {
    VaSession session{&wanted};
    Sequence seq;
    Slot slots[8];
    Slot cur;
    VADecPictureParameterBufferAV1 pic{};
    bool seen = false;
    bool dead = false;
    bool show = false;
    int refresh = 0;
    int tileCols = 1;
    int tileRows = 1;
    int tileBytes = 1;
    int tileNum = 0;
    int temporalId = 0;
    int spatialId = 0;
    std::vector<Tile> tiles;

    const char* name() const override { return "vaapi"; }
    void reset() override {
        for (Slot& slot : slots) slot = Slot();
        seen = false;
        tiles.clear();
    }

    int relative(int a, int b) const {
        if (!seq.orderHint) return 0;
        const int d = a - b, m = 1 << (seq.orderBits - 1);
        return (d & (m - 1)) - (d & m);
    }

    void frameRefs(int last, int golden, int hint, int* refIdx) const {
        bool used[8] = {};
        int shifted[8];
        for (int i = 0; i < 7; ++i) refIdx[i] = -1;
        refIdx[0] = last;
        refIdx[3] = golden;
        used[last] = used[golden] = true;
        const int now = 1 << (seq.orderBits - 1);
        for (int i = 0; i < 8; ++i) shifted[i] = now + relative(slots[i].orderHint, hint);
        int latest = shifted[last], earliest = shifted[golden];
        auto pick = [&](int into, bool later, bool forward) {
            int ref = -1;
            for (int i = 0; i < 8; ++i) {
                const int h = shifted[i];
                if (used[i] || (forward ? h >= now : h < now)) continue;
                if (ref < 0 || (later ? h >= latest : h < earliest)) {
                    ref = i;
                    (later ? latest : earliest) = h;
                }
            }
            if (ref >= 0) {
                refIdx[into] = ref;
                used[ref] = true;
            }
        };
        pick(6, true, false);
        pick(4, false, false);
        pick(5, false, false);
        for (int into : {1, 2, 4, 5, 6}) if (refIdx[into] < 0) pick(into, true, true);
        int ref = -1;
        for (int i = 0; i < 8; ++i) {
            if (ref < 0 || shifted[i] < earliest) {
                ref = i;
                earliest = shifted[i];
            }
        }
        for (int i = 0; i < 7; ++i) if (refIdx[i] < 0) refIdx[i] = ref;
    }

    bool frameHeader(BitReader& b, int& existing) {
        const Sequence& s = seq;
        if (!s.valid) return false;
        pic = VADecPictureParameterBufferAV1{};
        cur = Slot();
        tiles.clear();
        tileNum = 0;
        int type = kKey;
        bool showable = false, errorRes = false;
        show = true;
        if (!s.reduced && b.bit()) {
            existing = int(b.bits(3));
            if (s.decoderModel && !s.equalInterval) b.skip(size_t(s.presentationBits));
            if (s.frameIds) b.skip(size_t(s.idBits));
            return b.ok && slots[existing].valid;
        }
        if (!s.reduced) {
            type = int(b.bits(2));
            show = b.bit();
            if (show && s.decoderModel && !s.equalInterval) b.skip(size_t(s.presentationBits));
            showable = show ? type != kKey : b.bit();
            errorRes = type == kSwitch || (type == kKey && show) || b.bit();
        }
        const bool intra = type == kKey || type == kIntraOnly;
        if (type == kKey && show) {
            for (Slot& slot : slots) {
                slot.valid = false;
                slot.orderHint = 0;
            }
        }
        const bool disableCdf = b.bit();
        const bool screen = s.screenContent == 2 ? b.bit() : s.screenContent == 1;
        const bool integerMv = screen && (s.integerMv == 2 ? b.bit() : s.integerMv == 1);
        if (s.frameIds) {
            cur.frameId = int(b.bits(s.idBits));
            const int diff = 1 << s.deltaIdBits, id = cur.frameId;
            for (Slot& slot : slots) {
                const bool stale = id > diff ? slot.frameId > id || slot.frameId < id - diff : slot.frameId > id && slot.frameId < (1 << s.idBits) + id - diff;
                if (stale) slot.valid = false;
            }
        }
        const bool override = type == kSwitch || (!s.reduced && b.bit());
        cur.orderHint = s.orderHint ? int(b.bits(s.orderBits)) : 0;
        const int primary = intra || errorRes ? kNoRef : int(b.bits(3));
        if (s.decoderModel && b.bit()) {
            for (int i = 0; i < s.opCount; ++i) {
                const uint32_t idc = s.opIdc[i];
                if (s.opModel[i] && (idc == 0 || (((idc >> temporalId) & 1) && ((idc >> (spatialId + 8)) & 1)))) b.skip(size_t(s.removalBits));
            }
        }
        refresh = type == kSwitch || (type == kKey && show) ? 0xFF : int(b.bits(8));
        if ((!intra || refresh != 0xFF) && s.orderHint) {
            for (Slot& slot : slots) {
                const int hint = errorRes ? int(b.bits(s.orderBits)) : slot.orderHint;
                if (hint != slot.orderHint) slot.valid = false;
            }
        }
        bool superres = false;
        int denom = 8;
        auto superresParams = [&] {
            superres = s.superres && b.bit();
            denom = superres ? int(b.bits(3)) + 9 : 8;
            cur.width = (cur.upscaled * 8 + denom / 2) / denom;
        };
        auto frameSize = [&] {
            cur.upscaled = override ? int(b.bits(s.widthBits)) + 1 : s.maxWidth;
            cur.height = override ? int(b.bits(s.heightBits)) + 1 : s.maxHeight;
            superresParams();
            const bool differ = b.bit();
            cur.renderWidth = differ ? int(b.bits(16)) + 1 : cur.upscaled;
            cur.renderHeight = differ ? int(b.bits(16)) + 1 : cur.height;
        };
        bool intrabc = false, precise = false, refMvs = false, motionSwitchable = false;
        int filter = 0;
        int refIdx[7] = {};
        if (intra) {
            frameSize();
            intrabc = screen && cur.upscaled == cur.width && b.bit();
        } else {
            const bool shortSignal = s.orderHint && b.bit();
            if (shortSignal) {
                const int last = int(b.bits(3));
                frameRefs(last, int(b.bits(3)), cur.orderHint, refIdx);
            }
            for (int& r : refIdx) {
                if (!shortSignal) r = int(b.bits(3));
                if (s.frameIds) b.skip(size_t(s.deltaIdBits));
            }
            int found = 7;
            if (override && !errorRes) for (found = 0; found < 7 && !b.bit(); ++found) {}
            if (found < 7) {
                const Slot& ref = slots[refIdx[found]];
                if (!ref.valid) return false;
                cur.upscaled = ref.upscaled;
                cur.height = ref.height;
                cur.renderWidth = ref.renderWidth;
                cur.renderHeight = ref.renderHeight;
                superresParams();
            } else {
                frameSize();
            }
            precise = !integerMv && b.bit();
            filter = b.bit() ? 4 : int(b.bits(2));
            motionSwitchable = b.bit();
            refMvs = !errorRes && s.refMvs && b.bit();
        }
        const bool endUpdateOff = s.reduced || disableCdf || b.bit();

        const int miCols = 2 * ((cur.width + 7) >> 3), miRows = 2 * ((cur.height + 7) >> 3);
        const int sbShift = s.sb128 ? 5 : 4, sbSize = sbShift + 2;
        const int sbCols = (miCols + (1 << sbShift) - 1) >> sbShift, sbRows = (miRows + (1 << sbShift) - 1) >> sbShift;
        const int maxTileWidth = 4096 >> sbSize;
        const int minLog2Cols = tileLog2(maxTileWidth, sbCols);
        const int maxLog2Cols = tileLog2(1, std::min(sbCols, 64)), maxLog2Rows = tileLog2(1, std::min(sbRows, 64));
        const int minLog2Tiles = std::max(minLog2Cols, tileLog2((4096 * 2304) >> (2 * sbSize), sbRows * sbCols));
        const bool uniform = b.bit();
        int colsLog2 = 0, rowsLog2 = 0;
        int widths[64], heights[64];
        if (uniform) {
            colsLog2 = int(increment(b, uint32_t(minLog2Cols), uint32_t(maxLog2Cols)));
            const int w = (sbCols + (1 << colsLog2) - 1) >> colsLog2;
            tileCols = (sbCols + w - 1) / w;
            rowsLog2 = int(increment(b, uint32_t(std::max(minLog2Tiles - colsLog2, 0)), uint32_t(maxLog2Rows)));
            const int h = (sbRows + (1 << rowsLog2) - 1) >> rowsLog2;
            tileRows = (sbRows + h - 1) / h;
            if (tileCols > 64 || tileRows > 64) return false;
            for (int i = 0; i < tileCols; ++i) widths[i] = i < tileCols - 1 ? w - 1 : sbCols - (tileCols - 1) * w - 1;
            for (int i = 0; i < tileRows; ++i) heights[i] = i < tileRows - 1 ? h - 1 : sbRows - (tileRows - 1) * h - 1;
        } else {
            int widest = 0, start = 0, i = 0;
            for (; start < sbCols && i < 64; ++i) {
                widths[i] = int(ns(b, uint32_t(std::min(sbCols - start, maxTileWidth))));
                widest = std::max(widest, widths[i] + 1);
                start += widths[i] + 1;
            }
            colsLog2 = tileLog2(1, i);
            tileCols = i;
            const int area = minLog2Tiles > 0 ? (sbRows * sbCols) >> (minLog2Tiles + 1) : sbRows * sbCols;
            const int maxTileHeight = std::max(area / std::max(widest, 1), 1);
            for (start = 0, i = 0; start < sbRows && i < 64; ++i) {
                heights[i] = int(ns(b, uint32_t(std::min(sbRows - start, maxTileHeight))));
                start += heights[i] + 1;
            }
            rowsLog2 = tileLog2(1, i);
            tileRows = i;
        }
        int contextTile = 0;
        tileBytes = 1;
        if (colsLog2 > 0 || rowsLog2 > 0) {
            contextTile = int(b.bits(colsLog2 + rowsLog2));
            tileBytes = int(b.bits(2)) + 1;
        }
        if (tileCols < 1 || tileRows < 1 || tileCols * tileRows > 128 || contextTile >= tileCols * tileRows) return false;

        pic.base_qindex = uint8_t(b.bits(8));
        pic.y_dc_delta_q = int8_t(deltaQ(b));
        if (!s.mono) {
            const bool diffUv = s.separateUv && b.bit();
            pic.u_dc_delta_q = int8_t(deltaQ(b));
            pic.u_ac_delta_q = int8_t(deltaQ(b));
            pic.v_dc_delta_q = diffUv ? int8_t(deltaQ(b)) : pic.u_dc_delta_q;
            pic.v_ac_delta_q = diffUv ? int8_t(deltaQ(b)) : pic.u_ac_delta_q;
        }
        auto& qm = pic.qmatrix_fields.bits;
        if ((qm.using_qmatrix = b.bit())) {
            qm.qm_y = b.bits(4);
            qm.qm_u = b.bits(4);
            qm.qm_v = s.separateUv ? b.bits(4) : qm.qm_u;
        }

        auto& seg = pic.seg_info.segment_info_fields.bits;
        if ((seg.enabled = b.bit())) {
            if (primary == kNoRef) {
                seg.update_map = 1;
                seg.update_data = 1;
            } else {
                seg.update_map = b.bit();
                seg.temporal_update = seg.update_map && b.bit();
                seg.update_data = b.bit();
            }
            static const int kSegBits[8] = {8, 6, 6, 6, 6, 3, 0, 0};
            for (int i = 0; i < 8; ++i) {
                if (!seg.update_data) {
                    const Slot& from = slots[refIdx[primary]];
                    cur.features[i] = from.features[i];
                    memcpy(cur.values[i], from.values[i], sizeof(cur.values[i]));
                    continue;
                }
                for (int j = 0; j < 8; ++j) {
                    const bool on = b.bit();
                    cur.features[i] |= uint8_t(on << j);
                    if (on && kSegBits[j]) cur.values[i][j] = int16_t(j < 5 ? su(b, 1 + kSegBits[j]) : int(b.bits(kSegBits[j])));
                }
            }
        }

        auto& mode = pic.mode_control_fields.bits;
        if ((mode.delta_q_present_flag = pic.base_qindex > 0 && b.bit())) mode.log2_delta_q_res = b.bits(2);
        if (mode.delta_q_present_flag && !intrabc && (mode.delta_lf_present_flag = b.bit())) {
            mode.log2_delta_lf_res = b.bits(2);
            mode.delta_lf_multi = b.bit();
        }

        bool lossless = !pic.y_dc_delta_q && !pic.u_ac_delta_q && !pic.u_dc_delta_q && !pic.v_ac_delta_q && !pic.v_dc_delta_q;
        for (int i = 0; i < 8 && lossless; ++i) {
            const int q = (cur.features[i] & 1) ? pic.base_qindex + cur.values[i][0] : pic.base_qindex;
            if (std::clamp(q, 0, 255)) lossless = false;
        }
        const bool allLossless = lossless && cur.width == cur.upscaled;

        auto& lf = pic.loop_filter_info_fields.bits;
        if (!lossless && !intrabc) {
            pic.filter_level[0] = uint8_t(b.bits(6));
            pic.filter_level[1] = uint8_t(b.bits(6));
            if (!s.mono && (pic.filter_level[0] || pic.filter_level[1])) {
                pic.filter_level_u = uint8_t(b.bits(6));
                pic.filter_level_v = uint8_t(b.bits(6));
            }
            lf.sharpness_level = b.bits(3);
            if ((lf.mode_ref_delta_enabled = b.bit())) {
                if (primary != kNoRef) {
                    memcpy(cur.refDeltas, slots[refIdx[primary]].refDeltas, 8);
                    memcpy(cur.modeDeltas, slots[refIdx[primary]].modeDeltas, 2);
                }
                lf.mode_ref_delta_update = b.bit();
                for (int8_t& d : cur.refDeltas) if (lf.mode_ref_delta_update && b.bit()) d = int8_t(su(b, 7));
                for (int8_t& d : cur.modeDeltas) if (lf.mode_ref_delta_update && b.bit()) d = int8_t(su(b, 7));
            }
        }

        if (!lossless && !intrabc && s.cdef) {
            pic.cdef_damping_minus_3 = uint8_t(b.bits(2));
            pic.cdef_bits = uint8_t(b.bits(2));
            for (int i = 0; i < (1 << pic.cdef_bits); ++i) {
                const int yPri = int(b.bits(4)), ySec = int(b.bits(2));
                pic.cdef_y_strengths[i] = uint8_t((yPri << 2) + ySec);
                if (!s.mono) {
                    const int uvPri = int(b.bits(4)), uvSec = int(b.bits(2));
                    pic.cdef_uv_strengths[i] = uint8_t((uvPri << 2) + uvSec);
                }
            }
        }

        auto& lr = pic.loop_restoration_fields.bits;
        if (!allLossless && !intrabc && s.restoration) {
            static const uint8_t kRemap[4] = {0, 3, 1, 2};
            int types[3] = {};
            bool usesLr = false, usesChroma = false;
            for (int i = 0; i < (s.mono ? 1 : 3); ++i) {
                types[i] = int(b.bits(2));
                usesLr = usesLr || types[i];
                usesChroma = usesChroma || (i && types[i]);
            }
            lr.yframe_restoration_type = kRemap[types[0]];
            lr.cbframe_restoration_type = kRemap[types[1]];
            lr.crframe_restoration_type = kRemap[types[2]];
            if (usesLr) {
                lr.lr_unit_shift = s.sb128 ? increment(b, 1, 2) : increment(b, 0, 2);
                lr.lr_uv_shift = s.subX && s.subY && usesChroma && b.bit();
            }
        }

        mode.tx_mode = lossless ? 0 : increment(b, 1, 2);
        mode.reference_select = !intra && b.bit();
        bool skipAllowed = false;
        if (!intra && mode.reference_select && s.orderHint) {
            int forward = -1, backward = -1, forwardHint = 0, backwardHint = 0;
            for (int i = 0; i < 7; ++i) {
                const int h = slots[refIdx[i]].orderHint, d = relative(h, cur.orderHint);
                if (d < 0 && (forward < 0 || relative(h, forwardHint) > 0)) {
                    forward = i;
                    forwardHint = h;
                } else if (d > 0 && (backward < 0 || relative(h, backwardHint) < 0)) {
                    backward = i;
                    backwardHint = h;
                }
            }
            skipAllowed = forward >= 0 && backward >= 0;
            if (forward >= 0 && backward < 0) {
                int second = -1, secondHint = 0;
                for (int i = 0; i < 7; ++i) {
                    const int h = slots[refIdx[i]].orderHint;
                    if (relative(h, forwardHint) < 0 && (second < 0 || relative(h, secondHint) > 0)) {
                        second = i;
                        secondHint = h;
                    }
                }
                skipAllowed = second >= 0;
            }
        }
        mode.skip_mode_present = skipAllowed && b.bit();
        const bool warped = !intra && !errorRes && s.warped && b.bit();
        mode.reduced_tx_set_used = b.bit();

        for (int r = 1; r < 8; ++r) {
            for (int i = 0; i < 6; ++i) cur.gm[r][i] = i % 3 == 2 ? 1 << 16 : 0;
        }
        int gmType[8] = {};
        bool gmInvalid[8] = {};
        if (!intra) {
            const Slot* prev = primary == kNoRef ? &cur : &slots[refIdx[primary]];
            int32_t base[8][6];
            memcpy(base, prev->gm, sizeof(base));
            for (int r = 1; r < 8; ++r) {
                int kind = 0;
                if (b.bit()) kind = b.bit() ? 2 : b.bit() ? 1 : 3;
                gmType[r] = kind;
                int32_t* p = cur.gm[r];
                if (kind >= 2) {
                    p[2] = gmRead(b, kind, 2, base[r][2], precise);
                    p[3] = gmRead(b, kind, 3, base[r][3], precise);
                    if (kind == 3) {
                        p[4] = gmRead(b, kind, 4, base[r][4], precise);
                        p[5] = gmRead(b, kind, 5, base[r][5], precise);
                    } else {
                        p[4] = -p[3];
                        p[5] = p[2];
                    }
                }
                if (kind >= 1) {
                    p[0] = gmRead(b, kind, 0, base[r][0], precise);
                    p[1] = gmRead(b, kind, 1, base[r][1], precise);
                }
                gmInvalid[r] = !shearValid(p);
            }
        }

        if (s.grain && (show || showable) && b.bit()) {
            b.skip(16);
            if (type == kInter && !b.bit()) {
                b.skip(3);
            } else {
                auto points = [&](int most) {
                    const int n = int(b.bits(4));
                    for (int i = 0, last = -1; i < n; ++i) {
                        const int v = int(b.bits(8));
                        b.skip(8);
                        if (v <= last) b.ok = false;
                        last = v;
                    }
                    if (n > most) b.ok = false;
                    return n;
                };
                const int numY = points(14);
                const bool fromLuma = !s.mono && b.bit();
                int numCb = 0, numCr = 0;
                if (!s.mono && !fromLuma && !(s.subX && s.subY && !numY)) {
                    numCb = points(10);
                    numCr = points(10);
                }
                b.skip(2);
                const int lag = int(b.bits(2)), luma = 2 * lag * (lag + 1), chroma = numY ? luma + 1 : luma;
                const int coefficients = (numY ? luma : 0) + (fromLuma || numCb ? chroma : 0) + (fromLuma || numCr ? chroma : 0);
                b.skip(size_t(8 * coefficients + 4 + (numCb ? 25 : 0) + (numCr ? 25 : 0) + 2));
            }
        }
        if (!b.ok) return false;

        cur.valid = true;
        cur.type = type;
        auto& q = pic.seq_info_fields.fields;
        q.still_picture = s.still;
        q.use_128x128_superblock = s.sb128;
        q.enable_filter_intra = s.filterIntra;
        q.enable_intra_edge_filter = s.intraEdge;
        q.enable_interintra_compound = s.interintra;
        q.enable_masked_compound = s.masked;
        q.enable_dual_filter = s.dualFilter;
        q.enable_order_hint = s.orderHint;
        q.enable_jnt_comp = s.jnt;
        q.enable_cdef = s.cdef;
        q.mono_chrome = s.mono;
        q.color_range = s.color.full;
        q.subsampling_x = uint32_t(s.subX);
        q.subsampling_y = uint32_t(s.subY);
        pic.profile = uint8_t(s.profile);
        pic.order_hint_bits_minus_1 = uint8_t(s.orderHint ? s.orderBits - 1 : 0);
        pic.bit_depth_idx = s.bits > 8;
        pic.matrix_coefficients = uint8_t(s.matrix);
        pic.frame_width_minus1 = uint16_t(cur.upscaled - 1);
        pic.frame_height_minus1 = uint16_t(cur.height - 1);
        for (int i = 0; i < 7; ++i) pic.ref_frame_idx[i] = uint8_t(refIdx[i]);
        pic.primary_ref_frame = uint8_t(primary);
        pic.order_hint = uint8_t(cur.orderHint);
        pic.tile_cols = uint8_t(tileCols);
        pic.tile_rows = uint8_t(tileRows);
        for (int i = 0; i < std::min(tileCols, 63); ++i) pic.width_in_sbs_minus_1[i] = uint16_t(widths[i]);
        for (int i = 0; i < std::min(tileRows, 63); ++i) pic.height_in_sbs_minus_1[i] = uint16_t(heights[i]);
        pic.context_update_tile_id = uint16_t(contextTile);
        auto& f = pic.pic_info_fields.bits;
        f.frame_type = uint32_t(type);
        f.show_frame = show;
        f.showable_frame = showable;
        f.error_resilient_mode = errorRes;
        f.disable_cdf_update = disableCdf;
        f.allow_screen_content_tools = screen;
        f.force_integer_mv = integerMv || intra;
        f.allow_intrabc = intrabc;
        f.use_superres = superres;
        f.allow_high_precision_mv = precise;
        f.is_motion_mode_switchable = motionSwitchable;
        f.use_ref_frame_mvs = refMvs;
        f.disable_frame_end_update_cdf = endUpdateOff;
        f.allow_warped_motion = warped;
        pic.superres_scale_denominator = uint8_t(denom);
        pic.interp_filter = uint8_t(filter);
        memcpy(pic.ref_deltas, cur.refDeltas, 8);
        memcpy(pic.mode_deltas, cur.modeDeltas, 2);
        for (int r = 1; r < 8; ++r) {
            pic.wm[r - 1].invalid = gmInvalid[r];
            pic.wm[r - 1].wmtype = VAAV1TransformationType(gmType[r]);
            for (int j = 0; j < 6; ++j) pic.wm[r - 1].wmmat[j] = cur.gm[r][j];
        }
        static const int kSegMax[8] = {255, 63, 63, 63, 63, 7, 0, 0};
        for (int i = 0; i < 8; ++i) {
            pic.seg_info.feature_mask[i] = cur.features[i];
            for (int j = 0; j < 8; ++j) pic.seg_info.feature_data[i][j] = int16_t(std::clamp<int>(cur.values[i][j], j < 5 ? -kSegMax[j] : 0, kSegMax[j]));
        }
        return true;
    }

    bool finish(int64_t stamp, const PictureSink& sink) {
        seen = false;
        const bool key = cur.type == kKey;
        const bool deep = seq.bits > 8;
        if (!session.ready() || cur.upscaled != session.width() || cur.height != session.height() || deep != session.tenBit()) {
            if (!key || !show) return false;
            for (Slot& slot : slots) slot.surface = VA_INVALID_SURFACE;
            if (d3d12WrongTenBit(deep, cur.height) || !session.open(VAProfileAV1Profile0, cur.upscaled, cur.height, deep, 10)) return false;
        }
        const bool intra = key || cur.type == kIntraOnly;
        for (int i = 0; i < 7 && !intra; ++i) if (slots[pic.ref_frame_idx[i]].surface == VA_INVALID_SURFACE) return false;
        std::vector<VASurfaceID> busy;
        for (const Slot& slot : slots) busy.push_back(slot.surface);
        const VASurfaceID target = session.spare(busy);
        if (target == VA_INVALID_SURFACE) return false;
        VASurfaceID fallback = target;
        for (const Slot& slot : slots) {
            if (slot.surface != VA_INVALID_SURFACE) {
                fallback = slot.surface;
                break;
            }
        }
        for (int i = 0; i < 8; ++i) pic.ref_frame_map[i] = key && show ? VA_INVALID_ID : slots[i].surface != VA_INVALID_SURFACE ? slots[i].surface : fallback;
        pic.current_frame = target;
        pic.current_display_picture = target;
        bool ok = session.param(VAPictureParameterBufferType, &pic, sizeof(pic));
        for (const Tile& t : tiles) {
            VASliceParameterBufferAV1 slice{};
            slice.slice_data_size = t.size;
            slice.slice_data_flag = VA_SLICE_DATA_FLAG_ALL;
            slice.tile_row = uint16_t(t.row);
            slice.tile_column = uint16_t(t.col);
            ok = ok && session.slice(&slice, sizeof(slice), t.data, t.size);
        }
        tiles.clear();
        dead = !ok || !session.submit(target);
        if (dead) return false;
        cur.surface = target;
        for (int i = 0; i < 8; ++i) if (refresh & (1 << i)) slots[i] = cur;
        dead = show && !session.hand(target, cur.upscaled, cur.height, stamp, seq.color, sink);
        return !dead;
    }

    bool tileGroup(BitReader& b, const uint8_t* p, size_t n, int64_t stamp, const PictureSink& sink) {
        const int total = tileCols * tileRows;
        int first = 0, last = total - 1;
        if (total > 1 && b.bit()) {
            const int bits = tileLog2(1, tileCols) + tileLog2(1, tileRows);
            first = int(b.bits(bits));
            last = int(b.bits(bits));
            if (first < tileNum || last < first || last >= total) return false;
        }
        tileNum = last + 1;
        const size_t start = (b.at + 7) / 8;
        if (!b.ok || start > n) return false;
        const uint8_t* q = p + start;
        size_t left = n - start;
        for (int t = first; t <= last; ++t) {
            size_t length = left;
            if (t != last) {
                if (left < size_t(tileBytes)) return false;
                size_t v = 0;
                for (int i = 0; i < tileBytes; ++i) v |= size_t(q[i]) << (8 * i);
                q += tileBytes;
                left -= size_t(tileBytes);
                if (left <= v) return false;
                length = v + 1;
            }
            tiles.push_back({q, uint32_t(length), t / tileCols, t % tileCols});
            q += length;
            left -= length;
        }
        return last != total - 1 || finish(stamp, sink);
    }

    bool decode(const uint8_t* data, size_t size, int64_t stamp, const PictureSink& sink) override {
        if (dead) return false;
        seen = false;
        tiles.clear();
        size_t at = 0;
        while (at < size) {
            const uint8_t head = data[at++];
            if (head & 0x80) return false;
            const int type = (head >> 3) & 15;
            temporalId = spatialId = 0;
            if (head & 4) {
                if (at >= size) return false;
                temporalId = data[at] >> 5;
                spatialId = (data[at] >> 3) & 3;
                ++at;
            }
            uint64_t length = size - at;
            if (head & 2) {
                length = 0;
                int i = 0;
                for (; i < 8; ++i) {
                    if (at >= size) return false;
                    const uint8_t byte = data[at++];
                    length |= uint64_t(byte & 0x7F) << (7 * i);
                    if (!(byte & 0x80)) break;
                }
                if (i == 8) return false;
            }
            if (length > size - at) return false;
            const uint8_t* p = data + at;
            const size_t n = size_t(length);
            at += n;
            BitReader b{p, n};
            if (type == 1) {
                Sequence next;
                if (!parseSequence(b, next) || !supported(next)) return false;
                seq = next;
                seen = false;
                tiles.clear();
            } else if (type == 2) {
                seen = false;
                tiles.clear();
            } else if (type == 3 || type == 6 || (type == 7 && !seen)) {
                if (seen) return false;
                int existing = -1;
                if (!frameHeader(b, existing)) return false;
                if (existing >= 0) {
                    const Slot& slot = slots[existing];
                    if (type == 6 || slot.surface == VA_INVALID_SURFACE) return false;
                    if ((dead = !session.hand(slot.surface, slot.upscaled, slot.height, stamp, seq.color, sink))) return false;
                    if (slot.type == kKey) {
                        const Slot copy = slot;
                        for (Slot& other : slots) other = copy;
                    }
                    continue;
                }
                seen = true;
                if (type == 6) {
                    b.at = (b.at + 7) & ~size_t(7);
                    if (!tileGroup(b, p, n, stamp, sink)) return false;
                }
            } else if (type == 4) {
                if (!seen || !tileGroup(b, p, n, stamp, sink)) return false;
            }
        }
        return true;
    }
};

}

std::unique_ptr<SoftDecoder> openVaapiAv1(const std::vector<uint8_t>& config, const StreamShape& shape, int width, int height, std::string* error) {
    auto fail = [&](const char* why) {
        if (error) *error = why;
        return std::unique_ptr<SoftDecoder>();
    };
    if (shape.chromaX != 1 || shape.chromaY != 1 || shape.bits > 10 || (config.size() >= 4 && ((config[1] >> 5) || (config[2] & 0x10))))
        return fail("only 4:2:0 AV1 Main at 8 or 10 bits is decoded on the GPU");
    auto decoder = std::make_unique<Av1Decoder>();
    if (config.size() > 4 && !decoder->decode(config.data() + 4, config.size() - 4, 0, [](const Picture&) {}))
        return fail("the AV1 sequence header is not one the GPU decodes");
    if (decoder->seq.grain) return fail("AV1 film grain is applied by the software decoder");
    const bool deep = shape.bits > 8;
    if (d3d12WrongTenBit(deep, height)) return fail("the GPU decodes the chroma of this 10-bit AV1 frame height wrongly");
    if (!vaDecodes(VAProfileAV1Profile0) || !decoder->session.open(VAProfileAV1Profile0, width, height, deep, 10))
        return fail("the GPU does not decode AV1 Main");
    return decoder;
}

}
#endif
