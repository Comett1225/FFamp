#ifdef ABSENT_VAAPI
#include "vaapi.h"
#include "../nal.h"
#include <cstring>

namespace absent {

namespace {

const int16_t kDcQ[2][256] = {
    {
        4, 8, 8, 9, 10, 11, 12, 12, 13, 14, 15, 16, 17, 18, 19, 19, 20, 21, 22, 23, 24, 25, 26, 26,
        27, 28, 29, 30, 31, 32, 32, 33, 34, 35, 36, 37, 38, 38, 39, 40, 41, 42, 43, 43, 44, 45, 46, 47,
        48, 48, 49, 50, 51, 52, 53, 53, 54, 55, 56, 57, 57, 58, 59, 60, 61, 62, 62, 63, 64, 65, 66, 66,
        67, 68, 69, 70, 70, 71, 72, 73, 74, 74, 75, 76, 77, 78, 78, 79, 80, 81, 81, 82, 83, 84, 85, 85,
        87, 88, 90, 92, 93, 95, 96, 98, 99, 101, 102, 104, 105, 107, 108, 110, 111, 113, 114, 116, 117, 118, 120, 121,
        123, 125, 127, 129, 131, 134, 136, 138, 140, 142, 144, 146, 148, 150, 152, 154, 156, 158, 161, 164, 166, 169, 172, 174,
        177, 180, 182, 185, 187, 190, 192, 195, 199, 202, 205, 208, 211, 214, 217, 220, 223, 226, 230, 233, 237, 240, 243, 247,
        250, 253, 257, 261, 265, 269, 272, 276, 280, 284, 288, 292, 296, 300, 304, 309, 313, 317, 322, 326, 330, 335, 340, 344,
        349, 354, 359, 364, 369, 374, 379, 384, 389, 395, 400, 406, 411, 417, 423, 429, 435, 441, 447, 454, 461, 467, 475, 482,
        489, 497, 505, 513, 522, 530, 539, 549, 559, 569, 579, 590, 602, 614, 626, 640, 654, 668, 684, 700, 717, 736, 755, 775,
        796, 819, 843, 869, 896, 925, 955, 988, 1022, 1058, 1098, 1139, 1184, 1232, 1282, 1336,
    },
    {
        4, 9, 10, 13, 15, 17, 20, 22, 25, 28, 31, 34, 37, 40, 43, 47, 50, 53, 57, 60, 64, 68, 71, 75,
        78, 82, 86, 90, 93, 97, 101, 105, 109, 113, 116, 120, 124, 128, 132, 136, 140, 143, 147, 151, 155, 159, 163, 166,
        170, 174, 178, 182, 185, 189, 193, 197, 200, 204, 208, 212, 215, 219, 223, 226, 230, 233, 237, 241, 244, 248, 251, 255,
        259, 262, 266, 269, 273, 276, 280, 283, 287, 290, 293, 297, 300, 304, 307, 310, 314, 317, 321, 324, 327, 331, 334, 337,
        343, 350, 356, 362, 369, 375, 381, 387, 394, 400, 406, 412, 418, 424, 430, 436, 442, 448, 454, 460, 466, 472, 478, 484,
        490, 499, 507, 516, 525, 533, 542, 550, 559, 567, 576, 584, 592, 601, 609, 617, 625, 634, 644, 655, 666, 676, 687, 698,
        708, 718, 729, 739, 749, 759, 770, 782, 795, 807, 819, 831, 844, 856, 868, 880, 891, 906, 920, 933, 947, 961, 975, 988,
        1001, 1015, 1030, 1045, 1061, 1076, 1090, 1105, 1120, 1137, 1153, 1170, 1186, 1202, 1218, 1236, 1253, 1271, 1288, 1306, 1323, 1342, 1361, 1379,
        1398, 1416, 1436, 1456, 1476, 1496, 1516, 1537, 1559, 1580, 1601, 1624, 1647, 1670, 1692, 1717, 1741, 1766, 1791, 1817, 1844, 1871, 1900, 1929,
        1958, 1990, 2021, 2054, 2088, 2123, 2159, 2197, 2236, 2276, 2319, 2363, 2410, 2458, 2508, 2561, 2616, 2675, 2737, 2802, 2871, 2944, 3020, 3102,
        3188, 3280, 3375, 3478, 3586, 3702, 3823, 3953, 4089, 4236, 4394, 4559, 4737, 4929, 5130, 5347,
    },
};
const int16_t kAcQ[2][256] = {
    {
        4, 8, 9, 10, 11, 12, 13, 14, 15, 16, 17, 18, 19, 20, 21, 22, 23, 24, 25, 26, 27, 28, 29, 30,
        31, 32, 33, 34, 35, 36, 37, 38, 39, 40, 41, 42, 43, 44, 45, 46, 47, 48, 49, 50, 51, 52, 53, 54,
        55, 56, 57, 58, 59, 60, 61, 62, 63, 64, 65, 66, 67, 68, 69, 70, 71, 72, 73, 74, 75, 76, 77, 78,
        79, 80, 81, 82, 83, 84, 85, 86, 87, 88, 89, 90, 91, 92, 93, 94, 95, 96, 97, 98, 99, 100, 101, 102,
        104, 106, 108, 110, 112, 114, 116, 118, 120, 122, 124, 126, 128, 130, 132, 134, 136, 138, 140, 142, 144, 146, 148, 150,
        152, 155, 158, 161, 164, 167, 170, 173, 176, 179, 182, 185, 188, 191, 194, 197, 200, 203, 207, 211, 215, 219, 223, 227,
        231, 235, 239, 243, 247, 251, 255, 260, 265, 270, 275, 280, 285, 290, 295, 300, 305, 311, 317, 323, 329, 335, 341, 347,
        353, 359, 366, 373, 380, 387, 394, 401, 408, 416, 424, 432, 440, 448, 456, 465, 474, 483, 492, 501, 510, 520, 530, 540,
        550, 560, 571, 582, 593, 604, 615, 627, 639, 651, 663, 676, 689, 702, 715, 729, 743, 757, 771, 786, 801, 816, 832, 848,
        864, 881, 898, 915, 933, 951, 969, 988, 1007, 1026, 1046, 1066, 1087, 1108, 1129, 1151, 1173, 1196, 1219, 1243, 1267, 1292, 1317, 1343,
        1369, 1396, 1423, 1451, 1479, 1508, 1537, 1567, 1597, 1628, 1660, 1692, 1725, 1759, 1793, 1828,
    },
    {
        4, 9, 11, 13, 16, 18, 21, 24, 27, 30, 33, 37, 40, 44, 48, 51, 55, 59, 63, 67, 71, 75, 79, 83,
        88, 92, 96, 100, 105, 109, 114, 118, 122, 127, 131, 136, 140, 145, 149, 154, 158, 163, 168, 172, 177, 181, 186, 190,
        195, 199, 204, 208, 213, 217, 222, 226, 231, 235, 240, 244, 249, 253, 258, 262, 267, 271, 275, 280, 284, 289, 293, 297,
        302, 306, 311, 315, 319, 324, 328, 332, 337, 341, 345, 349, 354, 358, 362, 367, 371, 375, 379, 384, 388, 392, 396, 401,
        409, 417, 425, 433, 441, 449, 458, 466, 474, 482, 490, 498, 506, 514, 523, 531, 539, 547, 555, 563, 571, 579, 588, 596,
        604, 616, 628, 640, 652, 664, 676, 688, 700, 713, 725, 737, 749, 761, 773, 785, 797, 809, 825, 841, 857, 873, 889, 905,
        922, 938, 954, 970, 986, 1002, 1018, 1038, 1058, 1078, 1098, 1118, 1138, 1158, 1178, 1198, 1218, 1242, 1266, 1290, 1314, 1338, 1362, 1386,
        1411, 1435, 1463, 1491, 1519, 1547, 1575, 1603, 1631, 1663, 1695, 1727, 1759, 1791, 1823, 1859, 1895, 1931, 1967, 2003, 2039, 2079, 2119, 2159,
        2199, 2239, 2283, 2327, 2371, 2415, 2459, 2507, 2555, 2603, 2651, 2703, 2755, 2807, 2859, 2915, 2971, 3027, 3083, 3143, 3203, 3263, 3327, 3391,
        3455, 3523, 3591, 3659, 3731, 3803, 3876, 3952, 4028, 4104, 4184, 4264, 4348, 4432, 4516, 4604, 4692, 4784, 4876, 4972, 5068, 5168, 5268, 5372,
        5476, 5584, 5692, 5804, 5916, 6032, 6148, 6268, 6388, 6512, 6640, 6768, 6900, 7036, 7172, 7312,
    },
};

int sbits(BitReader& b, int count) {
    const int v = int(b.bits(count));
    return b.bit() ? -v : v;
}

int clip(int v, int top) { return v < 0 ? 0 : v > top ? top : v; }

struct Feature {
    bool q = false;
    bool lf = false;
    bool ref = false;
    bool skip = false;
    int qValue = 0;
    int lfValue = 0;
    int refValue = 0;
};

struct Vp9Decoder : SoftDecoder {
    VaSession session{&wanted};
    VASurfaceID refs[8];
    int refWidth[8] = {};
    int refHeight[8] = {};
    Color color;
    int profile = 0;
    int bits = 8;
    bool key = false;
    bool invisible = false;
    bool errorRes = false;
    bool intraOnly = false;
    int resetContext = 0;
    int refresh = 0;
    int refIndex[3] = {};
    bool signBias[3] = {};
    bool highPrecision = false;
    int filter = 0;
    bool refreshContext = false;
    bool parallel = false;
    int contextIndex = 0;
    int level = 0;
    int sharpness = 0;
    bool deltas = false;
    int refDelta[4] = {1, 0, -1, -1};
    int modeDelta[2] = {};
    int qIndex = 0;
    int yDc = 0;
    int uvDc = 0;
    int uvAc = 0;
    bool segEnabled = false;
    bool segMap = false;
    bool segTemporal = false;
    bool segAbsolute = false;
    uint8_t treeProbs[7] = {255, 255, 255, 255, 255, 255, 255};
    uint8_t predProbs[3] = {255, 255, 255};
    Feature feature[8];
    int log2Cols = 0;
    int log2Rows = 0;

