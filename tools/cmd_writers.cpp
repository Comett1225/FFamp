#include "cli.h"
#include "absent/audio.h"
#include <cstring>

namespace cli {

namespace {

int aencode(const Args& args) {
    if (args.plain.empty() || !args.has("pcm") || !args.has("rate") || !args.has("channels") || !args.has("format")) {
        return fail("aencode needs <out> --pcm file|- --rate R --channels C --format wav16|wav24|wavf|flac|flac24|aac|opus|mp3 [--bitrate bits] [--sample f32|s16] [--source-channels N] [--rf64]");
    }
    static const std::map<std::string, AudioFormat> formats = {{"wav16", AudioFormat::Wav16}, {"wav24", AudioFormat::Wav24}, {"wavf", AudioFormat::WavFloat}, {"flac", AudioFormat::Flac},
        {"flac24", AudioFormat::Flac24}, {"aac", AudioFormat::Aac}, {"opus", AudioFormat::Opus}, {"mp3", AudioFormat::Mp3}};
    auto format = formats.find(args.get("format"));
    if (format == formats.end()) return fail("unknown format " + args.get("format"));
    const std::string sample = args.get("sample", "f32");
    if (sample != "f32" && sample != "s16") return fail("unknown sample type " + sample);
    const bool shorts = sample == "s16";
    const int channels = int(args.number("channels", 2));
    if (channels < 1 || channels > 8) return fail("audio layout not supported");
    const int given = int(args.number("source-channels", channels));
    if (given != channels && !(given == 2 && channels == 1) && !(given == 1 && channels == 2)) return fail("only mono and stereo sources can be remixed");
    const std::string source = args.get("pcm");
    FILE* in = source == "-" ? stdin : openFile(source.c_str(), "rb");
    if (!in) return fail("cannot open " + source);
    std::string error;
    auto writer = AudioFileWriter::open(args.plain[0], format->second, int(args.number("rate", 48000)), channels, int(args.number("bitrate", 0)), &error);
    if (!writer) return fail(error);
    if (args.has("rf64")) writer->alwaysRf64();
    const size_t frameBytes = size_t(given) * (shorts ? 2 : 4);
    std::vector<uint8_t> raw(frameBytes * 8192);
    std::vector<float> pcm(size_t(std::max(given, channels)) * 8192);
    size_t kept = 0;
    for (;;) {
        size_t got = fread(raw.data() + kept, 1, raw.size() - kept, in);
        size_t frames = (kept + got) / frameBytes;
        if (shorts) {
            for (size_t i = 0; i < frames * size_t(given); ++i) {
                int16_t value;
                memcpy(&value, raw.data() + i * 2, 2);
                pcm[i] = float(value) / 32768.0f;
            }
        } else {
            memcpy(pcm.data(), raw.data(), frames * frameBytes);
        }
        if (given == 2 && channels == 1) for (size_t i = 0; i < frames; ++i) pcm[i] = (pcm[i * 2] + pcm[i * 2 + 1]) * 0.5f;
        if (given == 1 && channels == 2) for (size_t i = frames; i-- > 0;) pcm[i * 2] = pcm[i * 2 + 1] = pcm[i];
        if (frames && !writer->write(pcm.data(), frames)) return fail("writing the sound failed");
        kept = kept + got - frames * frameBytes;
        memmove(raw.data(), raw.data() + frames * frameBytes, kept);
        if (!got) break;
    }
    if (in != stdin) fclose(in);
    if (!writer->finish(&error)) return fail(error);
    return 0;
}

struct Stored {
    TrackSpec spec;
    std::vector<Packet> packets;
    int out = -1;
    size_t next = 0;
    double time() const { return double(packets[next].dts) / spec.timescale; }
};

struct Cursor {
    const std::vector<uint8_t>& d;
    size_t at = 0;
    bool ok = true;
    uint64_t number(int bytes) {
        if (!ok || d.size() - at < size_t(bytes)) {
            ok = false;
            return 0;
        }
        uint64_t value = 0;
        for (int i = 0; i < bytes; ++i) value = (value << 8) | d[at++];
        return value;
    }
    std::vector<uint8_t> bytes(size_t count) {
        if (!ok || d.size() - at < count) {
            ok = false;
            return {};
        }
        at += count;
        return std::vector<uint8_t>(d.begin() + long(at - count), d.begin() + long(at));
    }
};

bool load(const std::string& path, std::vector<Stored>& tracks) {
    FILE* file = openFile(path.c_str(), "rb");
    if (!file) return false;
    std::vector<uint8_t> all;
    uint8_t block[65536];
    for (size_t got; (got = fread(block, 1, sizeof(block), file)) > 0;) all.insert(all.end(), block, block + got);
    fclose(file);
    Cursor c{all};
    if (c.number(4) != fourcc("APKT")) return false;
    uint64_t count = c.number(4);
    for (uint64_t t = 0; t < count && c.ok; ++t) {
        Stored s;
        s.spec.kind = c.number(1) ? Kind::Audio : Kind::Video;
        s.spec.codec = uint32_t(c.number(4));
        s.spec.configType = uint32_t(c.number(4));
        s.spec.timescale = uint32_t(c.number(4));
        s.spec.width = int(c.number(2));
        s.spec.height = int(c.number(2));
        s.spec.sampleRate = int(c.number(4));
        s.spec.channels = int(c.number(2));
        s.spec.bits = int(c.number(1));
        uint64_t flags = c.number(1);
        s.spec.floating = flags & 1;
        s.spec.bigEndian = flags & 2;
        s.spec.skip = int64_t(c.number(8));
        s.spec.length = int64_t(c.number(8));
        s.spec.color.primaries = int(c.number(1));
        s.spec.color.transfer = int(c.number(1));
        s.spec.color.matrix = int(c.number(1));
        s.spec.color.full = c.number(1) != 0;
        s.spec.color.known = true;
        s.spec.config = c.bytes(size_t(c.number(4)));
        uint64_t packets = c.number(4);
        for (uint64_t i = 0; i < packets && c.ok; ++i) {
            Packet p;
            p.pts = int64_t(c.number(8));
            p.dts = int64_t(c.number(8));
            p.duration = uint32_t(c.number(4));
            p.sync = c.number(1) != 0;
            p.data = c.bytes(size_t(c.number(4)));
            s.packets.push_back(std::move(p));
        }
        if (!s.spec.timescale) return false;
        tracks.push_back(std::move(s));
    }
    return c.ok;
}

int pack(const Args& args) {
    if (args.plain.size() < 2) return fail("pack needs an output file and one or more packet files");
    std::vector<Stored> tracks;
    for (size_t i = 1; i < args.plain.size(); ++i) if (!load(args.plain[i], tracks)) return fail("cannot read " + args.plain[i]);
    std::string error;
    auto writer = Writer::open(args.plain[0], &error);
    if (!writer) return fail(error);
    for (Stored& s : tracks) if ((s.out = writer->add(s.spec)) < 0) return fail("the output file type cannot hold one of the tracks");
    for (;;) {
        Stored* best = nullptr;
        for (Stored& s : tracks) if (s.out >= 0 && s.next < s.packets.size() && (!best || s.time() < best->time())) best = &s;
        if (!best) break;
        const Packet& p = best->packets[best->next++];
        if (!writer->write(best->out, p.data.data(), p.data.size(), p.pts, p.dts, p.duration, p.sync)) return fail("writing a packet failed");
    }
    if (!writer->finish(&error)) return fail(error);
    return 0;
}

Register aencodeCommand("aencode", aencode);
Register packCommand("pack", pack);

}

}
