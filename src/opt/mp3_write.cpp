#ifdef ABSENT_LAME
#include "../writers.h"
#include <lame/lame.h>
#include <cstdio>

namespace absent {

namespace {

class LameFile : public Mp3File {
public:
    std::string path;
    FILE* file = nullptr;
    lame_global_flags* lame = nullptr;
    int channels = 0;
    bool failed = false;
    std::vector<uint8_t> coded;
    std::vector<float> left;
    std::vector<float> right;

    ~LameFile() override {
        if (lame) lame_close(lame);
        if (file) {
            fclose(file);
            remove(path.c_str());
        }
    }

    bool store(int size) {
        if (size < 0) failed = true;
        else if (size > 0 && fwrite(coded.data(), 1, size_t(size), file) != size_t(size)) failed = true;
        return !failed;
    }

    bool write(const float* pcm, size_t frames) override {
        const size_t part = 1 << 15;
        for (size_t at = 0; at < frames && !failed; at += part) {
            size_t count = std::min(part, frames - at);
            left.resize(count);
            right.resize(count);
            for (size_t i = 0; i < count; ++i) {
                left[i] = pcm[(at + i) * size_t(channels)];
                right[i] = channels > 1 ? pcm[(at + i) * size_t(channels) + 1] : left[i];
            }
            coded.resize(count * 5 / 4 + 7200);
            store(lame_encode_buffer_ieee_float(lame, left.data(), right.data(), int(count), coded.data(), int(coded.size())));
        }
        return !failed;
    }

    bool finish() override {
        coded.resize(7200);
        store(lame_encode_flush(lame, coded.data(), int(coded.size())));
        coded.resize(2880);
        size_t tag = lame_get_lametag_frame(lame, coded.data(), coded.size());
        if (tag > 0 && tag <= coded.size()) failed = failed || fseeko(file, 0, SEEK_SET) != 0 || fwrite(coded.data(), 1, tag, file) != tag;
        failed = fclose(file) != 0 || failed;
        file = nullptr;
        if (failed) remove(path.c_str());
        return !failed;
    }
};

}

std::unique_ptr<Mp3File> Mp3File::open(const std::string& path, int rate, int channels, int bitrate, std::string* error) {
    auto fail = [&](const char* text) {
        if (error) *error = text;
        return nullptr;
    };
    if (channels > 2) return fail("MP3 holds one or two channels");
    auto out = std::make_unique<LameFile>();
    out->path = path;
    out->channels = channels;
    out->lame = lame_init();
    if (!out->lame) return fail("the MP3 encoder did not start");
    lame_set_in_samplerate(out->lame, rate);
    lame_set_num_channels(out->lame, channels);
    lame_set_mode(out->lame, channels == 1 ? MONO : JOINT_STEREO);
    lame_set_VBR(out->lame, vbr_off);
    lame_set_brate(out->lame, bitrate / 1000);
    lame_set_quality(out->lame, 5);
    lame_set_bWriteVbrTag(out->lame, 1);
    lame_set_write_id3tag_automatic(out->lame, 0);
    if (lame_init_params(out->lame) < 0) return fail("the MP3 encoder does not take this rate or bitrate");
    out->file = fopen(path.c_str(), "wb");
    if (!out->file) return fail("cannot create output file");
    return out;
}

}

#endif
