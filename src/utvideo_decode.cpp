#include "own_video.h"
#include "read_common.h"
#include "vlc.h"

namespace absent {

namespace {

int midPred(int a, int b, int c) { return std::max(std::min(a, b), std::min(std::max(a, b), c)); }

struct LittleBits {
    const uint8_t* data = nullptr;
    size_t size = 0;
    size_t at = 0;
    int64_t left() const { return int64_t(size) * 8 - int64_t(at); }
    uint32_t read(int n) {
        uint32_t value = 0;
        for (int k = 0; k < n; ++k, ++at) {
            const size_t byte = at >> 3;
            if (byte < size) value |= uint32_t((data[byte] >> (at & 7)) & 1) << k;
        }
        return value;
    }
};

bool huffman(const uint8_t* lengths, int count, Vlc& vlc, int& fill) {
    fill = -1;
    std::vector<uint8_t> bits(static_cast<size_t>(count));
    int counts[34] = {};
    for (int i = 0; i < count; ++i) {
        if (lengths[i] == 0) {
            fill = i;
            return true;
        }
        if (lengths[i] != 255 && lengths[i] > 32) return false;
        bits[size_t(i)] = lengths[i] == 255 ? 0 : lengths[i];
        counts[bits[size_t(i)]]++;
    }
    if (counts[0] == count) return false;
    for (int i = 31; i >= 0; --i) counts[i] += counts[i + 1];
    std::vector<uint8_t> lens(static_cast<size_t>(count));
    std::vector<uint16_t> symbols(static_cast<size_t>(count));
    for (int i = 0; i < count; ++i) {
        const int at = --counts[bits[size_t(i)]];
        lens[size_t(at)] = bits[size_t(i)];
        symbols[size_t(at)] = uint16_t(i);
    }
    return vlc.buildInOrder(11, size_t(counts[0]), lens.data(), symbols.data(), 0);
}

struct UtDecoder : SoftDecoder {
    uint32_t tag = 0;
    int width = 0;
    int height = 0;
    int planes = 3;
    int chromaX = 0;
    int chromaY = 0;
    bool rgb = false;
    bool pro = false;
    bool pack = false;
    bool interlaced = false;
    int slices = 1;
    uint32_t frameInfoSize = 4;
    Color color;
    Planes out;
    std::vector<uint8_t> swapped;
    std::vector<uint16_t> line;

    const char* name() const override { return "utvideo"; }
    void reset() override {}

    int planeWidth(int plane) const { return plane == 1 || plane == 2 ? width >> chromaX : width; }
    int planeHeight(int plane) const { return plane == 1 || plane == 2 ? height >> chromaY : height; }
    int cmask(int plane) const {
        const int luma = !pro && chromaY && plane == 0;
        return interlaced ? ~(1 + 2 * luma) : ~luma;
    }

    template <typename T>
    bool plane(int index, T* dst, ptrdiff_t stride, const uint8_t* offsets, const uint8_t* lengths, bool predict, size_t room) {
        const int w = planeWidth(index), h = planeHeight(index), top = sizeof(T) == 2 ? 0x3FF : 0xFF, start = sizeof(T) == 2 ? 0x200 : 0x80;
        const int mask = sizeof(T) == 2 ? ~0 : cmask(index);
        Vlc vlc;
        int fill = -1;
        if (!huffman(lengths, sizeof(T) == 2 ? 1024 : 256, vlc, fill)) return false;
        int end = 0;
        for (int slice = 0; slice < slices; ++slice) {
            const int begin = end;
            end = (h * (slice + 1) / slices) & mask;
            T* dest = dst + ptrdiff_t(begin) * stride;
            int prev = start;
            if (fill >= 0) {
                for (int j = begin; j < end; ++j, dest += stride) {
                    for (int i = 0; i < w; ++i) {
                        int pix = fill;
                        if (predict) {
                            prev = (prev + pix) & top;
                            pix = prev;
                        }
                        dest[i] = T(pix);
                    }
                }
                continue;
            }
            const uint32_t from = slice ? le32(offsets + size_t(slice) * 4 - 4) : 0, to = le32(offsets + size_t(slice) * 4);
            if (to <= from || size_t(to) > room) return false;
            const size_t bytes = to - from;
            swapped.assign((bytes + 3) & ~size_t(3), 0);
            const uint8_t* data = offsets + size_t(slices) * 4 + from;
            for (size_t k = 0; k < swapped.size(); ++k) {
                const size_t source = (k & ~size_t(3)) + 3 - (k & 3);
                swapped[k] = source < bytes ? data[source] : 0;
            }
            BitReader in(swapped.data(), swapped.size(), int64_t(bytes) * 8);
            line.assign(size_t(w), 0);
            for (int j = begin; j < end; ++j, dest += stride) {
                for (int i = 0; i < w && in.left() > 0; ++i) {
                    const int symbol = vlc.read(in);
                    if (symbol < 0) return false;
                    if (predict) line[size_t(i)] = uint16_t(symbol);
                    else dest[i] = T(symbol);
                }
                if (predict) {
                    for (int i = 0; i < w; ++i) {
                        prev += line[size_t(i)];
                        dest[i] = T(prev & top);
                    }
                }
                prev = dest[w - 1];
            }
        }
        return true;
    }

