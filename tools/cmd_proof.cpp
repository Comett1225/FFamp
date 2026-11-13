#include "cli.h"
#include <cstring>

namespace {

using namespace cli;

struct Glyph {
    char letter;
    uint8_t rows[7];
};

const Glyph kGlyphs[] = {
    {'A', {0b01110, 0b10001, 0b10001, 0b11111, 0b10001, 0b10001, 0b10001}}, {'B', {0b11110, 0b10001, 0b10001, 0b11110, 0b10001, 0b10001, 0b11110}},
    {'C', {0b01110, 0b10001, 0b10000, 0b10000, 0b10000, 0b10001, 0b01110}}, {'D', {0b11110, 0b10001, 0b10001, 0b10001, 0b10001, 0b10001, 0b11110}},
    {'E', {0b11111, 0b10000, 0b10000, 0b11110, 0b10000, 0b10000, 0b11111}}, {'F', {0b11111, 0b10000, 0b10000, 0b11110, 0b10000, 0b10000, 0b10000}},
    {'G', {0b01110, 0b10001, 0b10000, 0b10111, 0b10001, 0b10001, 0b01111}}, {'H', {0b10001, 0b10001, 0b10001, 0b11111, 0b10001, 0b10001, 0b10001}},
    {'I', {0b01110, 0b00100, 0b00100, 0b00100, 0b00100, 0b00100, 0b01110}}, {'J', {0b00111, 0b00010, 0b00010, 0b00010, 0b00010, 0b10010, 0b01100}},
    {'K', {0b10001, 0b10010, 0b10100, 0b11000, 0b10100, 0b10010, 0b10001}}, {'L', {0b10000, 0b10000, 0b10000, 0b10000, 0b10000, 0b10000, 0b11111}},
    {'M', {0b10001, 0b11011, 0b10101, 0b10101, 0b10001, 0b10001, 0b10001}}, {'N', {0b10001, 0b11001, 0b10101, 0b10011, 0b10001, 0b10001, 0b10001}},
    {'O', {0b01110, 0b10001, 0b10001, 0b10001, 0b10001, 0b10001, 0b01110}}, {'P', {0b11110, 0b10001, 0b10001, 0b11110, 0b10000, 0b10000, 0b10000}},
    {'Q', {0b01110, 0b10001, 0b10001, 0b10001, 0b10101, 0b10010, 0b01101}}, {'R', {0b11110, 0b10001, 0b10001, 0b11110, 0b10100, 0b10010, 0b10001}},
    {'S', {0b01111, 0b10000, 0b10000, 0b01110, 0b00001, 0b00001, 0b11110}}, {'T', {0b11111, 0b00100, 0b00100, 0b00100, 0b00100, 0b00100, 0b00100}},
    {'U', {0b10001, 0b10001, 0b10001, 0b10001, 0b10001, 0b10001, 0b01110}}, {'V', {0b10001, 0b10001, 0b10001, 0b10001, 0b10001, 0b01010, 0b00100}},
    {'W', {0b10001, 0b10001, 0b10001, 0b10101, 0b10101, 0b11011, 0b10001}}, {'X', {0b10001, 0b10001, 0b01010, 0b00100, 0b01010, 0b10001, 0b10001}},
    {'Y', {0b10001, 0b10001, 0b01010, 0b00100, 0b00100, 0b00100, 0b00100}}, {'Z', {0b11111, 0b00001, 0b00010, 0b00100, 0b01000, 0b10000, 0b11111}},
    {'0', {0b01110, 0b10001, 0b10011, 0b10101, 0b11001, 0b10001, 0b01110}}, {'1', {0b00100, 0b01100, 0b00100, 0b00100, 0b00100, 0b00100, 0b01110}},
    {'2', {0b01110, 0b10001, 0b00001, 0b00010, 0b00100, 0b01000, 0b11111}}, {'3', {0b11110, 0b00001, 0b00001, 0b01110, 0b00001, 0b00001, 0b11110}},
    {'4', {0b00010, 0b00110, 0b01010, 0b10010, 0b11111, 0b00010, 0b00010}}, {'5', {0b11111, 0b10000, 0b11110, 0b00001, 0b00001, 0b10001, 0b01110}},
    {'6', {0b00110, 0b01000, 0b10000, 0b11110, 0b10001, 0b10001, 0b01110}}, {'7', {0b11111, 0b00001, 0b00010, 0b00100, 0b01000, 0b01000, 0b01000}},
    {'8', {0b01110, 0b10001, 0b10001, 0b01110, 0b10001, 0b10001, 0b01110}}, {'9', {0b01110, 0b10001, 0b10001, 0b01111, 0b00001, 0b00010, 0b01100}},
    {'-', {0, 0, 0, 0b11111, 0, 0, 0}}, {'.', {0, 0, 0, 0, 0, 0b01100, 0b01100}}, {':', {0, 0b01100, 0b01100, 0, 0b01100, 0b01100, 0}},
    {'(', {0b00010, 0b00100, 0b01000, 0b01000, 0b01000, 0b00100, 0b00010}}, {')', {0b01000, 0b00100, 0b00010, 0b00010, 0b00010, 0b00100, 0b01000}},
};

struct Canvas {
    int width = 0;
    int height = 0;
    std::vector<uint8_t> pixels;

