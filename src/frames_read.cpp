#include "frames.h"
#include <algorithm>
#include <cmath>

namespace absent {

struct FrameReader::Impl {
    std::unique_ptr<Reader> file;
    const Track* track = nullptr;
    std::unique_ptr<VideoDecoder> decoder;
    FrameReaderSpec spec;
    int width = 0;
    int height = 0;
    int turn = 0;
    double shift = 0;
    std::vector<uint8_t> upright;

    double time(uint32_t index) const { return double(decoder->pts(index) - track->mediaStart) / track->timescale + shift; }
};

FrameReader::FrameReader() : impl(new Impl) {}
FrameReader::~FrameReader() = default;

std::unique_ptr<FrameReader> FrameReader::open(const std::string& path, const FrameReaderSpec& spec, std::string* error) {
    auto fail = [&](const char* text) {
        if (error) *error = text;
        return nullptr;
    };
    if (spec.width < 0 || spec.height < 0 || spec.width > 16384 || spec.height > 16384 || (spec.width == 0) != (spec.height == 0)) return fail("bad frame size");
    if (spec.layout == PixelLayout::Gray) return fail("frames come out as rgba, rgb24, bgra or i420");
    std::unique_ptr<FrameReader> reader(new FrameReader);
    Impl& r = *reader->impl;
    r.spec = spec;
    r.file = Reader::open(path, error);
    if (!r.file) return nullptr;
    r.track = r.file->first(Kind::Video);
    if (!r.track) return fail("no video track");
    if (!r.track->timescale) return fail("the video track has no clock");
    r.decoder = VideoDecoder::open(*r.file, *r.track, error, spec.software);
    if (!r.decoder) return nullptr;
    if (!r.decoder->frames()) return fail("the video track has no frames");
    double earliest = 1e300;
    for (const Track& t : r.file->tracks()) if (t.timescale && !t.samples.empty()) earliest = std::min(earliest, double(t.delay) / t.timescale);
    r.shift = double(r.track->delay) / r.track->timescale - (earliest < 1e300 ? earliest : 0);
    r.turn = spec.rotate ? ((r.track->rotation % 360) + 360) % 360 / 90 * 90 : 0;
    int w = r.track->width, h = r.track->height;
    if (w <= 0 || h <= 0) {
        Frame first = r.decoder->frame(0);
        if (!first) return fail("the first frame did not decode");
        w = first.width;
        h = first.height;
    }
    const bool sideways = r.turn == 90 || r.turn == 270;
    r.width = spec.width ? spec.width : sideways ? h : w;
    r.height = spec.height ? spec.height : sideways ? w : h;
    return reader;
}

int FrameReader::width() const { return impl->width; }
int FrameReader::height() const { return impl->height; }
int FrameReader::sourceWidth() const { return impl->track->width; }
int FrameReader::sourceHeight() const { return impl->track->height; }
int FrameReader::rotation() const { return impl->track->rotation; }
size_t FrameReader::frameBytes() const { return pixelBytes(impl->spec.layout, impl->width, impl->height); }
uint32_t FrameReader::frames() const { return impl->decoder->frames(); }
double FrameReader::seconds(uint32_t index) const { return impl->time(index); }
double FrameReader::rate() const { return impl->track->rate(); }

double FrameReader::length() const {
    const Track& t = *impl->track;
    const uint32_t last = frames() - 1;
    const double tail = t.order.empty() ? 0 : double(t.samples[t.order.back()].duration) / t.timescale;
    return impl->time(last) + tail;
}

uint32_t FrameReader::indexAt(double seconds) const {
    uint32_t low = 0, high = frames();
    while (low < high) {
        const uint32_t middle = low + (high - low) / 2;
        if (impl->time(middle) < seconds - 5e-7) low = middle + 1;
        else high = middle;
    }
    return low;
}

std::vector<uint32_t> FrameReader::steady(double start, double fps, double length, long count) const {
    std::vector<uint32_t> picked;
    const uint32_t total = frames();
    uint32_t index = indexAt(start);
    if (index >= total || !(fps > 0) || fps > 1000) return picked;
    const Track& track = *impl->track;
    const int64_t offset = std::llround(impl->shift * track.timescale) - track.mediaStart - std::llround(start * track.timescale);
    auto slot = [&](int64_t ticks) { return std::llround(double(ticks + offset) * fps / track.timescale); };
    const int64_t tail = track.order.empty() ? 0 : int64_t(track.samples[track.order.back()].duration);
    const long long first = slot(impl->decoder->pts(index));
    const long long end = std::max(slot(impl->decoder->pts(total - 1) + tail), first + 1);
    const long long most = length >= 0 ? std::llround(length * fps) : end;
    const bool filled = impl->file->tracks().size() > 1;
    for (long long n = filled ? 0 : first; n < end && n < most; ++n) {
        if (count >= 0 && long(picked.size()) >= count) break;
        while (index + 1 < total && slot(impl->decoder->pts(index + 1)) <= std::max(n, first)) ++index;
        picked.push_back(index);
    }
    return picked;
}

bool FrameReader::read(uint32_t index, std::vector<uint8_t>& out) {
    Impl& r = *impl;
    Frame frame = r.decoder->frame(index);
    if (!frame) return false;
    const bool sideways = r.turn == 90 || r.turn == 270;
    const int w = sideways ? r.height : r.width, h = sideways ? r.width : r.height;
    std::vector<uint8_t>& target = r.turn ? r.upright : out;
    const bool ok = withPixelBuffer(frame.native.get(), [&](const Picture& seen) {
        Picture p = seen;
        p.color = frame.color;
        p.color.full = seen.color.full;
        return pictureToLayout(p, w, h, r.spec.layout, target);
    });
    if (!ok) return false;
    if (r.turn) turnPixels(r.upright, w, h, r.spec.layout, r.turn, out);
    return true;
}

}