    bool packed(int index, uint8_t* dst, ptrdiff_t stride, const std::vector<std::pair<const uint8_t*, size_t>>& control, const std::vector<std::pair<const uint8_t*, size_t>>& data) {
        const int h = planeHeight(index), mask = cmask(index);
        int end = 0;
        for (int slice = 0; slice < slices; ++slice) {
            LittleBits c{control[size_t(index * slices + slice)].first, control[size_t(index * slices + slice)].second};
            LittleBits p{data[size_t(index * slices + slice)].first, data[size_t(index * slices + slice)].second};
            const int begin = end;
            end = (h * (slice + 1) / slices) & mask;
            uint8_t* first = dst + ptrdiff_t(begin) * stride;
            uint8_t* last = dst + ptrdiff_t(end) * stride;
            if (3 * ((last - first + 7) / 8) > c.left()) return false;
            for (uint8_t* q = first; q < last; q += 8) {
                const int bits = int(c.read(3));
                if (!bits) {
                    memset(q, 0, 8);
                    continue;
                }
                const uint32_t sub = 0x80u >> (8 - (bits + 1));
                if ((bits + 1) * 8 > p.left()) return false;
                for (int k = 0; k < 8; ++k) {
                    uint8_t v = uint8_t(p.read(bits + 1));
                    const uint32_t add = (~uint32_t(v) & sub) << (8 - bits);
                    v = uint8_t(v - sub);
                    v = uint8_t(v + add);
                    q[k] = v;
                }
            }
        }
        return true;
    }

    void median(uint8_t* src, ptrdiff_t stride, int w, int h, int rmode) {
        const int mask = interlaced ? ~(rmode ? 3 : 1) : ~rmode;
        const ptrdiff_t step = interlaced ? stride * 2 : stride;
        for (int slice = 0; slice < slices; ++slice) {
            const int begin = ((slice * h) / slices) & mask;
            int rows = ((((slice + 1) * h) / slices) & mask) - begin;
            if (interlaced) rows >>= 1;
            if (!rows) continue;
            uint8_t* b = src + ptrdiff_t(begin) * stride;
            b[0] = uint8_t(b[0] + 0x80);
            int acc = 0;
            for (int i = 0; i < w; ++i) b[i] = uint8_t(acc += b[i]);
            if (interlaced)
                for (int i = 0; i < w; ++i) b[stride + i] = uint8_t(acc += b[stride + i]);
            b += step;
            if (rows <= 1) continue;
            int c = b[-step];
            b[0] = uint8_t(b[0] + c);
            uint8_t left = b[0], leftTop = uint8_t(c);
            auto run = [&](uint8_t* dst, const uint8_t* above, int from, int count) {
                for (int i = from; i < from + count; ++i) {
                    left = uint8_t(midPred(left, above[i], uint8_t(left + above[i] - leftTop)) + dst[i]);
                    leftTop = above[i];
                    dst[i] = left;
                }
            };
            run(b, b - step, 1, w - 1);
            if (interlaced) run(b + stride, b - stride, 0, w);
            b += step;
            for (int j = 2; j < rows; ++j, b += step) {
                run(b, b - step, 0, w);
                if (interlaced) run(b + stride, b - stride, 0, w);
            }
        }
    }

