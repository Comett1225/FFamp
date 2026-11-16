#include "cli.h"
#include "../src/h264/h264_ffamp.h"

namespace cli {

namespace {

struct RawOut {
    FILE* out = nullptr;
    long frames = 0;
    bool failed = false;
    bool quiet = false;
    std::vector<uint8_t> row;
};

void writeFrame(void* opaque, const FFampH264Frame* f) {
    RawOut& o = *static_cast<RawOut*>(opaque);
    if (o.frames == 0 && !o.quiet) {
        const char* layout = f->chroma_shift_x ? (f->chroma_shift_y ? "420" : "422") : "444";
        std::string format = std::string(f->rgb ? "gbr" : "yuv") + (f->rgb ? "" : layout) + "p";
        if (f->bits > 8) format += std::to_string(f->bits) + "le";
        fprintf(stderr, "format %s size %dx%d\n", format.c_str(), f->width, f->height);
    }
    ++o.frames;
    if (!o.out || o.failed) return;
    const int bytes = f->bits > 8 ? 2 : 1;
    for (int p = 0; p < 3; ++p) {
        const int w = p ? -((-f->width) >> f->chroma_shift_x) : f->width;
        const int h = p ? -((-f->height) >> f->chroma_shift_y) : f->height;
        for (int y = 0; y < h; ++y) {
            if (fwrite(f->data[p] + (ptrdiff_t)y * f->linesize[p], 1, (size_t)w * bytes, o.out) != (size_t)w * bytes) {
                o.failed = true;
                return;
            }
        }
    }
}

struct Lead {
    const std::vector<std::vector<uint8_t>>* packets = nullptr;
    size_t at = 0;
};

int nextPacket(void* opaque, const uint8_t** data, int* size) {
    Lead& lead = *static_cast<Lead*>(opaque);
    if (lead.at >= lead.packets->size() || lead.at >= 250) return 0;
    const std::vector<uint8_t>& packet = (*lead.packets)[lead.at++];
    *data = packet.data();
    *size = int(packet.size());
    return 1;
}

int h264(const Args& args) {
    if (args.plain.empty()) return fail("h264 needs a file [--threads n] [--slice] [--null] [--repeat n]");
    std::string error;
    auto reader = Reader::open(args.plain[0], &error);
    if (!reader) return fail(error);
    const Track* track = reader->first(Kind::Video);
    if (!track || track->codec != fourcc("avc1")) return fail("no H.264 video track");
    const int threads = int(args.number("threads", 0));
    const int type = args.has("slice") ? FFAMP_H264_SLICE_THREADS : FFAMP_H264_FRAME_THREADS;
    std::vector<std::vector<uint8_t>> packets(track->samples.size());
    for (size_t i = 0; i < track->samples.size(); ++i) {
        packets[i].resize(track->samples[i].size);
        if (!reader->read(*track, i, packets[i].data())) return fail("sample " + std::to_string(i) + " could not be read");
    }
    RawOut out;
    out.out = args.has("null") ? nullptr : stdout;
    const long repeat = std::max(1L, args.number("repeat", 1));
    double start = now();
    for (long r = 0; r < repeat; ++r) {
        FFampH264* decoder = ffamp_h264_open(track->config.data(), int(track->config.size()), threads, type);
        if (!decoder) return fail("the H.264 decoder did not open");
        Lead lead{&packets};
        ffamp_h264_guess_delay(decoder, nextPacket, &lead);
        out.quiet = r > 0;
        if (r > 0) out.frames = 0;
        for (size_t i = 0; i < packets.size(); ++i) {
            ffamp_h264_decode(decoder, packets[i].data(), int(packets[i].size()), track->samples[i].pts, writeFrame, &out);
        }
        ffamp_h264_drain(decoder, writeFrame, &out);
        ffamp_h264_close(decoder);
    }
    double spent = now() - start;
    if (args.has("null")) fprintf(stderr, "decoded %ld frames %ld times in %.3f s (%.1f fps)\n", out.frames, repeat, spent, out.frames * repeat / spent);
    return out.failed ? fail("output closed") : 0;
}

}

Register h264Command("h264", h264);

}
