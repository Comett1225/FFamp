#include "image.h"
#include "../readers.h"
#include "absent/media.h"
#include <algorithm>
#include <atomic>
#include <condition_variable>
#include <cstring>
#include <deque>
#include <functional>
#include <map>
#include <mutex>
#include <thread>

namespace absent {

std::unique_ptr<SoftDecoder> openStillDecoder(std::string* error);
std::unique_ptr<SoftDecoder> openPlatformVideo(const Track& track, const std::vector<uint8_t>& config, const StreamShape& shape, std::string* error);

std::shared_ptr<Image> ImagePool::take() {
    std::unique_ptr<Image> image;
    {
        std::lock_guard<std::mutex> hold(lock);
        if (!spare.empty()) {
            image = std::move(spare.back());
            spare.pop_back();
        }
    }
    if (!image) image = std::make_unique<Image>();
    std::weak_ptr<ImagePool> home = weak_from_this();
    return std::shared_ptr<Image>(image.release(), [home](Image* done) {
        if (auto pool = home.lock()) {
            std::lock_guard<std::mutex> hold(pool->lock);
            if (pool->spare.size() < 24) {
                done->alpha.clear();
                pool->spare.emplace_back(done);
                return;
            }
        }
        delete done;
    });
}

std::shared_ptr<Image> ImagePool::copy(const Picture& p) {
    auto image = take();
    fillImage(*image, p);
    return image;
}

void shapeImage(Image& image, int width, int height, int chromaWidth, int chromaHeight, int bits, bool interleaved) {
    const size_t unit = bits > 8 ? 2 : 1;
    const size_t lumaRow = size_t(width) * unit, chromaRow = size_t(chromaWidth) * unit * (interleaved ? 2 : 1);
    const size_t luma = lumaRow * size_t(height), chroma = chromaRow * size_t(chromaHeight);
    image.bytes.resize(luma + chroma * (interleaved ? 1 : 2));
    Picture& p = image.picture;
    p = Picture();
    p.width = width;
    p.height = height;
    p.chromaWidth = chromaWidth;
    p.chromaHeight = chromaHeight;
    p.bits = bits;
    p.interleaved = interleaved;
    p.plane[0] = image.bytes.data();
    p.plane[1] = image.bytes.data() + luma;
    p.plane[2] = interleaved ? nullptr : image.bytes.data() + luma + chroma;
    p.stride[0] = lumaRow;
    p.stride[1] = chromaRow;
    p.stride[2] = interleaved ? 0 : chromaRow;
}

void fillImage(Image& image, const Picture& from) {
    shapeImage(image, from.width, from.height, from.chromaWidth, from.chromaHeight, from.bits, from.interleaved);
    Picture& to = image.picture;
    to.shift = from.shift;
    to.stamp = from.stamp;
    to.color = from.color;
    for (int plane = 0; plane < (from.interleaved ? 2 : 3); ++plane) {
        const int rows = plane ? from.chromaHeight : from.height;
        for (int y = 0; y < rows; ++y) memcpy(const_cast<uint8_t*>(to.plane[plane]) + size_t(y) * to.stride[plane], from.plane[plane] + size_t(y) * from.stride[plane], to.stride[plane]);
    }
    if (from.alpha) {
        image.alpha.resize(size_t(from.width) * size_t(from.height));
        for (int y = 0; y < from.height; ++y) memcpy(image.alpha.data() + size_t(y) * size_t(from.width), from.alpha + size_t(y) * from.alphaStride, size_t(from.width));
        to.alpha = image.alpha.data();
        to.alphaStride = size_t(from.width);
    }
}

const Image* imageOf(const void* native) { return static_cast<const Image*>(native); }

bool withPixelBuffer(void* pixelBuffer, const std::function<bool(const Picture&)>& use) {
    const Image* image = imageOf(pixelBuffer);
    return image && image->picture.plane[0] && use(image->picture);
}

namespace {

const uint32_t kNone = 0xFFFFFFFFu;

bool locked(const Frame& frame, const std::function<bool(Picture&)>& use) {
    return withPixelBuffer(frame.native.get(), [&](const Picture& seen) {
        Picture p = seen;
        p.color = frame.color;
        p.color.full = seen.color.full;
        return use(p);
    });
}

}

bool copyI420(const Frame& frame, std::vector<uint8_t>& out) {
    if (frame.bits != 8 || frame.chromaWidth != (frame.width + 1) / 2 || frame.chromaHeight != (frame.height + 1) / 2) return false;
    return locked(frame, [&](Picture& p) {
        pictureToPlanar(p, out, true);
        return true;
    });
}

bool copyYuv(const Frame& frame, std::vector<uint8_t>& out) {
    if (frame.bits == 8 && frame.chromaWidth == (frame.width + 1) / 2 && frame.chromaHeight == (frame.height + 1) / 2) return copyI420(frame, out);
    return locked(frame, [&](Picture& p) {
        pictureToPlanar(p, out);
        return true;
    });
}

bool copyBGRA(const Frame& frame, std::vector<uint8_t>& out) {
    if (frame.width <= 0 || frame.height <= 0) return false;
    out.resize(size_t(frame.width) * size_t(frame.height) * 4);
    return locked(frame, [&](Picture& p) { return pictureToBGRA(p, out.data(), size_t(p.width) * 4); });
}

struct VideoDecoder::Impl {
    Reader* file = nullptr;
    const Track* track = nullptr;
    std::shared_ptr<ImagePool> pool = std::make_shared<ImagePool>();
    std::mutex lock;
    std::map<int64_t, std::shared_ptr<Image>> ready;
    std::atomic<uint64_t> shown{0};
    std::atomic<int64_t> floor{0};
    bool keep = true;
    uint32_t ahead = 8;
    uint32_t next = kNone;
    uint32_t last = kNone;
    int64_t kept = INT64_MIN;
    struct Job {
        std::vector<uint8_t> bytes;
        std::vector<uint8_t> mask;
        int64_t stamp = 0;
    };
    std::unique_ptr<SoftDecoder> soft;
    std::unique_ptr<SoftDecoder> softMask;
    std::vector<uint8_t> veil;
    int64_t veilStamp = INT64_MIN;
    std::condition_variable arrived, queued;
    std::deque<Job> jobs;
    bool busy = false;
    bool stopping = false;
    std::thread worker;
    uint64_t fed = 0;
    uint64_t restarts = 0;
    StreamShape shape;
    const char* backend = "software";
    struct Shown {
        uint32_t first = kNone;
        uint32_t second = kNone;
        bool firstTop = true;
    };
    std::vector<Shown> list;
    std::vector<uint32_t> keys;
    int concealing = 0;
    std::vector<bool> lost;

