#include "tools_common.h"
#include <algorithm>
#include <cmath>
#include <cstdio>
#include <limits>
#include "portable.h"

namespace absent {

bool Ticker::operator()(double fraction) {
    if (cancelled) return false;
    fraction = std::clamp(fraction, 0.0, 1.0);
    if (fraction < last + 0.001 && fraction < 1) return true;
    last = fraction;
    if (progress && !progress(fraction)) cancelled = true;
    return !cancelled;
}

bool Ticker::stopped(std::string* error) const {
    if (cancelled && error) *error = "cancelled";
    return cancelled;
}

bool endsWith(const std::string& text, const char* tail) {
    const std::string end = tail;
    if (text.size() < end.size()) return false;
    for (size_t i = 0; i < end.size(); ++i) if (tolower(uint8_t(text[text.size() - end.size() + i])) != end[i]) return false;
    return true;
}

bool matroskaPath(const std::string& path) { return endsWith(path, ".mkv") || endsWith(path, ".webm") || endsWith(path, ".mka"); }

TrackSpec specFor(const Track& t) {
    TrackSpec spec;
    spec.kind = t.kind;
    spec.codec = t.codec;
    spec.timescale = t.timescale;
    spec.width = t.width;
    spec.height = t.height;
    spec.rotation = t.rotation;
    spec.sampleRate = t.sampleRate;
    spec.channels = t.channels;
    spec.configType = t.configType;
    spec.config = t.config;
    spec.color = t.color;
    spec.bits = t.bits;
    spec.floating = t.floating;
    spec.bigEndian = t.bigEndian;
    return spec;
}

namespace {

const double kForever = std::numeric_limits<double>::infinity();

int64_t firstShown(const Track& t) {
    int64_t first = std::numeric_limits<int64_t>::max();
    for (const Sample& s : t.samples) if (s.pts + int64_t(std::max<uint32_t>(s.duration, 1)) > t.mediaStart) first = std::min(first, s.pts);
    return first == std::numeric_limits<int64_t>::max() ? t.mediaStart : std::max(first, t.mediaStart);
}

double clockOf(const Track& t, int64_t tick) { return double(tick - t.mediaStart + t.delay) / t.timescale; }

int64_t tickAt(const Track& t, double seconds) {
    const double tick = seconds * t.timescale;
    if (!(tick < 9e17)) return std::numeric_limits<int64_t>::max() / 4;
    if (tick < -9e17) return std::numeric_limits<int64_t>::min() / 4;
    return std::llround(tick) - t.delay + t.mediaStart;
}

int64_t endOf(const Sample& s) { return s.pts + int64_t(std::max<uint32_t>(s.duration, 1)); }

int rollBack(uint32_t codec) {
    if (codec == fourcc("mp4a")) return 2;
    if (codec == fourcc("Opus") || codec == fourcc(".mp3")) return 4;
    if (codec == fourcc("vorb")) return 1;
    return 0;
}

}

double zeroSeconds(const Reader& reader) {
    double zero = kForever;
    for (const Track& t : reader.tracks()) {
        if (t.kind == Kind::Other || t.samples.empty() || !t.timescale) continue;
        zero = std::min(zero, clockOf(t, firstShown(t)));
    }
    return zero == kForever ? 0 : zero;
}

double Piece::time() const {
    const Sample& s = track->samples[samples[next]];
    return double(s.dts - track->samples[samples.front()].dts) / track->timescale + delay;
}

bool Piece::step(Writer& writer) {
    const uint32_t index = samples[next];
    const Sample& s = track->samples[index];
    buffer.resize(s.size);
    if (!reader->read(*track, index, buffer.data())) return false;
    ++next;
    return writer.write(out, buffer.data(), s.size, s.pts - base, s.dts - base, s.duration, s.sync);
}

bool planPiece(Reader& reader, const Track& t, double zero, double from, double to, bool whole, Piece& piece) {
    if (!t.timescale || t.samples.empty() || t.samples.size() > 0xFFFFFFF0u) return false;
    piece.reader = &reader;
    piece.track = &t;
    piece.spec = specFor(t);
    piece.samples.clear();
    int64_t natural = std::numeric_limits<int64_t>::min();
    int64_t lowest = std::numeric_limits<int64_t>::max();
    for (const Sample& s : t.samples) {
        natural = std::max(natural, endOf(s));
        lowest = std::min(lowest, s.pts);
    }
    const bool bounded = t.mediaEnd > t.mediaStart && t.mediaEnd < natural;
    const int64_t finish = bounded ? t.mediaEnd : natural;
    const uint32_t count = uint32_t(t.samples.size());
    if (whole) {
        for (uint32_t i = 0; i < count; ++i) piece.samples.push_back(i);
        const int64_t begin = std::max(t.mediaStart, lowest);
        piece.spec.skip = begin - lowest;
        piece.spec.length = bounded ? std::max<int64_t>(finish - begin, 1) : 0;
        piece.base = lowest;
        piece.start = clockOf(t, begin) - zero;
        return true;
    }
    const int64_t low = std::max(tickAt(t, from + zero), t.mediaStart);
    const int64_t high = std::min(tickAt(t, to + zero), finish);
    if (high <= low) return false;
    if (t.kind == Kind::Video) {
        uint32_t key = 0;
        bool found = false;
        for (uint32_t index : t.syncs) {
            if (index >= count) continue;
            const int64_t pts = t.samples[index].pts;
            if (pts <= low ? (!found || pts > t.samples[key].pts || t.samples[key].pts > low) : !found) {
                key = index;
                found = true;
            }
        }
        const int64_t keyTime = t.samples[key].pts;
        if (keyTime >= high) return false;
        uint32_t last = key;
        for (uint32_t i = key; i < count; ++i) {
            if (i > key && t.samples[i].sync && t.samples[i].pts >= high) break;
            if (t.samples[i].pts >= keyTime && t.samples[i].pts < high) last = i;
        }
        int64_t reach = keyTime;
        for (uint32_t i = key; i <= last; ++i) {
            if (t.samples[i].pts < keyTime) continue;
            piece.samples.push_back(i);
            reach = std::max(reach, endOf(t.samples[i]));
        }
        piece.spec.skip = 0;
        piece.spec.length = reach > high ? high - keyTime : 0;
        piece.base = keyTime;
        piece.start = clockOf(t, keyTime) - zero;
        return true;
    }
    uint32_t first = count, last = 0;
    for (uint32_t i = 0; i < count; ++i) {
        if (endOf(t.samples[i]) <= low || t.samples[i].pts >= high) continue;
        first = std::min(first, i);
        last = std::max(last, i);
    }
    if (first == count) return false;
    const int64_t begin = std::max(low, t.samples[first].pts);
    first -= std::min<uint32_t>(first, uint32_t(rollBack(t.codec)));
    for (uint32_t i = first; i <= last; ++i) piece.samples.push_back(i);
    piece.base = t.samples[first].pts;
    piece.spec.skip = begin - piece.base;
    piece.spec.length = std::max<int64_t>(std::min(high, endOf(t.samples[last])) - begin, 1);
    piece.start = clockOf(t, begin) - zero;
    return true;
}

void settle(std::vector<Piece>& pieces, double origin, bool matroska, Writer& writer, bool fromZero) {
    double least = kForever;
    for (Piece& p : pieces) {
        p.delay = p.start - origin;
        if (matroska && p.spec.kind == Kind::Video && p.spec.skip > 0) {
            p.delay -= double(p.spec.skip) / p.spec.timescale;
            p.spec.skip = 0;
        }
        least = std::min(least, p.delay);
    }
    if (!fromZero) least = std::min(least, 0.0);
    for (Piece& p : pieces) {
        p.delay -= least;
        p.spec.delay = std::llround(p.delay * p.spec.timescale);
        p.out = writer.add(p.spec);
    }
}

bool copyPieces(std::vector<Piece>& pieces, Writer& writer, Ticker& ticker, std::string* error) {
    size_t total = 0, written = 0;
    for (const Piece& p : pieces) total += p.samples.size();
    for (;;) {
        Piece* best = nullptr;
        for (Piece& p : pieces) if (!p.done() && (!best || p.time() < best->time())) best = &p;
        if (!best) break;
        if (!best->step(writer)) {
            if (error) *error = "copying a packet failed";
            return false;
        }
        if ((++written & 31) == 0 && !ticker(double(written) / double(total) * 0.99)) break;
    }
    return !ticker.stopped(error);
}

namespace {

struct Source {
    std::unique_ptr<Reader> reader;
    double zero = 0;
};

bool openSource(const std::string& path, Source& source, std::string* error) {
    source.reader = Reader::open(path, error);
    if (!source.reader) return false;
    source.zero = zeroSeconds(*source.reader);
    return true;
}

bool fail(std::string* error, const char* text) {
    if (error) *error = text;
    return false;
}

bool writeOut(std::vector<Piece>& pieces, double origin, const std::string& output, const Reader& from, const Progress& progress, std::string* error) {
    if (pieces.empty()) return fail(error, "nothing to copy");
    auto writer = Writer::open(output, error);
    if (!writer) return false;
    writer->tags(from.tags());
    settle(pieces, origin, matroskaPath(output), *writer);
    if (std::any_of(pieces.begin(), pieces.end(), [](const Piece& p) { return p.out < 0; })) return fail(error, kCannotHold);
    Ticker ticker{progress};
    if (!copyPieces(pieces, *writer, ticker, error)) return false;
    if (!writer->finish(error)) return false;
    if (progress) progress(1.0);
    return true;
}

bool sameFile(const std::string& a, const std::string& b) { return a == b; }

}

bool trimCopy(const std::string& input, const std::string& output, int64_t startUs, int64_t endUs, const Progress& progress, std::string* error) {
    if (sameFile(input, output)) return fail(error, "the output would replace the input");
    Source source;
    if (!openSource(input, source, error)) return false;
    const double from = std::max<int64_t>(startUs, 0) / 1e6;
    const double to = endUs > startUs ? endUs / 1e6 : kForever;
    std::vector<Piece> pieces;
    double origin = from;
    if (const Track* video = source.reader->first(Kind::Video)) {
        Piece piece;
        if (planPiece(*source.reader, *video, source.zero, from, to, false, piece)) {
            origin = piece.start;
            pieces.push_back(std::move(piece));
        }
    }
    if (const Track* audio = source.reader->first(Kind::Audio)) {
        Piece piece;
        if (planPiece(*source.reader, *audio, source.zero, origin, to, false, piece)) pieces.push_back(std::move(piece));
    }
    if (pieces.empty()) return fail(error, "nothing lies between the two times");
    return writeOut(pieces, origin, output, *source.reader, progress, error);
}

const char* audioCopyExtension(uint32_t codec) {
    if (codec == fourcc("mp4a") || codec == fourcc("alac")) return "m4a";
    if (codec == fourcc(".mp3")) return "mp3";
    if (codec == fourcc("Opus")) return "opus";
    return "mka";
}

namespace {

bool copyFrames(Piece& piece, const std::string& output, Ticker& ticker, std::string* error) {
    const std::string temp = output + ".part";
    FILE* file = openFile(temp, "wb");
    if (!file) return fail(error, "cannot create output file");
    bool ok = true;
    std::vector<uint8_t> packet;
    for (size_t i = 0; ok && i < piece.samples.size(); ++i) {
        const Sample& s = piece.track->samples[piece.samples[i]];
        packet.resize(s.size);
        ok = piece.reader->read(*piece.track, piece.samples[i], packet.data()) && fwrite(packet.data(), 1, packet.size(), file) == packet.size();
        if ((i & 31) == 0 && !ticker(double(i) / double(piece.samples.size()) * 0.99)) break;
    }
    ok = fclose(file) == 0 && ok && !ticker.cancelled;
    if (ok) ok = renameFile(temp, output);
    if (!ok) {
        removeFile(temp);
        if (!ticker.stopped(error) && error) *error = "writing the output file failed";
    }
    return ok;
}

int nearestRate(int rate, std::initializer_list<int> allowed) {
    int best = 0;
    for (int r : allowed) if (r >= rate && (!best || r < best)) best = r;
    return best ? best : 48000;
}

}

bool soundPlan(Reader& reader, bool mp3, SoundPlan& plan, std::string* error) {
    const Track* track = reader.first(Kind::Audio);
    if (!track) return fail(error, "no audio track");
    auto decoder = AudioDecoder::open(reader, *track, error);
    if (!decoder) return false;
    plan.channels = std::clamp(decoder->channels(), 1, 2);
    plan.rate = mp3 ? nearestRate(decoder->sampleRate(), {8000, 11025, 12000, 16000, 22050, 24000, 32000, 44100, 48000})
                    : nearestRate(decoder->sampleRate(), {8000, 11025, 12000, 16000, 22050, 24000, 32000, 44100, 48000, 64000, 88200, 96000});
    plan.start = clockOf(*track, firstShown(*track)) - zeroSeconds(reader);
    return true;
}

bool extractAudio(const std::string& input, const std::string& output, SoundChoice how, int bitrate, const Progress& progress, std::string* error) {
    if (sameFile(input, output)) return fail(error, "the output would replace the input");
    Source source;
    if (!openSource(input, source, error)) return false;
    const Track* track = source.reader->first(Kind::Audio);
    if (!track) return fail(error, "no audio track");
    Ticker ticker{progress};
    if (how == SoundChoice::Copy) {
        std::vector<Piece> pieces(1);
        if (!planPiece(*source.reader, *track, source.zero, 0, kForever, true, pieces[0])) return fail(error, "no audio track");
        if (track->codec == fourcc(".mp3") && endsWith(output, ".mp3")) {
            if (!copyFrames(pieces[0], output, ticker, error)) return false;
            if (progress) progress(1.0);
            return true;
        }
        if (!writeOut(pieces, pieces[0].start, output, *source.reader, progress, error)) {
            if (error && *error != "cancelled") *error += std::string("; this sound is copied into a .") + audioCopyExtension(track->codec) + " file";
            return false;
        }
        return true;
    }
    if (how != SoundChoice::Aac && how != SoundChoice::Mp3) return fail(error, "choose copy, AAC or MP3");
    SoundPlan plan;
    if (!soundPlan(*source.reader, how == SoundChoice::Mp3, plan, error)) return false;
    auto pcm = PcmReader::open(input, plan.rate, plan.channels, error);
    if (!pcm) return false;
    auto writer = AudioFileWriter::open(output, how == SoundChoice::Mp3 ? AudioFormat::Mp3 : AudioFormat::Aac, plan.rate, plan.channels, bitrate, error);
    if (!writer) return false;
    const int64_t length = pcm->length();
    std::vector<float> block(size_t(8192) * size_t(plan.channels));
    for (int64_t at = 0; at < length;) {
        const size_t frames = size_t(std::min<int64_t>(8192, length - at));
        if (!pcm->read(at, frames, block.data()) || !writer->write(block.data(), frames)) return fail(error, "encoding the sound failed");
        at += int64_t(frames);
        if (!ticker(double(at) / double(length) * 0.99)) break;
    }
    if (ticker.stopped(error)) return false;
    if (!writer->finish(error)) return false;
    if (progress) progress(1.0);
    return true;
}

const char* mergeExtension(uint32_t video, uint32_t audio) {
    const bool open = video == fourcc("vp08") || video == fourcc("vp09") || video == fourcc("av01") || !video;
    const bool openSound = audio == fourcc("Opus") || audio == fourcc("vorb") || !audio;
    if ((video == fourcc("vp08") || video == fourcc("vp09")) && openSound) return "webm";
    const bool movie = video == fourcc("avc1") || video == fourcc("hvc1") || video == fourcc("av01") || video == fourcc("vp09") || video == fourcc("mp4v") || !video;
    const bool movieSound = audio == fourcc("mp4a") || audio == fourcc("alac") || audio == fourcc("Opus") || audio == fourcc("fLaC") || !audio;
    if (movie && movieSound) return "mp4";
    if (open && openSound) return "webm";
    return "mkv";
}

bool merge(const std::string& video, const std::string& audio, const std::string& output, const Progress& progress, std::string* error) {
    if (sameFile(video, output) || sameFile(audio, output)) return fail(error, "the output would replace an input");
    Source picture, sound;
    if (!openSource(video, picture, error) || !openSource(audio, sound, error)) return false;
    const Track* pictureTrack = picture.reader->first(Kind::Video);
    const Track* soundTrack = sound.reader->first(Kind::Audio);
    if (!pictureTrack) return fail(error, "the first file has no video");
    if (!soundTrack) return fail(error, "the second file has no sound");
    std::vector<Piece> pieces(2);
    if (!planPiece(*picture.reader, *pictureTrack, picture.zero, 0, kForever, true, pieces[0])) return fail(error, "the first file has no video");
    if (!planPiece(*sound.reader, *soundTrack, sound.zero, 0, kForever, true, pieces[1])) return fail(error, "the second file has no sound");
    return writeOut(pieces, 0, output, *picture.reader, progress, error);
}

namespace {

bool copyKinds(const std::string& input, const std::string& output, bool sound, const Progress& progress, std::string* error) {
    if (sameFile(input, output)) return fail(error, "the output would replace the input");
    Source source;
    if (!openSource(input, source, error)) return false;
    std::vector<Piece> pieces;
    for (const Track& t : source.reader->tracks()) {
        if (t.kind == Kind::Other || (t.kind == Kind::Audio && !sound)) continue;
        Piece piece;
        if (planPiece(*source.reader, t, source.zero, 0, kForever, true, piece)) pieces.push_back(std::move(piece));
    }
    if (pieces.empty()) return fail(error, sound ? "nothing to copy" : "no video track");
    return writeOut(pieces, 0, output, *source.reader, progress, error);
}

}

bool stripAudio(const std::string& input, const std::string& output, const Progress& progress, std::string* error) { return copyKinds(input, output, false, progress, error); }

bool rewrap(const std::string& input, const std::string& output, const Progress& progress, std::string* error) { return copyKinds(input, output, true, progress, error); }

}
