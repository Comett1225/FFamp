#include "soft_video.h"
#include "vc1.h"
#include <cstring>

namespace absent {

namespace vc1 {

namespace {

constexpr int kFrameMargin = 64;

std::vector<uint8_t> padded(const uint8_t* data, size_t size) {
    std::vector<uint8_t> out(size + Bits::kPadding, 0);
    if (size) memcpy(out.data(), data, size);
    return out;
}

}

bool Vc1::allocate() {
    width = codedWidth;
    height = codedHeight;
    mbWidth = (width + 15) / 16;
    mbHeight = (height + 15) / 16;
    const int aligned = (mbHeight + 1) & ~1;
    mbStride = mbWidth + 1;
    b8Stride = mbWidth * 2 + 1;
    hEdgePos = mbWidth * 16;
    vEdgePos = mbHeight * 16;
    blockWrap[0] = blockWrap[1] = blockWrap[2] = blockWrap[3] = b8Stride;
    blockWrap[4] = blockWrap[5] = mbStride;
    const size_t ySize = size_t(b8Stride) * (2 * aligned + 1);
    const size_t cSize = size_t(mbStride) * (aligned + 1);
    const size_t ycSize = ySize + 2 * cSize + 64;
    dcValMemory.assign(ycSize, 1024);
    dcVal = dcValMemory.data() + b8Stride + 1;
    acValMemory.assign(ycSize * 16, 0);
    acVal = reinterpret_cast<int16_t (*)[16]>(acValMemory.data()) + b8Stride + 1;
    codedBlockMemory.assign(ySize + 64, 0);
    codedBlock = codedBlockMemory.data() + b8Stride + 1;
    const size_t mbArray = size_t(aligned) * mbStride;
    mbskipMemory.assign(mbArray + 2, 0);
    mbskipTable = mbskipMemory.data();
    planes.assign(mbArray * 6, 0);
    mvTypeMbPlane = planes.data();
    directMbPlane = planes.data() + mbArray;
    forwardMbPlane = planes.data() + 2 * mbArray;
    fieldtxPlane = planes.data() + 3 * mbArray;
    acpredPlane = planes.data() + 4 * mbArray;
    overFlagsPlane = planes.data() + 5 * mbArray;
    nAllocatedBlks = mbWidth + 2;
    blockMemory.assign(size_t(nAllocatedBlks) * 6 * 64, 0);
    block = reinterpret_cast<int16_t (*)[6][64]>(blockMemory.data());
    cbpMemory.assign(size_t(3) * mbStride, 0);
    cbpBase = cbpMemory.data();
    cbp = cbpBase + 2 * mbStride;
    ttblkMemory.assign(size_t(3) * mbStride, 0);
    ttblkBase = ttblkMemory.data();
    ttblk = ttblkBase + 2 * mbStride;
    isIntraMemory.assign(size_t(3) * mbStride, 0);
    isIntraBase = isIntraMemory.data();
    isIntra = isIntraBase + 2 * mbStride;
    lumaMvMemory.assign(size_t(3) * mbStride * 2, 0);
    lumaMvBase = reinterpret_cast<int16_t (*)[2]>(lumaMvMemory.data());
    lumaMv = lumaMvBase + 2 * mbStride;
    const size_t typeSize = size_t(b8Stride) * (aligned * 2 + 1) + size_t(mbStride) * (aligned + 1) * 2;
    mbTypeMemory.assign(typeSize + 64, 0);
    mbType = mbTypeMemory.data() + b8Stride + 1;
    blkMvTypeMemory.assign(size_t(b8Stride) * (aligned * 2 + 1) + 64, 0);
    blkMvType = blkMvTypeMemory.data() + b8Stride + 1;
    mvFMemory.assign(2 * typeSize + 64, 0);
    mvF[0] = mvFMemory.data() + b8Stride + 1;
    mvF[1] = mvF[0] + typeSize;
    mvFNextMemory.assign(2 * typeSize + 64, 0);
    mvFNext[0] = mvFNextMemory.data() + b8Stride + 1;
    mvFNext[1] = mvFNext[0] + typeSize;
    lumaLines = ((mbWidth * 16 + 2 * kFrameMargin) + 63) & ~63;
    chromaLines = ((mbWidth * 8 + kFrameMargin) + 63) & ~63;
    edgeEmuStride = 2 * lumaLines;
    edgeEmuBuffer.assign(size_t(edgeEmuStride) * 80, 0);
    pool.clear();
    lastPic.set(nullptr);
    nextPic.set(nullptr);
    curPic.set(nullptr);
    contextReady = true;
    return true;
}

std::shared_ptr<Frame> Vc1::newFrame() {
    for (auto& f : pool) {
        if (f.use_count() == 1) {
            reset(*f);
            return f;
        }
    }
    auto f = std::make_shared<Frame>();
    const int aligned = (mbHeight + 1) & ~1;
    const int rows = aligned * 16 + 2 * kFrameMargin;
    const int chromaRows = aligned * 8 + kFrameMargin;
    f->memory.assign(size_t(lumaLines) * rows + 2 * size_t(chromaLines) * chromaRows + 64, 0);
    f->linesize[0] = lumaLines;
    f->linesize[1] = f->linesize[2] = chromaLines;
    f->data[0] = f->memory.data() + size_t(lumaLines) * kFrameMargin + kFrameMargin;
    uint8_t* chroma = f->memory.data() + size_t(lumaLines) * rows;
    f->data[1] = chroma + size_t(chromaLines) * (kFrameMargin / 2) + kFrameMargin / 2;
    f->data[2] = chroma + size_t(chromaLines) * chromaRows + size_t(chromaLines) * (kFrameMargin / 2) + kFrameMargin / 2;
    const size_t b8Array = size_t(b8Stride) * aligned * 2;
    for (int i = 0; i < 2; i++) {
        f->motionMemory[i].assign(2 * (b8Array + 4) + 2 * size_t(mbStride), 0);
        f->motionVal[i] = reinterpret_cast<int16_t (*)[2]>(f->motionMemory[i].data()) + 4;
    }
    const size_t tableSize = size_t(aligned + 2) * mbStride + 1;
    f->mbTypeMemory.assign(tableSize + 2 * size_t(mbStride), 0);
    f->mbType = f->mbTypeMemory.data() + 2 * mbStride + 1;
    f->qscaleMemory.assign(tableSize + 2 * size_t(mbStride), 0);
    f->qscaleTable = f->qscaleMemory.data() + 2 * mbStride + 1;
    pool.push_back(f);
    return f;
}

void Vc1::reset(Frame& f) {
    for (auto& m : f.motionMemory) std::fill(m.begin(), m.end(), int16_t(0));
    f.interlaced = false;
    f.fieldPicture = false;
}

void Vc1::grey(Frame& f) {
    const int aligned = (mbHeight + 1) & ~1;
    for (int y = 0; y < aligned * 16; y++) memset(f.data[0] + ptrdiff_t(y) * f.linesize[0], 0x80, size_t(mbWidth) * 16);
    for (int p = 1; p < 3; p++) {
        for (int y = 0; y < aligned * 8; y++) memset(f.data[p] + ptrdiff_t(y) * f.linesize[p], 0x80, size_t(mbWidth) * 8);
    }
}

bool Vc1::frameStart() {
    curPic.set(newFrame());
    linesize = curPic.linesize[0];
    uvlinesize = curPic.linesize[1];
    if (pictType != kPictureB) {
        lastPic.set(nextPic.ptr);
        nextPic.set(curPic.ptr);
    }
    if (!lastPic.ptr && pictType != kPictureI) {
        auto dummy = newFrame();
        grey(*dummy);
        lastPic.set(dummy);
    }
    if (!nextPic.ptr && pictType == kPictureB) nextPic.set(newFrame());
    return true;
}

bool Vc1::start(const uint8_t* config, size_t size, bool wmv3, int w, int h) {
    vc1dsp = dsp();
    currUseIc = &auxUseIc;
    pq = uint8_t(-1);
    mvrange = 0;
    currLuty = lastLuty;
    currLutuv = lastLutuv;
    if (wmv3) {
        auto bytes = padded(config, size);
        Bits bits;
        bits.reset(bytes.data(), size);
        if (decodeSequenceHeader(bits) < 0) return false;
        if (profile == kProfileAdvanced) return false;
        codedWidth = w;
        codedHeight = h;
    } else {
        if (size < 16) return false;
        std::vector<uint8_t> buffer(size + Bits::kPadding, 0);
        const uint8_t* end = config + size;
        const uint8_t* next = findNextMarker(config, end);
        for (const uint8_t* at = next; next < end; at = next) {
            next = findNextMarker(at + 4, end);
            const int length = int(next - at - 4);
            if (length <= 0) continue;
            std::fill(buffer.begin(), buffer.end(), uint8_t(0));
            const int unescaped = unescapeBuffer(at + 4, length, buffer.data());
            Bits bits;
            bits.reset(buffer.data(), size_t(unescaped));
            const uint32_t code = readBe32(at);
            if (code == kCodeSequence) {
                if (decodeSequenceHeader(bits) < 0) return false;
                sequenceSeen = true;
            } else if (code == kCodeEntryPoint) {
                if (decodeEntryPoint(bits) < 0) return false;
                entrySeen = true;
            }
        }
        if (!sequenceSeen || !entrySeen || profile != kProfileAdvanced) return false;
    }
    if (resSprite || resX8) return false;
    if (profile == kProfileAdvanced || resFasttx) {
        for (int i = 0; i < 64; i++) {
            auto transpose = [](int x) { return (x >> 3) | ((x & 7) << 3); };
            for (int k = 0; k < 4; k++) zz8x8[k][i] = uint8_t(transpose(kWmv1Scantable[k][i]));
            zzi8x8[i] = uint8_t(transpose(kAdvInterlaced8x8Zz[i]));
        }
        leftBlkSh = 0;
        topBlkSh = 3;
    } else {
        memcpy(zz8x8, kWmv1Scantable, sizeof zz8x8);
        leftBlkSh = 3;
        topBlkSh = 0;
        simpleIdct(vc1dsp);
    }
    lowDelay = !maxBFrames;
    mbWidth = (codedWidth + 15) >> 4;
    mbHeight = (codedHeight + 15) >> 4;
    return codedWidth > 0 && codedHeight > 0 && codedWidth <= 4096 && codedHeight <= 4096;
}

bool Vc1::decodeFrame(const uint8_t* buf, size_t size) {
    struct Slice {
        std::vector<uint8_t> buf;
        Bits gb;
        int mbyStart = 0;
    };
    std::vector<Slice> slices;
    int nSlices1 = -1;
    secondField = 0;
    broken = false;
    if (size >= 4 && readBe32(buf + size - 4) == kCodeEndOfSequence) size -= 4;
    if (size == 0) return true;
    std::vector<uint8_t> buf2;
    if (profile == kProfileAdvanced) {
        buf2.assign(size + Bits::kPadding, 0);
        int size2 = 0;
        auto addSlice = [&](const uint8_t* from, int length, int start) -> Slice& {
            slices.emplace_back();
            Slice& s = slices.back();
            s.buf.assign(size_t(length) + Bits::kPadding, 0);
            const int unescaped = unescapeBuffer(from, length, s.buf.data());
            s.gb.reset(s.buf.data(), size_t(unescaped));
            s.mbyStart = start;
            return s;
        };
        if (size >= 4 && (readBe32(buf) & ~0xFFu) == 0x100) {
            const uint8_t* end = buf + size;
            const uint8_t* next = buf;
            for (const uint8_t* start = buf; next < end; start = next) {
                next = findNextMarker(start + 4, end);
                const int length = int(next - start - 4);
                if (length <= 0) continue;
                switch (readBe32(start)) {
                case kCodeFrame:
                    size2 = unescapeBuffer(start + 4, length, buf2.data());
                    break;
                case kCodeField:
                    addSlice(start + 4, length, (codedHeight + 31) >> 5);
                    nSlices1 = int(slices.size()) - 2;
                    break;
                case kCodeEntryPoint: {
                    size2 = unescapeBuffer(start + 4, length, buf2.data());
                    Bits bits;
                    bits.reset(buf2.data(), size_t(size2));
                    decodeEntryPoint(bits);
                    break;
                }
                case kCodeSequence: {
                    std::vector<uint8_t> header(size_t(length) + Bits::kPadding, 0);
                    const int unescaped = unescapeBuffer(start + 4, length, header.data());
                    Bits bits;
                    bits.reset(header.data(), size_t(unescaped));
                    decodeSequenceHeader(bits);
                    break;
                }
                case kCodeSlice: {
                    Slice& s = addSlice(start + 4, length, 0);
                    s.mbyStart = int(s.gb.bits(9));
                    break;
                }
                }
            }
        } else if (interlace && ((buf[0] & 0xC0) == 0xC0)) {
            const uint8_t* divider = findNextMarker(buf, buf + size);
            if (divider == buf + size || readBe32(divider) != kCodeField) return false;
            addSlice(divider + 4, int(buf + size - divider - 4), (mbHeight + 1) >> 1);
            nSlices1 = int(slices.size()) - 2;
            size2 = unescapeBuffer(buf, int(divider - buf), buf2.data());
        } else {
            size2 = unescapeBuffer(buf, int(size), buf2.data());
        }
        gb.reset(buf2.data(), size_t(size2));
    } else {
        buf2 = padded(buf, size);
        gb.reset(buf2.data(), size);
    }
    const int nSlices = int(slices.size());
    if (contextReady && (width != codedWidth || height != codedHeight)) contextReady = false;
    if (!contextReady) {
        if (codedWidth <= 1 || codedHeight <= 1 || !allocate()) return false;
        if (profile == kProfileAdvanced) {
            hEdgePos = codedWidth;
            vEdgePos = codedHeight;
        }
    }
    picHeaderFlag = 0;
    firstPicHeaderFlag = 1;
    if (profile < kProfileAdvanced) {
        if (parseFrameHeader(gb) < 0) return false;
    } else {
        if (parseFrameHeaderAdv(gb) < 0) return false;
    }
    firstPicHeaderFlag = 0;
    if ((mbHeight >> fieldMode) == 0) return false;
    if (!lastPic.ptr && pictType == kPictureB) return false;
    if (!frameStart()) return false;
    curPic.ptr->fieldPicture = fieldMode;
    curPic.ptr->interlaced = fcm != kProgressive;
    lastInterlaced = lastPic.ptr ? lastPic.ptr->interlaced : 0;
    nextInterlaced = nextPic.ptr ? nextPic.ptr->interlaced : 0;
    endMbX = mbWidth;
    if (fieldMode) {
        for (int i = 0; i < 3; i++) curPic.linesize[i] <<= 1;
        linesize <<= 1;
        uvlinesize <<= 1;
    }
    const int rows = mbHeight >> fieldMode;
    int headerResult = 0;
    for (int i = 0; i <= nSlices; i++) {
        if (i > 0 && slices[size_t(i - 1)].mbyStart >= rows) {
            if (fieldMode <= 0) continue;
            secondField = 1;
            blocksOff = b8Stride * (mbHeight & ~1);
            mbOff = mbStride * mbHeight >> 1;
        } else {
            secondField = 0;
            blocksOff = 0;
            mbOff = 0;
        }
        if (i) {
            picHeaderFlag = 0;
            if (fieldMode && i == nSlices1 + 2) {
                if ((headerResult = parseFrameHeaderAdv(gb)) < 0) continue;
            } else if (gb.bit()) {
                picHeaderFlag = 1;
                if ((headerResult = parseFrameHeaderAdv(gb)) < 0) continue;
            }
        }
        if (headerResult < 0) continue;
        startMbY = i == 0 ? 0 : std::max(0, slices[size_t(i - 1)].mbyStart % rows);
        if (!fieldMode || secondField) {
            endMbY = i == nSlices ? rows : std::min(rows, slices[size_t(i)].mbyStart % rows);
        } else {
            if (i >= nSlices) continue;
            endMbY = i == nSlices1 + 1 ? rows : std::min(rows, slices[size_t(i)].mbyStart % rows);
        }
        if (endMbY <= startMbY) continue;
        if (((pictType == kPictureP && !pFrameSkipped) || (pictType == kPictureB && !biType)) && !cbpcyVlc) continue;
        decodeBlocks();
        if (i != nSlices) gb = slices[size_t(i)].gb;
    }
    if (fieldMode) {
        secondField = 0;
        for (int i = 0; i < 3; i++) curPic.linesize[i] >>= 1;
        linesize >>= 1;
        uvlinesize >>= 1;
        if (pictType != kPictureBI && pictType != kPictureB) {
            std::swap(mvFNext[0], mvF[0]);
            std::swap(mvFNext[1], mvF[1]);
        }
    }
    return !broken;
}

}

namespace {

struct Vc1Decoder : SoftDecoder {
    std::unique_ptr<vc1::Vc1> v;
    std::vector<uint8_t> config;
    std::vector<uint8_t> mapped[3];
    bool wmv3 = false;
    int width = 0;
    int height = 0;

