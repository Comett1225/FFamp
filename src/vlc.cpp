#include "vlc.h"
#include <algorithm>

namespace absent {

size_t Vlc::fill(const std::vector<Code>& codes, int bits) {
    const size_t base = table.size();
    table.resize(base + (size_t(1) << bits));
    std::vector<std::pair<uint32_t, Code>> deeper;
    for (const Code& c : codes) {
        if (c.length <= bits) {
            const uint32_t first = c.bits << (bits - c.length);
            for (uint32_t i = 0; i < (1u << (bits - c.length)); ++i) table[base + first + i] = {c.symbol, int8_t(c.length)};
        } else {
            const int rest = c.length - bits;
            deeper.push_back({c.bits >> rest, {c.bits & ((1u << rest) - 1), rest, c.symbol}});
        }
    }
    std::stable_sort(deeper.begin(), deeper.end(), [](const auto& a, const auto& b) { return a.first < b.first; });
    for (size_t i = 0; i < deeper.size();) {
        size_t j = i;
        int most = 0;
        std::vector<Code> group;
        for (; j < deeper.size() && deeper[j].first == deeper[i].first; ++j) {
            group.push_back(deeper[j].second);
            most = std::max(most, deeper[j].second.length);
        }
        const int sub = std::min(most, root);
        const size_t at = fill(group, sub);
        table[base + deeper[i].first] = {int32_t(at), int8_t(-sub)};
        i = j;
    }
    return base;
}

bool Vlc::build(int rootBits, size_t count, const uint8_t* lengths, const uint32_t* codes, const int32_t* symbols) {
    std::vector<Code> list;
    for (size_t i = 0; i < count; ++i) {
        const int length = lengths[i];
        if (!length) continue;
        if (length > 32 || (length < 32 && (codes[i] >> length))) return false;
        list.push_back({codes[i], length, symbols ? symbols[i] : int32_t(i)});
    }
    table.clear();
    root = rootBits;
    fill(list, rootBits);
    return true;
}

bool Vlc::buildInOrder(int rootBits, size_t count, const uint8_t* lengths, const uint8_t* symbols, int offset) {
    std::vector<uint32_t> codes(count);
    std::vector<int32_t> values(count);
    uint64_t code = 0;
    for (size_t i = 0; i < count; ++i) {
        const int length = lengths[i];
        if (length < 1 || length > 32 || (code & ((uint64_t(1) << (32 - length)) - 1))) return false;
        codes[i] = uint32_t(code >> (32 - length));
        values[i] = int32_t(symbols[i]) + offset;
        code += uint64_t(1) << (32 - length);
        if (code > (uint64_t(1) << 32)) return false;
    }
    return build(rootBits, count, lengths, codes.data(), values.data());
}

}