    Canvas(int w, int h) : width(w), height(h), pixels(size_t(w) * h + 2 * size_t(w / 2) * (h / 2), 128) {
        std::fill(pixels.begin(), pixels.begin() + long(w) * h, 16);
    }

    void text(const std::string& words, int centre, int top, int scale) {
        int step = 6 * scale;
        int left = centre - int(words.size()) * step / 2;
        for (size_t n = 0; n < words.size(); ++n) {
            char letter = words[n] >= 'a' && words[n] <= 'z' ? char(words[n] - 32) : words[n];
            for (const Glyph& g : kGlyphs) {
                if (g.letter != letter) continue;
                for (int row = 0; row < 7; ++row) {
                    for (int column = 0; column < 5; ++column) {
                        if (!(g.rows[row] & (16 >> column))) continue;
                        for (int y = top + row * scale; y < top + (row + 1) * scale; ++y) {
                            for (int x = left + int(n) * step + column * scale; x < left + int(n) * step + (column + 1) * scale; ++x) {
                                if (x >= 0 && x < width && y >= 0 && y < height) pixels[size_t(y) * width + x] = 235;
                            }
                        }
                    }
                }
            }
        }
    }

    void place(const std::vector<uint8_t>& picture, int w, int h, int left, int top) {
        const uint8_t* from = picture.data();
        for (int y = 0; y < h; ++y) memcpy(pixels.data() + size_t(top + y) * width + left, from + size_t(y) * w, size_t(w));
        from += size_t(w) * h;
        uint8_t* to = pixels.data() + size_t(width) * height;
        for (int plane = 0; plane < 2; ++plane) {
            for (int y = 0; y < h / 2; ++y) memcpy(to + size_t(top / 2 + y) * (width / 2) + left / 2, from + size_t(y) * (w / 2), size_t(w / 2));
            from += size_t(w / 2) * (h / 2);
            to += size_t(width / 2) * (height / 2);
        }
    }
};

struct Side {
    std::unique_ptr<Reader> reader;
    const Track* track = nullptr;
    std::unique_ptr<VideoDecoder> decoder;
    std::vector<uint8_t> picture;

    bool open(const std::string& path, std::string& error) {
        reader = Reader::open(path, &error);
        if (!reader) return false;
        track = reader->first(Kind::Video);
        if (!track || track->width % 2 || track->height % 2) {
            error = "needs a video track with an even width and height";
            return false;
        }
        decoder = VideoDecoder::open(*reader, *track, &error);
        return decoder != nullptr;
    }