    const char* name() const override { return "vc1"; }

    bool open() {
        v = std::make_unique<vc1::Vc1>();
        return v->start(config.data(), config.size(), wmv3, width, height);
    }

    void reset() override { open(); }

    bool decode(const uint8_t* data, size_t size, int64_t stamp, const PictureSink& shown) override {
        if (!v->decodeFrame(data, size)) return false;
        if (!v->curPic.ptr) return true;
        Picture p;
        p.stamp = stamp;
        p.width = std::min(width, v->codedWidth);
        p.height = std::min(height, v->codedHeight);
        p.chromaWidth = (p.width + 1) / 2;
        p.chromaHeight = (p.height + 1) / 2;
        for (int i = 0; i < 3; i++) {
            p.plane[i] = v->curPic.ptr->data[i];
            p.stride[i] = size_t(v->curPic.ptr->linesize[i]);
            const int factor = v->profile != vc1::kProfileAdvanced ? 0 : i ? (v->rangeMapuvFlag ? v->rangeMapuv + 9 : 0) : (v->rangeMapyFlag ? v->rangeMapy + 9 : 0);
            if (!factor) continue;
            const int columns = i ? p.chromaWidth : p.width, rows = i ? p.chromaHeight : p.height;
            mapped[i].resize(size_t(columns) * size_t(rows));
            for (int y = 0; y < rows; y++) {
                const uint8_t* from = p.plane[i] + size_t(y) * p.stride[i];
                uint8_t* to = mapped[i].data() + size_t(y) * size_t(columns);
                for (int x = 0; x < columns; x++) to[x] = vc1::clipU8((((from[x] - 128) * factor + 4) >> 3) + 128);
            }
            p.plane[i] = mapped[i].data();
            p.stride[i] = size_t(columns);
        }
        shown(p);
        return true;
    }
};

}

std::unique_ptr<SoftDecoder> openVc1(uint32_t codec, const std::vector<uint8_t>& config, int width, int height, std::string* error) {
    auto fail = [&](const char* text) -> std::unique_ptr<SoftDecoder> {
        if (error) *error = text;
        return nullptr;
    };
    if (config.size() < 40 || width <= 0 || height <= 0 || width > 4096 || height > 4096) return fail("the VC-1 stream has no usable header");
    auto decoder = std::make_unique<Vc1Decoder>();
    decoder->wmv3 = codec == fourcc("WMV3");
    decoder->config.assign(config.begin() + 40, config.end());
    decoder->width = width;
    decoder->height = height;
    if (!decoder->open()) return fail(decoder->v->resX8 ? "this VC-1 stream uses X8 intra pictures (Windows Media Video 9 Complex profile), which are not decoded yet" : "the VC-1 sequence header is not supported");
    return decoder;
}

}
