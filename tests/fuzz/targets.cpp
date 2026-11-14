#include "absent/audio.h"
#include "absent/media.h"
#include "absent/probe.h"
#include "audio_codec.h"
#include "soft_video.h"
#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <string>
#include <unistd.h>
#include <vector>

namespace {

using namespace absent;

const size_t kPacketLimit = 8u << 20;
const uint32_t kPacketsPerTrack = 4000;
const int64_t kPictureLimit = 4096 * 2304;
uint64_t picturesSeen = 0;

const std::string& scratch() {
    static const std::string path = [] {
        const char* folder = getenv("TMPDIR");
        return std::string(folder ? folder : "/tmp") + "/absent_fuzz_" + std::to_string(getpid());
    }();
    return path;
}

bool store(const uint8_t* data, size_t size) {
    FILE* file = fopen(scratch().c_str(), "wb");
    if (!file) return false;
    bool whole = fwrite(data, 1, size, file) == size;
    return fclose(file) == 0 && whole;
}

std::unique_ptr<Reader> opened(const uint8_t* data, size_t size) {
    return store(data, size) ? Reader::open(scratch()) : nullptr;
}

void open(const uint8_t* data, size_t size) {
    std::string error;
    if (!store(data, size)) return;
    auto reader = Reader::open(scratch(), &error);
    if (!reader) return;
    reader->format();
    reader->tags();
    reader->statedSeconds();
    for (const Track& t : reader->tracks()) {
        t.seconds();
        t.rate();
    }
}

void packets(const uint8_t* data, size_t size) {
    auto reader = opened(data, size);
    if (!reader) return;
    std::vector<uint8_t> packet;
    for (const Track& t : reader->tracks()) {
        uint32_t count = uint32_t(std::min<size_t>(t.samples.size(), kPacketsPerTrack));
        for (uint32_t n = 0; n < count; ++n) {
            uint32_t index = n < count / 2 ? n : uint32_t(t.samples.size()) - (count - n);
            if (t.samples[index].size > kPacketLimit) continue;
            packet.resize(t.samples[index].size);
            reader->read(t, index, packet.data());
        }
        reader->read(t, uint32_t(t.samples.size()), packet.data());
    }
}

void probe(const uint8_t* data, size_t size) {
    if (!store(data, size)) return;
    std::string error;
    probeJson(scratch(), true, &error);
    probeJson(scratch(), false, nullptr);
}

void audio(const uint8_t* data, size_t size) {
    auto reader = opened(data, size);
    if (!reader) return;
    for (const Track& t : reader->tracks()) {
        if (t.kind != Kind::Audio) continue;
        std::string error;
        auto decoder = AudioDecoder::open(*reader, t, &error);
        if (!decoder) continue;
        int channels = decoder->channels(), rate = decoder->sampleRate();
        if (channels < 1 || channels > 64 || rate < 1) continue;
        const size_t chunk = 4096;
        std::vector<float> pcm(chunk * size_t(channels));
        int64_t length = decoder->length();
        int64_t end = std::min<int64_t>(length, int64_t(rate) * 3);
        for (int64_t at = 0; at < end; at += int64_t(chunk)) {
            if (!decoder->read(at, size_t(std::min<int64_t>(int64_t(chunk), end - at)), pcm.data())) break;
        }
        decoder->read(length / 2, 1000, pcm.data());
        decoder->read(std::max<int64_t>(0, length - 500), 1000, pcm.data());
        decoder->read(-300, 1000, pcm.data());
        decoder->read(length / 3, 1, pcm.data());
    }
    for (int rate : {48000, 44100}) {
        std::string error;
        auto mixed = PcmReader::open(scratch(), rate, 2, &error);
        if (!mixed) continue;
        std::vector<float> pcm(2048 * 2);
        int64_t length = mixed->length();
        mixed->read(0, 2048, pcm.data());
        mixed->read(length / 2, 2048, pcm.data());
        mixed->read(std::max<int64_t>(0, length - 100), 2048, pcm.data());
    }
}

void video(const uint8_t* data, size_t size) {
    auto reader = opened(data, size);
    if (!reader) return;
    for (const Track& t : reader->tracks()) {
        if (t.kind != Kind::Video || t.samples.empty()) continue;
        if (t.width < 1 || t.height < 1 || int64_t(t.width) * t.height > kPictureLimit) continue;
        bool own = t.codec == fourcc("av01") || t.codec == fourcc("vp09") || t.codec == fourcc("vp08");
        for (int software = 0; software <= (own ? 1 : 0); ++software) {
            std::string error;
            auto decoder = VideoDecoder::open(*reader, t, &error, software == 1);
            if (!decoder) continue;
            decoder->backend();
            uint32_t count = decoder->frames();
            std::vector<uint8_t> pixels;
            for (uint32_t n = 0; n < std::min<uint32_t>(count, 4); ++n) {
                Frame frame = decoder->frame(n);
                if (frame) (n & 1) ? copyBGRA(frame, pixels) : copyYuv(frame, pixels);
            }
            if (count > 4) {
                Frame frame = decoder->frame(count - 1);
                if (frame) copyI420(frame, pixels);
                decoder->frame(count / 2);
            }
            decoder->frame(count);
        }
    }
}

void mpeg4(const uint8_t* data, size_t size) {
    if (size < 2) return;
    static const int sizes[4][2] = {{160, 120}, {176, 144}, {161, 121}, {32, 32}};
    const int* shape = sizes[(data[0] ^ data[1]) & 3];
    auto decoder = openXvid({}, shape[0], shape[1]);
    if (!decoder) return;
    PictureSink look = [&](const Picture& p) {
        for (int i = 0; i < 3; ++i) {
            const int rows = i ? p.chromaHeight : p.height, columns = i ? p.chromaWidth : p.width;
            picturesSeen += p.plane[i][0] + p.plane[i][size_t(rows - 1) * p.stride[i] + size_t(columns - 1)];
        }
    };
    size_t start = 2;
    int64_t stamp = 0;
    for (size_t at = start + 1; at + 4 <= size; ++at) {
        if (data[at] || data[at + 1] || data[at + 2] != 1 || data[at + 3] != 0xB6) continue;
        decoder->decode(data + start, at - start, stamp++, look);
        start = at;
        if ((stamp & 15) == 9) decoder->reset();
    }
    decoder->decode(data + start, size - start, stamp, look);
    decoder->drain(look);
}

void vc1(const uint8_t* data, size_t size) {
    if (size < 8) return;
    static const int sizes[4][2] = {{176, 144}, {64, 48}, {33, 17}, {320, 240}};
    const int* shape = sizes[(data[0] >> 1) & 3];
    const bool advanced = data[0] & 1;
    std::vector<uint8_t> config(40, 0);
    size_t start = 1;
    if (advanced) {
        size_t end = start;
        while (end + 4 <= size && end < 256 && !(data[end] == 0 && data[end + 1] == 0 && data[end + 2] == 1 && data[end + 3] == 0x0D)) ++end;
        config.insert(config.end(), data + start, data + end);
        start = end;
    } else {
        config.insert(config.end(), data + 1, data + 5);
        start = 5;
    }
    auto decoder = openVc1(advanced ? fourcc("WVC1") : fourcc("WMV3"), config, shape[0], shape[1], nullptr);
    if (!decoder) return;
    PictureSink look = [&](const Picture& p) {
        for (int i = 0; i < 3; ++i) {
            const int rows = i ? p.chromaHeight : p.height, columns = i ? p.chromaWidth : p.width;
            picturesSeen += p.plane[i][0] + p.plane[i][size_t(rows - 1) * p.stride[i] + size_t(columns - 1)];
        }
    };
    int64_t stamp = 0;
    while (start + 2 <= size) {
        const size_t length = std::min<size_t>(size - start - 2, size_t(data[start]) << 8 | data[start + 1]);
        decoder->decode(data + start + 2, length, stamp++, look);
        start += 2 + length;
        if ((stamp & 15) == 9) decoder->reset();
    }
    decoder->drain(look);
}

void wma(const uint8_t* data, size_t size) {
    if (size < 18) return;
    const size_t head = std::min<size_t>(size, 18 + size_t(data[16] | data[17] << 8));
    Track t;
    t.kind = Kind::Audio;
    const int tag = data[0] | data[1] << 8;
    t.codec = tag == 0x160 ? fourcc("wma1") : tag == 0x162 ? fourcc("wmap") : tag == 0x163 ? fourcc("wmal") : tag == 0x0A ? fourcc("wmas") : fourcc("wma2");
    t.configType = fourcc("wfex");
    t.config.assign(data, data + head);
    auto decoder = t.codec == fourcc("wmap") ? openWmaProAudio(t, nullptr) : t.codec == fourcc("wmal") ? openWmaLosslessAudio(t, nullptr) : t.codec == fourcc("wmas") ? openWmaVoiceAudio(t, nullptr) : openWmaAudio(t, nullptr);
    if (!decoder) return;
    const size_t align = size_t(data[12] | data[13] << 8);
    std::vector<uint8_t> packet, state;
    std::vector<float> out;
    size_t turn = 0;
    for (size_t at = head; at < size; ++turn) {
        const size_t take = std::min(size - at, align * (1 + turn % 3));
        packet.assign(data + at, data + at + take);
        at += take;
        if (turn % 7 == 3) decoder->skim(packet);
        else decoder->feed(packet, out);
        if (turn % 11 == 5) {
            decoder->save(state);
            decoder->reset();
            decoder->load(state);
        }
        if (out.size() > (1u << 20)) out.clear();
    }
    decoder->finish(out);
}

void resample(const uint8_t* data, size_t size) {
    if (size < 8) return;
    static const int rates[] = {4000, 6000, 8000, 11025, 16000, 22050, 32000, 44100, 48000, 88200, 96000, 176400, 192000, 384000, 47999, 48001};
    int inRate = (data[0] & 0x80) ? 4000 + ((data[0] & 0x7F) << 8 | data[1]) * 12 : rates[data[0] & 15];
    int outRate = (data[2] & 0x80) ? 4000 + ((data[2] & 0x7F) << 8 | data[3]) * 12 : rates[data[2] & 15];
    int inChannels = 1 + (data[4] & 7), outChannels = 1 + (data[5] & 7);
    bool wild = data[6] & 1, restart = data[6] & 2;
    size_t budget = 200000;
    Resampler resampler(inRate, inChannels, outRate, outChannels);
    mixPeak(inChannels, outChannels);
    std::vector<float> in, out(4096 * size_t(outChannels));
    size_t at = 8;
    auto drain = [&](size_t step) {
        while (budget) {
            size_t made = resampler.pull(out.data(), std::clamp<size_t>(step, 1, 4096));
            if (!made) break;
            budget -= std::min(budget, made);
        }
    };
    while (at + 2 <= size && budget) {
        size_t frames = size_t(data[at]) << 4 | (data[at + 1] >> 4);
        size_t step = 1 + (size_t(data[at + 1] & 15) << 8);
        at += 2;
        in.assign(frames * size_t(inChannels), 0.0f);
        for (size_t i = 0; i < in.size() && at < size; ++i, ++at) {
            if (wild && at + 4 <= size) {
                memcpy(&in[i], data + at, 4);
                at += 3;
            } else {
                in[i] = float(int8_t(data[at])) / 128.0f;
            }
        }
        resampler.push(in.data(), frames);
        drain(step);
        if (restart && (frames & 31) == 7) resampler.reset();
    }
    resampler.finish();
    drain(4096);
}

}

extern "C" int LLVMFuzzerTestOneInput(const uint8_t* data, size_t size) {
    FUZZ_TARGET(data, size);
    return 0;
}