    ~Impl() {
        if (worker.joinable()) {
            {
                std::lock_guard<std::mutex> hold(lock);
                stopping = true;
            }
            queued.notify_all();
            worker.join();
        }
    }

    bool decodeSoft(const Job& job) {
        veilStamp = INT64_MIN;
        if (softMask && !job.mask.empty()) {
            softMask->decode(job.mask.data(), job.mask.size(), job.stamp, [&](const Picture& m) {
                if (m.bits != 8 || m.interleaved || !m.plane[0] || m.width <= 0 || m.height <= 0) return;
                veil.resize(size_t(m.width) * size_t(m.height));
                for (int y = 0; y < m.height; ++y) memcpy(veil.data() + size_t(y) * size_t(m.width), m.plane[0] + size_t(y) * m.stride[0], size_t(m.width));
                veilStamp = m.stamp;
            });
        }
        return soft->decode(job.bytes.data(), job.bytes.size(), job.stamp, [&](const Picture& p) {
            Picture seen = p;
            if (veilStamp == p.stamp && veil.size() == size_t(p.width) * size_t(p.height)) {
                seen.alpha = veil.data();
                seen.alphaStride = size_t(p.width);
            }
            store(seen);
        });
    }

    Frame concealed(VideoDecoder& owner, uint32_t index) {
        uint32_t earlier = index;
        while (earlier > 0 && lost[earlier]) --earlier;
        if (lost[earlier] || concealing >= 30) return {};
        ++concealing;
        next = kNone;
        Frame shownFrame = owner.frame(earlier);
        --concealing;
        last = index;
        if (shownFrame) {
            const Shown item = list[index];
            shownFrame.pts = track->samples[item.first].pts;
            shownFrame.duration = int64_t(track->samples[item.first].duration) + (item.second != kNone ? int64_t(track->samples[item.second].duration) : 0);
        }
        return shownFrame;
    }

