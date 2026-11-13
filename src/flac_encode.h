#pragma once
#include <cstddef>
#include <cstdint>
#include <functional>
#include <memory>
#include <vector>

namespace absent {

struct Md5 {
    uint32_t state[4] = {0x67452301, 0xEFCDAB89, 0x98BADCFE, 0x10325476};
    uint64_t length = 0;
    uint8_t block[64] = {};
    void add(const uint8_t* data, size_t size);
    void finish(uint8_t out[16]);
};

using FlacSink = std::function<void(const uint8_t* data, size_t size, uint32_t frames)>;

class FlacEncoder {
public:
    static constexpr int kBlock = 4608;
    FlacEncoder(int rate, int channels, int bits);
    ~FlacEncoder();
    void encode(const int32_t* interleaved, size_t frames, const FlacSink& sink);
    void finish(const FlacSink& sink);
    std::vector<uint8_t> streamInfo() const;

private:
    struct Impl;
    std::unique_ptr<Impl> impl;
};

}
