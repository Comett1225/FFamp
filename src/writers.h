#pragma once
#include "absent/media.h"
#include "bytes.h"
#include <algorithm>
#include <cstdio>
#include <deque>

namespace absent {

std::unique_ptr<Writer> openOggWriter(const std::string& path, std::string* error);

class Mp3File {
public:
    static std::unique_ptr<Mp3File> open(const std::string& path, int rate, int channels, int bitrate, std::string* error);
    virtual ~Mp3File() = default;
    virtual bool write(const float* pcm, size_t frames) = 0;
    virtual bool finish() = 0;
};

struct Queued {
    int track = 0;
    std::vector<uint8_t> data;
    int64_t pts = 0;
    int64_t dts = 0;
    uint32_t duration = 0;
    bool sync = false;
    double time = 0;
};

class Interleaver {
public:
    std::function<bool(const Queued&)> sink;

    void add() { queues.emplace_back(); }
    bool push(Queued packet) {
        if (packet.track < 0 || size_t(packet.track) >= queues.size()) return false;
        bytes += packet.data.size();
        latest = std::max(latest, packet.time);
        queues[size_t(packet.track)].push_back(std::move(packet));
        return emit(false);
    }
    bool drain() { return emit(true); }

private:
    bool emit(bool all) {
        for (;;) {
            std::deque<Queued>* first = nullptr;
            bool full = true;
            for (auto& queue : queues) {
                if (queue.empty()) full = false;
                else if (!first || queue.front().time < first->front().time) first = &queue;
            }
            if (!first || !(all || full || latest - first->front().time > 10 || bytes > (64u << 20))) return true;
            Queued packet = std::move(first->front());
            first->pop_front();
            bytes -= packet.data.size();
            if (!sink(packet)) return false;
        }
    }

    std::vector<std::deque<Queued>> queues;
    size_t bytes = 0;
    double latest = -1e300;
};

bool placeInFront(FILE*& data, uint64_t size, const std::vector<const Bytes*>& front, const std::string& temp, const std::string& path);

std::vector<uint8_t> dolbySpecific(uint32_t codec, const uint8_t* packet, size_t size);

}
