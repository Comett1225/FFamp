#pragma once
#include "absent/audio.h"
#include "absent/tools.h"

namespace absent {

struct Ticker {
    const Progress& progress;
    bool cancelled = false;
    double last = -1;
    bool operator()(double fraction);
    bool stopped(std::string* error) const;
};

inline const char* kCannotHold = "the output file type cannot hold this codec (WebM holds only VP8, VP9 or AV1 pictures and Opus or Vorbis sound, Ogg one Opus track)";

bool endsWith(const std::string& text, const char* tail);
bool matroskaPath(const std::string& path);
TrackSpec specFor(const Track& track);
double zeroSeconds(const Reader& reader);

struct Piece {
    Reader* reader = nullptr;
    const Track* track = nullptr;
    std::vector<uint32_t> samples;
    TrackSpec spec;
    double start = 0;
    double delay = 0;
    int64_t base = 0;
    int out = -1;
    size_t next = 0;
    std::vector<uint8_t> buffer;

    bool done() const { return next >= samples.size(); }
    double time() const;
    bool step(Writer& writer);
};

bool planPiece(Reader& reader, const Track& track, double zero, double from, double to, bool whole, Piece& piece);
void settle(std::vector<Piece>& pieces, double origin, bool matroska, Writer& writer, bool fromZero = true);
bool copyPieces(std::vector<Piece>& pieces, Writer& writer, Ticker& ticker, std::string* error);

struct SoundPlan {
    int rate = 0;
    int channels = 0;
    double start = 0;
};

bool soundPlan(Reader& reader, bool mp3, SoundPlan& plan, std::string* error);

class Scaler {
public:
    Scaler(int width, int height);
    ~Scaler();
    void* fit(const Frame& frame);

private:
    struct Impl;
    std::unique_ptr<Impl> impl;
};

void releaseBuffer(void* buffer);

}
