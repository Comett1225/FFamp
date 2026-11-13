#pragma once
#include "absent/media.h"

namespace absent {

using Progress = std::function<bool(double)>;

enum class SoundChoice { None, Copy, Aac, Mp3 };

struct CompressJob {
    std::string input;
    std::string output;
    int64_t startUs = 0;
    int64_t endUs = 0;
    int width = 0;
    int height = 0;
    bool keepAspect = false;
    int fpsNumerator = 0;
    int fpsDenominator = 1;
    uint32_t codec = fourcc("avc1");
    int bitrate = 0;
    SoundChoice sound = SoundChoice::Copy;
    int audioBitrate = 128000;
};

bool compress(const CompressJob& job, const Progress& progress, std::string* error = nullptr);

bool trimCopy(const std::string& input, const std::string& output, int64_t startUs, int64_t endUs, const Progress& progress, std::string* error = nullptr);

bool extractAudio(const std::string& input, const std::string& output, SoundChoice how, int bitrate, const Progress& progress, std::string* error = nullptr);
const char* audioCopyExtension(uint32_t codec);

struct ReverseJob {
    std::string input;
    std::string output;
    int64_t startUs = 0;
    int64_t endUs = 0;
    int bitrate = 0;
    int audioBitrate = 128000;
    uint64_t memoryBytes = 512ull << 20;
};

struct ReverseResult {
    int width = 0;
    int height = 0;
    uint32_t frames = 0;
    int64_t durationUs = 0;
    uint32_t spilledGroups = 0;
};

bool reverse(const ReverseJob& job, const Progress& progress, ReverseResult* result = nullptr, std::string* error = nullptr);

struct GifInfo {
    int width = 0;
    int height = 0;
    int videoWidth = 0;
    int videoHeight = 0;
    int fps = 0;
    int64_t durationMs = 0;
    bool transparent = false;
    std::vector<int> delaysMs;
};

int gifDelayMs(double seconds);
bool gifVideoSize(int width, int height, int& videoWidth, int& videoHeight);
bool gifRead(const std::string& path, GifInfo& info, const std::function<bool(uint32_t index, const uint8_t* premultipliedBgra)>& frame, std::string* error = nullptr);
bool gifToVideo(const std::string& input, const std::string& colour, const std::string& matte, GifInfo* info, const Progress& progress, std::string* error = nullptr);

const char* mergeExtension(uint32_t videoCodec, uint32_t audioCodec);
bool merge(const std::string& video, const std::string& audio, const std::string& output, const Progress& progress, std::string* error = nullptr);
bool stripAudio(const std::string& input, const std::string& output, const Progress& progress, std::string* error = nullptr);
bool rewrap(const std::string& input, const std::string& output, const Progress& progress, std::string* error = nullptr);

}