    void pairFields(const H264Layout& layout) {
        const auto& samples = track->samples;
        std::vector<uint8_t> kind(samples.size(), 0);
        std::vector<uint32_t> number(samples.size(), 0);
        std::vector<uint8_t> bytes;
        for (uint32_t i = 0; i < samples.size(); ++i) {
            bytes.resize(samples[i].size);
            if (file->read(*track, i, bytes.data())) kind[i] = uint8_t(h264FieldOf(layout, bytes.data(), bytes.size(), number[i]));
        }
        std::vector<uint8_t> second(samples.size(), 0);
        for (uint32_t i = 0; i + 1 < samples.size(); ++i) {
            if (kind[i] && kind[i + 1] && kind[i] != kind[i + 1] && number[i] == number[i + 1] && !second[i]) second[i + 1] = 1;
        }
        std::vector<Shown> paired;
        for (uint32_t index : track->order) {
            if (index >= samples.size() || second[index]) continue;
            Shown item;
            item.first = index;
            if (index + 1 < samples.size() && second[index + 1]) item.second = index + 1;
            item.firstTop = kind[index] != 2;
            paired.push_back(item);
        }
        list = std::move(paired);
        keys.clear();
        for (uint32_t index : track->syncs) if (index < samples.size() && !second[index]) keys.push_back(index);
    }

    std::shared_ptr<Image> peek(int64_t pts) {
        std::lock_guard<std::mutex> hold(lock);
        auto found = ready.find(pts);
        return found == ready.end() ? nullptr : found->second;
    }

    bool has(int64_t pts) {
        std::lock_guard<std::mutex> hold(lock);
        return ready.count(pts) > 0;
    }

    std::shared_ptr<Image> weave(const Image& top, const Image& bottom) {
        const Picture& a = top.picture;
        const Picture& b = bottom.picture;
        if (a.width != b.width || a.height != b.height || a.bits != b.bits || a.chromaWidth != b.chromaWidth || a.chromaHeight != b.chromaHeight || a.interleaved != b.interleaved) return nullptr;
        auto out = pool->take();
        shapeImage(*out, a.width, a.height, a.chromaWidth, a.chromaHeight, a.bits, a.interleaved);
        out->picture.shift = a.shift;
        out->picture.color = a.color;
        out->picture.stamp = a.stamp;
        for (int plane = 0; plane < (a.interleaved ? 2 : 3); ++plane) {
            const int rows = plane ? a.chromaHeight : a.height;
            for (int row = 0; row < rows; ++row) {
                const Picture& source = row % 2 ? b : a;
                memcpy(const_cast<uint8_t*>(out->picture.plane[plane]) + size_t(row) * out->picture.stride[plane], source.plane[plane] + size_t(row) * source.stride[plane], out->picture.stride[plane]);
            }
        }
        return out;
    }

    void restart() {
        {
            std::unique_lock<std::mutex> hold(lock);
            jobs.clear();
            arrived.wait(hold, [&] { return !busy; });
        }
        soft->reset();
        if (softMask) softMask->reset();
        clear();
        kept = INT64_MIN;
    }

    void store(const Picture& p) {
        ++shown;
        if (p.color.known) {
            std::lock_guard<std::mutex> hold(lock);
            if (!shape.color.known) shape.color = p.color;
        }
        if (!keep || p.stamp < floor) return;
        auto image = pool->copy(p);
        {
            std::lock_guard<std::mutex> hold(lock);
            ready[p.stamp] = std::move(image);
        }
        arrived.notify_all();
    }

    void work() {
        std::unique_lock<std::mutex> hold(lock);
        for (;;) {
            queued.wait(hold, [&] { return stopping || !jobs.empty(); });
            if (stopping) return;
            Job job = std::move(jobs.front());
            jobs.pop_front();
            busy = true;
            hold.unlock();
            decodeSoft(job);
            hold.lock();
            busy = false;
            arrived.notify_all();
        }
    }