    void gradient(uint8_t* src, ptrdiff_t stride, int w, int h, int rmode) {
        const int mask = interlaced ? ~(rmode ? 3 : 1) : ~rmode;
        const ptrdiff_t step = interlaced ? stride * 2 : stride;
        for (int slice = 0; slice < slices; ++slice) {
            const int begin = ((slice * h) / slices) & mask;
            int rows = ((((slice + 1) * h) / slices) & mask) - begin;
            if (interlaced) rows >>= 1;
            if (!rows) continue;
            uint8_t* b = src + ptrdiff_t(begin) * stride;
            b[0] = uint8_t(b[0] + 0x80);
            int acc = 0;
            for (int i = 0; i < w; ++i) b[i] = uint8_t(acc += b[i]);
            if (interlaced)
                for (int i = 0; i < w; ++i) b[stride + i] = uint8_t(acc += b[stride + i]);
            b += step;
            for (int j = 1; j < rows; ++j, b += step) {
                b[0] = uint8_t(b[0] + b[-step]);
                for (int i = 1; i < w; ++i) b[i] = uint8_t(b[i - step] - b[i - step - 1] + b[i - 1] + b[i]);
                if (interlaced) {
                    uint8_t* n = b + stride;
                    n[0] = uint8_t(b[-stride] - b[w - 1 - step] + b[w - 1] + n[0]);
                    for (int i = 1; i < w; ++i) n[i] = uint8_t(n[i - step] - n[i - 1 - step] + n[i - 1] + n[i]);
                }
            }
        }
    }