    Vp9Decoder() { reset(); }
    const char* name() const override { return "vaapi"; }
    void reset() override {
        for (VASurfaceID& id : refs) id = VA_INVALID_SURFACE;
    }

    void colorDetails(BitReader& b) {
        bits = profile <= 1 ? 8 : 10 + 2 * int(b.bit());
        static const int matrices[8] = {2, 5, 1, 6, 7, 9, 2, 0};
        const int space = int(b.bits(3));
        color.matrix = matrices[space];
        color.primaries = color.matrix == 9 ? 9 : color.matrix == 1 ? 1 : 2;
        color.transfer = color.matrix == 1 ? 1 : 2;
        color.known = space != 0;
        color.full = b.bit();
    }

    bool frame(const uint8_t* data, size_t size, int64_t stamp, const PictureSink& sink) {
        BitReader b{data, size};
        if (b.bits(2) != 2) return false;
        profile = int(b.bit());
        profile |= int(b.bit()) << 1;
        if (profile == 3) profile += int(b.bit());
        if (profile != 0 && profile != 2) return false;
        if (b.bit()) {
            const int slot = int(b.bits(3));
            if (refs[slot] == VA_INVALID_SURFACE) return false;
            return session.hand(refs[slot], refWidth[slot], refHeight[slot], stamp, color, sink);
        }
        key = !b.bit();
        invisible = !b.bit();
        errorRes = b.bit();
        int width = 0, height = 0;
        if (key) {
            if (b.bits(24) != 0x498342) return false;
            colorDetails(b);
            refresh = 0xFF;
            width = int(b.bits(16)) + 1;
            height = int(b.bits(16)) + 1;
            if (b.bit()) b.skip(32);
        } else {
            intraOnly = invisible ? b.bit() : false;
            resetContext = errorRes ? 0 : int(b.bits(2));
            if (intraOnly) {
                if (b.bits(24) != 0x498342) return false;
                if (profile >= 1) {
                    colorDetails(b);
                } else {
                    bits = 8;
                    color.known = true;
                    color.matrix = 5;
                    color.primaries = 2;
                    color.transfer = 2;
                    color.full = false;
                }
                refresh = int(b.bits(8));
                width = int(b.bits(16)) + 1;
                height = int(b.bits(16)) + 1;
                if (b.bit()) b.skip(32);
            } else {
                refresh = int(b.bits(8));
                for (int i = 0; i < 3; ++i) {
                    refIndex[i] = int(b.bits(3));
                    signBias[i] = b.bit() && !errorRes;
                    if (refs[refIndex[i]] == VA_INVALID_SURFACE) return false;
                }
                int from = 0;
                while (from < 3 && !b.bit()) ++from;
                if (from < 3) {
                    width = refWidth[refIndex[from]];
                    height = refHeight[refIndex[from]];
                } else {
                    width = int(b.bits(16)) + 1;
                    height = int(b.bits(16)) + 1;
                }
                if (b.bit()) b.skip(32);
                highPrecision = b.bit();
                filter = b.bit() ? 4 : int(b.bits(2));
            }
        }
        refreshContext = errorRes ? false : b.bit();
        parallel = errorRes ? true : b.bit();
        contextIndex = int(b.bits(2));
        if (key || intraOnly) contextIndex = 0;
        if (key || errorRes || intraOnly) {
            refDelta[0] = 1;
            refDelta[1] = 0;
            refDelta[2] = refDelta[3] = -1;
            modeDelta[0] = modeDelta[1] = 0;
            for (Feature& f : feature) f = Feature();
        }
        level = int(b.bits(6));
        sharpness = int(b.bits(3));
        if ((deltas = b.bit()) && b.bit()) {
            for (int& d : refDelta) if (b.bit()) d = sbits(b, 6);
            for (int& d : modeDelta) if (b.bit()) d = sbits(b, 6);
        }
        qIndex = int(b.bits(8));
        yDc = b.bit() ? sbits(b, 4) : 0;
        uvDc = b.bit() ? sbits(b, 4) : 0;
        uvAc = b.bit() ? sbits(b, 4) : 0;
        if ((segEnabled = b.bit())) {
            if ((segMap = b.bit())) {
                for (uint8_t& p : treeProbs) p = uint8_t(b.bit() ? b.bits(8) : 255);
                if ((segTemporal = b.bit())) for (uint8_t& p : predProbs) p = uint8_t(b.bit() ? b.bits(8) : 255);
            }
            if (b.bit()) {
                segAbsolute = b.bit();
                for (Feature& f : feature) {
                    if ((f.q = b.bit())) f.qValue = sbits(b, 8);
                    if ((f.lf = b.bit())) f.lfValue = sbits(b, 6);
                    if ((f.ref = b.bit())) f.refValue = int(b.bits(2));
                    f.skip = b.bit();
                }
            }
        } else {
            segTemporal = false;
            segMap = false;
        }
        const int sbCols = (width + 63) >> 6;
        log2Cols = 0;
        while (sbCols > (64 << log2Cols)) ++log2Cols;
        int most = 0;
        while ((sbCols >> most) >= 4) ++most;
        most = std::max(0, most - 1);
        while (most > log2Cols && b.bit()) ++log2Cols;
        log2Rows = int(b.bit());
        if (log2Rows) log2Rows += int(b.bit());
        const int compressed = int(b.bits(16));
        const int header = int((b.at + 7) / 8);
        if (!b.ok || size_t(header) + size_t(compressed) > size) return false;

        const bool deep = bits > 8;
        if (bits > 10) return false;
        if (!session.ready() || width != session.width() || height != session.height() || deep != session.tenBit()) {
            if (!key && session.ready()) return false;
            reset();
            if (d3d12Wrong(deep, width, height) || !session.open(deep ? VAProfileVP9Profile2 : VAProfileVP9Profile0, width, height, deep, 10)) return false;
        }
        const VASurfaceID target = session.spare(std::vector<VASurfaceID>(std::begin(refs), std::end(refs)));
        if (target == VA_INVALID_SURFACE) return false;

        VADecPictureParameterBufferVP9 pic{};
        pic.frame_width = uint16_t(width);
        pic.frame_height = uint16_t(height);
        for (int i = 0; i < 8; ++i) pic.reference_frames[i] = refs[i];
        auto& f = pic.pic_fields.bits;
        f.subsampling_x = 1;
        f.subsampling_y = 1;
        f.frame_type = !key;
        f.show_frame = !invisible;
        f.error_resilient_mode = errorRes;
        f.intra_only = intraOnly;
        f.allow_high_precision_mv = key ? 0 : highPrecision;
        f.mcomp_filter_type = uint32_t(filter ^ (filter <= 1));
        f.frame_parallel_decoding_mode = parallel;
        f.reset_frame_context = uint32_t(resetContext);
        f.refresh_frame_context = refreshContext;
        f.frame_context_idx = uint32_t(contextIndex);
        f.segmentation_enabled = segEnabled;
        f.segmentation_temporal_update = segTemporal;
        f.segmentation_update_map = segMap;
        f.last_ref_frame = uint32_t(refIndex[0]);
        f.last_ref_frame_sign_bias = signBias[0];
        f.golden_ref_frame = uint32_t(refIndex[1]);
        f.golden_ref_frame_sign_bias = signBias[1];
        f.alt_ref_frame = uint32_t(refIndex[2]);
        f.alt_ref_frame_sign_bias = signBias[2];
        f.lossless_flag = qIndex == 0 && yDc == 0 && uvDc == 0 && uvAc == 0;
        pic.filter_level = uint8_t(level);
        pic.sharpness_level = uint8_t(sharpness);
        pic.log2_tile_rows = uint8_t(log2Rows);
        pic.log2_tile_columns = uint8_t(log2Cols);
        pic.frame_header_length_in_bytes = uint8_t(header);
        pic.first_partition_size = uint16_t(compressed);
        memcpy(pic.mb_segment_tree_probs, treeProbs, 7);
        if (segTemporal) memcpy(pic.segment_pred_probs, predProbs, 3);
        else memset(pic.segment_pred_probs, 255, 3);
        pic.profile = uint8_t(profile);
        pic.bit_depth = uint8_t(bits);

        VASliceParameterBufferVP9 slice{};
        slice.slice_data_size = uint32_t(size);
        slice.slice_data_flag = VA_SLICE_DATA_FLAG_ALL;
        const int depth = bits > 8 ? 1 : 0;
        for (int i = 0; i < 8; ++i) {
            const Feature& s = feature[segEnabled ? i : 0];
            int qyac = qIndex;
            if (segEnabled && s.q) qyac = clip(segAbsolute ? s.qValue : qIndex + s.qValue, 255);
            VASegmentParameterVP9& out = slice.seg_param[i];
            out.segment_flags.fields.segment_reference_enabled = feature[i].ref;
            out.segment_flags.fields.segment_reference = uint16_t(feature[i].refValue);
            out.segment_flags.fields.segment_reference_skipped = feature[i].skip;
            out.luma_dc_quant_scale = kDcQ[depth][clip(qyac + yDc, 255)];
            out.luma_ac_quant_scale = kAcQ[depth][clip(qyac, 255)];
            out.chroma_dc_quant_scale = kDcQ[depth][clip(qyac + uvDc, 255)];
            out.chroma_ac_quant_scale = kAcQ[depth][clip(qyac + uvAc, 255)];
            const int shift = level >= 32;
            int lf = level;
            if (segEnabled && s.lf) lf = clip(segAbsolute ? s.lfValue : level + s.lfValue, 63);
            for (int r = 0; r < 4; ++r) {
                for (int m = 0; m < 2; ++m) {
                    const int delta = r == 0 ? refDelta[0] : refDelta[r] + modeDelta[m];
                    out.filter_level[r][m] = uint8_t(deltas ? clip(lf + delta * (1 << shift), 63) : lf);
                }
            }
        }
        if (!session.param(VAPictureParameterBufferType, &pic, sizeof(pic)) || !session.slice(&slice, sizeof(slice), data, size) || !session.submit(target)) return false;
        for (int i = 0; i < 8; ++i) {
            if (!(refresh & (1 << i))) continue;
            refs[i] = target;
            refWidth[i] = width;
            refHeight[i] = height;
        }
        return invisible || session.hand(target, width, height, stamp, color, sink);
    }