    void settle(int64_t pts, int64_t later) {
        std::unique_lock<std::mutex> hold(lock);
        arrived.wait(hold, [&] { return (ready.count(pts) && ready.count(later)) || (jobs.empty() && !busy); });
        if (ready.count(pts) && ready.count(later)) return;
        hold.unlock();
        for (int extra = 0; extra < 16 && next != kNone && next < track->samples.size(); ++extra) {
            feed(next++, false);
            if (has(pts) && has(later)) return;
        }
        soft->drain([&](const Picture& p) { store(p); });
        if (!soft->drainKeepsState()) {
            soft->reset();
            next = kNone;
        }
    }

    bool feed(uint32_t index, bool queue) {
        const Sample& s = track->samples[index];
        if (!s.size) {
            ++fed;
            return true;
        }
        Job job;
        job.bytes.resize(s.size);
        job.stamp = s.pts;
        if (!file->read(*track, index, job.bytes.data())) return false;
        if (softMask) file->readAlpha(*track, index, job.mask);
        ++fed;
        if (!queue) return decodeSoft(job);
        if (!worker.joinable()) worker = std::thread([this] { work(); });
        {
            std::lock_guard<std::mutex> hold(lock);
            jobs.push_back(std::move(job));
        }
        queued.notify_one();
        return true;
    }

    void clear() {
        std::lock_guard<std::mutex> hold(lock);
        ready.clear();
    }

    uint32_t keyFor(uint32_t target) const {
        auto it = std::upper_bound(keys.begin(), keys.end(), target);
        while (it != keys.begin()) {
            --it;
            if (track->samples[*it].pts <= track->samples[target].pts) return *it;
        }
        return keys.empty() || keys.front() > target ? 0 : keys.front();
    }