    bool decode(const uint8_t* data, size_t size, int64_t stamp, const PictureSink& sink) override {
        const int bits = pro ? 10 : 8;
        if (rgb) out.rgb(width, height, bits, planes == 4);
        else {
            out.shape(width, height, width >> chromaX, height >> chromaY, bits, false);
            out.color = color;
        }
        const uint8_t* start[5] = {};
        uint32_t frameInfo = 0;
        size_t at = 0;
        std::vector<std::pair<const uint8_t*, size_t>> control, stream;
        if (pack) {
            if (size < 8 || data[0] != 1) return false;
            const uint32_t offset = le32(data + 4);
            if (size <= uint64_t(offset) + 8) return false;
            const uint8_t* table = data + 8 + offset;
            const size_t tableSize = size - 8 - offset;
            if (tableSize < 4) return false;
            const uint32_t controlBytes = le32(table);
            if (controlBytes > offset || tableSize < 4 + size_t(planes) * size_t(slices) * 8) return false;
            const uint8_t* p = data + 8;
            const uint8_t* c = p + (offset - controlBytes);
            size_t left = size_t(c - p), entry = 4;
            for (int k = 0; k < planes * slices; ++k, entry += 4) {
                const size_t n = le32(table + entry);
                if (n > left) return false;
                stream.push_back({p, n});
                p += n;
                left -= n;
            }
            left = size_t(data + size - c);
            for (int k = 0; k < planes * slices; ++k, entry += 4) {
                const size_t n = le32(table + entry);
                if (n > left) return false;
                control.push_back({c, n});
                c += n;
                left -= n;
            }
            frameInfo = 2 << 8;
        } else if (pro) {
            if (size < 4) return false;
            frameInfo = le32(data);
            slices = int((frameInfo >> 16) & 0xFF) + 1;
            at = 4;
            for (int i = 0; i < planes; ++i) {
                start[i] = data + at;
                if (size - at < 1024 + 4 * size_t(slices)) return false;
                uint32_t previous = 0, end = 0;
                for (int j = 0; j < slices; ++j) {
                    end = le32(data + at + 4 * size_t(j));
                    if (end < previous || size - at - 4 * size_t(slices) < size_t(end) + 1024) return false;
                    previous = end;
                }
                at += 4 * size_t(slices) + end + 1024;
            }
            start[planes] = data + at;
        } else {
            for (int i = 0; i < planes; ++i) {
                start[i] = data + at;
                if (size - at < 256 + 4 * size_t(slices)) return false;
                uint32_t previous = 0, end = 0;
                for (int j = 0; j < slices; ++j) {
                    end = le32(data + at + 256 + 4 * size_t(j));
                    if (end < previous || size - at - 256 - 4 * size_t(slices) < end) return false;
                    previous = end;
                }
                at += 256 + 4 * size_t(slices) + end;
            }
            start[planes] = data + at;
            if (size - at < frameInfoSize || size - at < 4) return false;
            frameInfo = le32(data + at);
        }
        const int predict = (frameInfo >> 8) & 3;
        for (int i = 0; i < planes; ++i) {
            const ptrdiff_t stride = ptrdiff_t(out.stride(i) / out.unit());
            bool ok;
            if (pack) ok = packed(i, out.data[i].data(), stride, control, stream);
            else if (pro) ok = plane(i, reinterpret_cast<uint16_t*>(out.data[i].data()), stride, start[i], start[i + 1] - 1024, predict == 1, size_t(start[i + 1] - 1024 - start[i]) - 4 * size_t(slices));
            else ok = plane(i, out.data[i].data(), stride, start[i] + 256, start[i], predict == 1, size_t(start[i + 1] - start[i]) - 256 - 4 * size_t(slices));
            if (!ok) return false;
            if (!pro) {
                const int rmode = chromaY && i == 0 ? 1 : 0;
                if (predict == 3) median(out.data[i].data(), stride, planeWidth(i), planeHeight(i), rmode);
                else if (predict == 2) gradient(out.data[i].data(), stride, planeWidth(i), planeHeight(i), rmode);
            }
        }
        if (rgb) {
            for (int y = 0; y < height; ++y) {
                if (pro) {
                    uint16_t* g = out.wide(0, y);
                    uint16_t* b = out.wide(1, y);
                    uint16_t* r = out.wide(2, y);
                    for (int x = 0; x < width; ++x) {
                        r[x] = uint16_t((r[x] + g[x] - 0x200) & 0x3FF);
                        b[x] = uint16_t((b[x] + g[x] - 0x200) & 0x3FF);
                    }
                } else {
                    uint8_t* g = out.row(0, y);
                    uint8_t* b = out.row(1, y);
                    uint8_t* r = out.row(2, y);
                    for (int x = 0; x < width; ++x) {
                        r[x] = uint8_t(r[x] + g[x] - 0x80);
                        b[x] = uint8_t(b[x] + g[x] - 0x80);
                    }
                }
            }
        }
        sink(out.picture(stamp));
        return true;
    }
};

}

std::unique_ptr<SoftDecoder> openUtVideo(const Track& track, std::string* error) {
    auto fail = [&](const char* why) {
        if (error) *error = why;
        return std::unique_ptr<SoftDecoder>();
    };
    auto d = std::make_unique<UtDecoder>();
    d->tag = track.codec;
    d->width = track.width;
    d->height = track.height;
    const std::string name = fourccText(track.codec);
    if (name.size() != 4 || name[0] != 'U') return fail("not a Ut Video track");
    d->pro = name[1] == 'Q';
    d->pack = name[1] == 'M';
    const std::string kind = name.substr(2);
    d->rgb = kind == "RG" || kind == "RA";
    d->planes = kind == "RA" ? 4 : 3;
    d->chromaX = kind == "Y0" || kind == "H0" || kind == "Y2" || kind == "H2" ? 1 : 0;
    d->chromaY = kind == "Y0" || kind == "H0" ? 1 : 0;
    d->color.known = !d->rgb;
    d->color.matrix = kind[0] == 'H' ? 1 : 5;
    d->color.primaries = kind[0] == 'H' ? 1 : 5;
    d->color.transfer = kind[0] == 'H' ? 1 : 6;
    if ((d->width & ((1 << d->chromaX) - 1)) || (d->height & ((1 << d->chromaY) - 1))) return fail("Ut Video with odd picture sizes is not decoded");
    std::vector<uint8_t> extra = track.config;
    if (track.configType == fourcc("bmih")) extra.erase(extra.begin(), extra.begin() + std::min<size_t>(40, extra.size()));
    if (d->pack && extra.size() >= 16) {
        d->slices = extra[9] + 1;
    } else if (!d->pro && !d->pack && extra.size() >= 16) {
        d->frameInfoSize = le32(extra.data() + 8);
        const uint32_t flags = le32(extra.data() + 12);
        d->slices = int(flags >> 24) + 1;
        d->interlaced = flags & 0x800;
    } else if (!(d->pro && extra.size() == 8)) {
        return fail("the Ut Video codec data is missing");
    }
    return d;
}

}