    bool decode(const uint8_t* data, size_t size, int64_t stamp, const PictureSink& sink) override {
        if (!size) return false;
        const uint8_t marker = data[size - 1];
        if ((marker & 0xE0) == 0xC0) {
            const int count = (marker & 7) + 1, bytes = ((marker >> 3) & 3) + 1;
            const size_t index = 2 + size_t(bytes) * size_t(count);
            if (size >= index && data[size - index] == marker) {
                const uint8_t* at = data + size - index + 1;
                size_t offset = 0;
                bool ok = true;
                for (int i = 0; i < count; ++i, at += bytes) {
                    size_t length = 0;
                    for (int k = 0; k < bytes; ++k) length |= size_t(at[k]) << (8 * k);
                    if (length > size - index - offset) return false;
                    if (length) ok = frame(data + offset, length, stamp, sink) && ok;
                    offset += length;
                }
                return ok;
            }
        }
        return frame(data, size, stamp, sink);
    }
};

}

std::unique_ptr<SoftDecoder> openVaapiVp9(const StreamShape& shape, int width, int height, std::string* error) {
    if (shape.chromaX != 1 || shape.chromaY != 1 || shape.bits > 10) {
        if (error) *error = "only 4:2:0 VP9 at 8 or 10 bits is decoded on the GPU";
        return nullptr;
    }
    const bool deep = shape.bits > 8;
    if (d3d12Wrong(deep, width, height)) {
        if (error) *error = "the GPU driver decodes this VP9 frame size wrongly or crashes on it";
        return nullptr;
    }
    const VAProfile profile = deep ? VAProfileVP9Profile2 : VAProfileVP9Profile0;
    auto decoder = std::make_unique<Vp9Decoder>();
    if (!vaDecodes(profile) || !decoder->session.open(profile, width, height, deep, 10)) {
        if (error) *error = "the GPU does not decode this VP9 profile";
        return nullptr;
    }
    return decoder;
}

}
#endif