    std::shared_ptr<Image> take(int64_t pts) {
        std::lock_guard<std::mutex> hold(lock);
        auto found = ready.find(pts);
        if (found == ready.end()) return nullptr;
        std::shared_ptr<Image> image = found->second;
        ready.erase(ready.begin(), found);
        return image;
    }
};

VideoDecoder::VideoDecoder() : impl(new Impl) {}
VideoDecoder::~VideoDecoder() = default;

std::unique_ptr<VideoDecoder> VideoDecoder::open(Reader& file, const Track& track, std::string* error, bool software) {
    auto fail = [&](const char* text) {
        if (error) *error = text;
        return nullptr;
    };
    if (track.samples.empty() || !track.timescale || track.timescale > uint32_t(INT32_MAX) || track.width <= 0 || track.height <= 0) return fail("track is empty");
    const uint32_t codec = track.codec;
    const bool vc1 = vc1Codec(codec) && track.configType == fourcc("bmih");
    const bool msmpeg4 = msmpeg4Codec(codec);
    if (const char* name = windowsMediaName(track.codec); name && !vc1 && !msmpeg4) {
        if (error) *error = std::string("this video codec (") + name + ") is not decoded yet";
        return nullptr;
    }
    const bool vp9 = codec == fourcc("vp09"), vp8 = codec == fourcc("vp08"), av1 = codec == fourcc("av01");
    const bool h264 = track.configType == fourcc("avcC"), hevc = track.configType == fourcc("hvcC");
    const bool apv = codec == fourcc("apv1");
    const bool part2 = codec == fourcc("mp4v") && track.configType == fourcc("esds");
    const bool jpeg = codec == fourcc("jpeg");
    const bool mpeg = codec == fourcc("mp1v") || codec == fourcc("mp2v");
    const bool twelve = codec == fourcc("ap4h") || codec == fourcc("ap4x");
    const bool prores = twelve || codec == fourcc("apch") || codec == fourcc("apcn") || codec == fourcc("apcs") || codec == fourcc("apco");
    const bool dv = (codec >> 16) == 0x6476;
    const bool h263 = codec == fourcc("h263");
    if (!h264 && !hevc && !vp9 && !vp8 && !av1 && !apv && !part2 && !jpeg && !vc1 && !msmpeg4 && !mpeg && !prores && !dv && !h263) return fail("video codec not supported by this decoder");
    if ((h264 || hevc) && track.config.empty()) return fail("track is empty");

    std::unique_ptr<VideoDecoder> decoder(new VideoDecoder);
    Impl& d = *decoder->impl;
    d.file = &file;
    d.track = &track;
    d.shape.width = track.width;
    d.shape.height = track.height;
    if (uint64_t(track.width) * uint64_t(track.height) > 2304u * 1296u) d.ahead = 4;

    std::vector<uint8_t> config = track.config;
    if (apv) {
        const bool described = track.configType == fourcc("apvC") && config.size() >= 22;
        const int chroma = described ? config[20] >> 4 : 2;
        d.shape.bits = described ? 8 + (config[20] & 15) : 10;
        d.shape.chromaX = chroma == 2 ? 1 : 0;
        d.shape.chromaY = 0;
        if (chroma != 2 && chroma != 3) return fail("this APV picture format is not supported");
    } else if (prores) {
        d.shape.bits = twelve ? 12 : 10;
        d.shape.chromaX = twelve ? 0 : 1;
        d.shape.chromaY = 0;
    } else if (dv) {
        d.shape.chromaX = codec == fourcc("dvc ") || codec == fourcc("dvpp") ? 2 : 1;
        d.shape.chromaY = codec == fourcc("dvcp") ? 1 : 0;
    } else if (part2 || jpeg || mpeg) {
        d.shape.chromaY = 1;
    } else if (!shapeFromConfig(track.configType, config, d.shape) && (vp9 || av1)) {
        config.clear();
    }
    if ((vp9 || av1) && config.empty()) {
        const uint32_t first = track.syncs.empty() ? 0 : track.syncs.front();
        if (first >= track.samples.size()) return fail("track is empty");
        std::vector<uint8_t> key(track.samples[first].size);
        if (!file.read(track, first, key.data())) return fail("cannot read the first key frame");
        if (vp9) {
            if (!vp9Shape(key.data(), key.size(), d.shape)) return fail("the first VP9 key frame has no readable header");
            config = vp9Config(d.shape, track.rate(), 0);
        } else if (!av1Config(key.data(), key.size(), config, d.shape)) {
            return fail("the first AV1 key frame has no sequence header");
        }
    }
    d.keys = track.syncs;
    H264Layout layout;
    if (h264 && h264Layout(config, layout) && layout.fields) d.pairFields(layout);
    if (d.list.empty()) {
        d.list.reserve(track.order.size());
        for (uint32_t index : track.order) {
            if (index >= track.samples.size()) return fail("track is broken");
            Impl::Shown item;
            item.first = index;
            d.list.push_back(item);
        }
    }
    const Color fromStream = d.shape.color;
    if (track.color.known) d.shape.color = track.color;
    if (h264 || hevc || !track.color.known) d.shape.color.full = fromStream.full;

    std::string problem;
    if (vc1) {
        d.soft = openVc1(codec, config, track.width, track.height, error);
    } else if (msmpeg4) {
        d.soft = openMsmpeg4(codec, config, track.width, track.height, error);
    } else if (h263) {
        d.soft = openH263(error);
    } else if (part2) {
        d.soft = openXvid(config, track.width, track.height);
        if (!d.soft) return fail("MPEG-4 part 2 video needs xvidcore, which was not found when building");
    } else if (jpeg) {
        d.soft = openStillDecoder(error);
        d.shape.color = Color();
    } else if (apv) {
        d.soft = openApvDecoder(error);
    } else if (prores) {
        d.soft = openProRes(codec, error);
    } else if (dv) {
        d.soft = openDv();
    } else if (av1) {
        d.soft = openDav1d(int(d.ahead), error);
        if (d.soft) d.ahead += d.ahead / 2;
    } else if (vp8 || vp9) {
        if (track.alpha) d.softMask = openVpxDecoder(vp8, nullptr);
        d.soft = openVpxDecoder(vp8, error);
    } else if (mpeg) {
        if (!software) d.soft = openPlatformVideo(track, config, d.shape, &problem);
        if (!d.soft && error) *error = problem.empty() ? "MPEG-1 and MPEG-2 video are not decoded on this system yet" : problem;
    } else {
        std::string fallback;
        if (!software) d.soft = openPlatformVideo(track, config, d.shape, &problem);
        if (!d.soft) d.soft = h264 ? openOpenH264(config, &fallback) : openDe265(config, &fallback);
        if (!d.soft && error) *error = problem.empty() ? fallback : problem + "; " + fallback;
    }
    if (!d.soft) return nullptr;
    d.backend = d.soft->name();
    return decoder;
}

const char* VideoDecoder::backend() const { return impl->backend; }
uint32_t VideoDecoder::frames() const { return uint32_t(impl->list.size()); }
int64_t VideoDecoder::pts(uint32_t index) const { return index < impl->list.size() ? impl->track->samples[impl->list[index].first].pts : 0; }
uint64_t VideoDecoder::fed() const { return impl->fed; }
uint64_t VideoDecoder::restarts() const { return impl->restarts; }

Frame VideoDecoder::frame(uint32_t index) {
    Impl& d = *impl;
    const Track& track = *d.track;
    if (index >= d.list.size()) return {};
    if (index < d.lost.size() && d.lost[index]) return d.concealed(*this, index);
    const Impl::Shown item = d.list[index];
    const bool pair = item.second != kNone && !d.soft->weavesFields();
    const uint32_t target = item.second != kNone ? item.second : item.first;
    const int64_t pts = track.samples[item.first].pts;
    const int64_t later = pair ? track.samples[target].pts : pts;
    d.keep = true;
    d.floor = std::min(pts, later);
    const bool following = d.last != kNone && index == d.last + 1;
    d.last = index;
    auto present = [&] { return d.has(pts) && d.has(later); };
    if (!present() && d.next != kNone && d.next > target && d.floor >= d.kept) d.settle(pts, later);
    if (!present()) {
        const uint32_t key = d.keyFor(item.first);
        if (d.next == kNone || d.next < key || d.next > item.first) {
            d.restart();
            d.next = key;
            ++d.restarts;
        }
        while (d.next <= target) {
            d.feed(d.next, true);
            ++d.next;
        }
    }
    if (following && d.next != kNone) {
        const uint64_t limit = std::min<uint64_t>(uint64_t(target) + d.ahead, track.samples.size() - 1);
        while (d.next <= limit) {
            d.feed(d.next, true);
            ++d.next;
        }
    }
    if (!present()) d.settle(pts, later);
    std::shared_ptr<Image> image;
    if (pair) {
        auto one = d.peek(pts), two = d.peek(later);
        if (one && two) image = item.firstTop ? d.weave(*one, *two) : d.weave(*two, *one);
        d.take(std::min(pts, later));
    } else {
        image = d.take(pts);
    }
    if (!image) {
        if (d.lost.size() < d.list.size()) d.lost.resize(d.list.size(), false);
        d.lost[index] = true;
        return d.concealed(*this, index);
    }
    d.kept = d.floor;
    const Picture& p = image->picture;
    Frame frame;
    frame.native = image;
    frame.width = p.width;
    frame.height = p.height;
    frame.pts = pts;
    frame.duration = int64_t(track.samples[item.first].duration) + (item.second != kNone ? int64_t(track.samples[item.second].duration) : 0);
    frame.bits = p.bits;
    frame.chromaWidth = p.chromaWidth;
    frame.chromaHeight = p.chromaHeight;
    {
        std::lock_guard<std::mutex> hold(d.lock);
        frame.color = d.shape.color;
    }
    if (!frame.color.known && p.color.known) frame.color = p.color;
    if (!frame.color.known) {
        const bool high = frame.height >= 720;
        frame.color.known = true;
        frame.color.primaries = high ? 1 : 6;
        frame.color.transfer = 1;
        frame.color.matrix = high ? 1 : 6;
    }
    frame.color.full = p.color.full;
    return frame;
}

uint64_t VideoDecoder::run(bool) {
    Impl& d = *impl;
    d.restart();
    d.keep = false;
    d.shown = 0;
    for (uint32_t i = 0; i < d.track->samples.size(); ++i) d.feed(i, false);
    d.soft->drain([&](const Picture& p) { d.store(p); });
    d.next = kNone;
    d.last = kNone;
    return d.shown;
}

}
