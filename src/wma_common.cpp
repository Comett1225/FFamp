#include "wma.h"
#include <mutex>

namespace absent::wma {

int frameLengthBits(int rate, int version, unsigned flags) {
    int bits = rate <= 16000 ? 9 : rate <= 22050 || (rate <= 32000 && version == 1) ? 10 : rate <= 48000 || version < 3 ? 11 : rate <= 96000 ? 12 : 13;
    if (version == 3) {
        const unsigned size = flags & 6;
        bits += size == 2 ? 1 : size == 4 ? -1 : size == 6 ? -2 : 0;
    }
    return bits;
}

uint32_t largeValue(BitReader& in) {
    int bits = 8;
    if (in.bit()) {
        bits += 8;
        if (in.bit()) {
            bits += 8;
            if (in.bit()) bits += 7;
        }
    }
    return in.read(bits);
}

const RunLevel& coefficientTable(int index) {
    static RunLevel tables[6];
    static std::once_flag made;
    std::call_once(made, [] {
        for (int k = 0; k < 6; ++k) {
            const CoefTable& source = kCoefTables[k];
            RunLevel& t = tables[k];
            t.vlc.build(9, size_t(source.count), source.lengths, source.codes);
            t.levels.assign(size_t(source.count), 0.0f);
            t.runs.assign(size_t(source.count), 0);
            int i = 2, level = 1;
            for (int n = 0; n < source.levelCount && i < source.count; ++n, ++level) {
                for (int run = 0; run < source.levels[n] && i < source.count; ++run, ++i) {
                    t.runs[size_t(i)] = uint16_t(run);
                    t.levels[size_t(i)] = float(level);
                }
            }
        }
    });
    return tables[index];
}

bool runLevel(BitReader& in, const RunLevel& table, int version, float* out, int offset, int count, int blockLength, int frameBits, int escapeBits) {
    const unsigned mask = unsigned(blockLength - 1);
    for (; offset < count; ++offset) {
        const int code = table.vlc.read(in);
        if (code > 1) {
            offset += table.runs[size_t(code)];
            const float level = table.levels[size_t(code)];
            out[unsigned(offset) & mask] = in.bit() ? level : -level;
        } else if (code == 1) {
            break;
        } else {
            int level;
            if (!version) {
                level = int(in.read(escapeBits));
                offset += int(in.read(frameBits));
            } else {
                level = int(largeValue(in));
                if (in.bit()) {
                    if (in.bit()) {
                        if (in.bit()) return false;
                        offset += int(in.read(frameBits)) + 4;
                    } else {
                        offset += int(in.read(2)) + 1;
                    }
                }
            }
            const int sign = int(in.read(1)) - 1;
            out[unsigned(offset) & mask] = float((level ^ sign) - sign);
        }
    }
    return offset <= count;
}

}