    bool show(uint32_t n, Canvas& canvas, int left, int top) {
        if (n < decoder->frames()) {
            Frame frame = decoder->frame(n);
            if (!frame || !copyI420(frame, picture)) return false;
        }
        if (picture.size() < size_t(track->width) * track->height * 3 / 2) return false;
        canvas.place(picture, track->width, track->height, left, top);
        return true;
    }
};

int sidebyside(const Args& args) {
    if (args.plain.size() < 3) return fail("usage: sidebyside <left file> <right file> <output> [--left TEXT] [--right TEXT] [--bitrate n] [--sound left|right|none]");
    std::string error;
    Side left, right;
    if (!left.open(args.plain[0], error)) return fail(args.plain[0] + ": " + error);
    if (!right.open(args.plain[1], error)) return fail(args.plain[1] + ": " + error);
    const int gap = 16, banner = 64, scale = 5;
    Canvas canvas(left.track->width + gap + right.track->width, banner + std::max(left.track->height, right.track->height));
    canvas.text(args.get("left", "LEFT"), left.track->width / 2, (banner - 7 * scale) / 2, scale);
    canvas.text(args.get("right", "RIGHT"), left.track->width + gap + right.track->width / 2, (banner - 7 * scale) / 2, scale);

    const Track& clock = *left.track;
    if (clock.samples.empty() || !clock.samples[0].duration) return fail("the left file has no frame timing");
    uint32_t tick = clock.samples[0].duration;
    EncodeSpec spec;
    spec.width = canvas.width;
    spec.height = canvas.height;
    spec.timescale = clock.timescale;
    spec.fps = clock.rate();
    spec.bitrate = int(args.number("bitrate", 16000000));
    spec.keyInterval = int(spec.fps + 0.5);
    spec.codec = fourcc("avc1");
    auto encoder = VideoEncoder::open(spec, &error);
    if (!encoder) return fail(error);
    auto writer = openOutput(args.plain[2], &error);
    if (!writer) return fail(error);
    TrackSpec picture;
    picture.kind = Kind::Video;
    picture.codec = spec.codec;
    picture.timescale = clock.timescale;
    picture.width = canvas.width;
    picture.height = canvas.height;
    int videoTrack = writer->add(picture);
    std::string choice = args.get("sound", "right");
    Side* heard = choice == "left" ? &left : choice == "right" ? &right : nullptr;
    const Track* sound = heard ? heard->reader->first(Kind::Audio) : nullptr;
    std::unique_ptr<Copier> copier;
    if (sound && !sound->config.empty()) copier.reset(new Copier{*heard->reader, *writer, *sound, writer->add(specOf(*sound))});
    bool configured = false;
    auto drain = [&] {
        Packet packet;
        while (encoder->take(packet)) {
            if (!configured) {
                writer->configure(videoTrack, encoder->configType(), encoder->config());
                configured = true;
            }
            writer->write(videoTrack, packet.data.data(), packet.data.size(), packet.pts, packet.dts, packet.duration ? packet.duration : tick, packet.sync);
        }
    };
    uint32_t count = std::max(left.decoder->frames(), right.decoder->frames());
    for (uint32_t n = 0; n < count; ++n) {
        if (!left.show(n, canvas, 0, banner) || !right.show(n, canvas, left.track->width + gap, banner)) return fail("frame " + std::to_string(n) + " did not decode");
        if (!encoder->encodeI420(canvas.pixels.data(), int64_t(n) * tick, tick)) return fail("the encoder refused a frame");
        drain();
        if (copier && !copier->until(double(n) * tick / clock.timescale)) return fail("copying audio failed");
    }
    if (!encoder->finish()) return fail("the encoder failed");
    drain();
    if (copier && !copier->until(1e299)) return fail("copying audio failed");
    if (!writer->finish(&error)) return fail(error);
    fprintf(stderr, "side by side: %u frames, %dx%d\n", count, canvas.width, canvas.height);
    return 0;
}

Register sideBySideCommand("sidebyside", sidebyside);

}
