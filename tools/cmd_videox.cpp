#include "cli.h"
#include <CoreVideo/CoreVideo.h>
#include <VideoToolbox/VideoToolbox.h>
#include <cstring>
#include <random>

namespace cli {

namespace {

struct IvfWriter : Writer {
    FILE* file = nullptr;
    uint32_t codec = 0;
    uint32_t count = 0;
    bool failed = false;

    ~IvfWriter() override {
        if (file) fclose(file);
    }
    void little(uint64_t value, int bytes) {
        for (int i = 0; i < bytes; ++i) fputc(int((value >> (8 * i)) & 0xFF), file);
    }
    int add(const TrackSpec& spec) override {
        if (codec || spec.kind != Kind::Video || (spec.codec != fourcc("av01") && spec.codec != fourcc("vp09")) || spec.width > 65535 || spec.height > 65535) {
            failed = true;
            return -1;
        }
        codec = spec.codec;
        fwrite("DKIF", 1, 4, file);
        little(0, 2);
        little(32, 2);
        fwrite(codec == fourcc("av01") ? "AV01" : "VP90", 1, 4, file);
        little(uint64_t(spec.width), 2);
        little(uint64_t(spec.height), 2);
        little(spec.timescale, 4);
        little(1, 4);
        little(0, 8);
        return 0;
    }
    void configure(int, uint32_t, const std::vector<uint8_t>&) override {}
    bool write(int track, const uint8_t* data, size_t size, int64_t pts, int64_t, uint32_t, bool) override {
        if (track != 0 || failed || size > 0x7FFFFFF0u) return false;
        const bool delimiter = codec == fourcc("av01");
        little(size + (delimiter ? 2 : 0), 4);
        little(uint64_t(pts), 8);
        if (delimiter) fwrite("\x12\x00", 1, 2, file);
        ++count;
        return fwrite(data, 1, size, file) == size;
    }
    bool finish(std::string* error) override {
        bool ok = !failed && codec && fseeko(file, 24, SEEK_SET) == 0;
        if (ok) little(count, 4);
        ok = fclose(file) == 0 && ok;
        file = nullptr;
        if (!ok && error) *error = "IVF takes one AV1 or VP9 video track and nothing else";
        return ok;
    }
};

bool endsWith(const std::string& text, const char* tail) {
    const size_t n = strlen(tail);
    return text.size() >= n && text.compare(text.size() - n, n, tail) == 0;
}

int videoinfo(const Args& args) {
    if (args.plain.empty()) return fail("videoinfo needs a file");
    std::string error;
    auto reader = Reader::open(args.plain[0], &error);
    if (!reader) return fail(error);
    const Track* track = reader->first(Kind::Video);
    if (!track) return fail("no video track");
    auto decoder = VideoDecoder::open(*reader, *track, &error, args.has("software"));
    if (!decoder) return fail(error);
    Frame frame = decoder->frame(0);
    if (!frame) return fail("frame 0 did not decode");
    printf("format %s codec %s backend %s size %dx%d bits %d chroma %dx%d frames %u primaries %d transfer %d matrix %d range %s\n", reader->format(),
        fourccText(track->codec).c_str(), decoder->backend(), frame.width, frame.height, frame.bits, frame.chromaWidth, frame.chromaHeight, decoder->frames(),
        frame.color.primaries, frame.color.transfer, frame.color.matrix, frame.color.full ? "full" : "limited");
    return 0;
}

CVPixelBufferRef surfaceBuffer(int width, int height, OSType format) {
    const void* keys[] = {kCVPixelBufferIOSurfacePropertiesKey};
    CFDictionaryRef empty = CFDictionaryCreate(nullptr, nullptr, nullptr, 0, &kCFTypeDictionaryKeyCallBacks, &kCFTypeDictionaryValueCallBacks);
    const void* values[] = {empty};
    CFDictionaryRef attributes = CFDictionaryCreate(nullptr, keys, values, 1, &kCFTypeDictionaryKeyCallBacks, &kCFTypeDictionaryValueCallBacks);
    CVPixelBufferRef buffer = nullptr;
    CVPixelBufferCreate(nullptr, size_t(width), size_t(height), format, attributes, &buffer);
    CFRelease(attributes);
    CFRelease(empty);
    return buffer;
}

void testCard(std::vector<uint8_t>& pixels, int width, int height) {
    std::mt19937 random(7);
    pixels.resize(size_t(width) * size_t(height) * 4);
    for (int y = 0; y < height; ++y) {
        for (int x = 0; x < width; ++x) {
            uint8_t* p = &pixels[(size_t(y) * size_t(width) + size_t(x)) * 4];
            const int band = y * 4 / height;
            if (band == 0) {
                p[0] = uint8_t(random());
                p[1] = uint8_t(random());
                p[2] = uint8_t(random());
            } else if (band == 1) {
                p[0] = uint8_t(x * 255 / std::max(width - 1, 1));
                p[1] = uint8_t(255 - p[0]);
                p[2] = uint8_t((y * 8) & 255);
            } else if (band == 2) {
                const int bar = x * 8 / width;
                p[0] = bar & 1 ? 255 : 0;
                p[1] = bar & 4 ? 255 : 0;
                p[2] = bar & 2 ? 255 : 0;
            } else {
                const uint8_t grey = uint8_t(x * 255 / std::max(width - 1, 1));
                p[0] = p[1] = p[2] = (x / 2 + y) % 2 ? grey : uint8_t(255 - grey);
            }
            p[3] = 255;
        }
    }
}

int pixels(const Args& args) {
    int width = 320, height = 180;
    if (args.has("size") && (sscanf(args.get("size").c_str(), "%dx%d", &width, &height) != 2 || width <= 0 || height <= 0 || width > 8192 || height > 8192)) return fail("bad --size");
    std::vector<uint8_t> source;
    testCard(source, width, height);
    const size_t cw = size_t((width + 1) / 2), ch = size_t((height + 1) / 2), luma = size_t(width) * size_t(height);
    std::vector<uint8_t> ours(luma + 2 * cw * ch);
    FILE* in = args.has("video") ? fopen(args.get("video").c_str(), "rb") : nullptr;
    FILE* out = args.has("out") ? fopen(args.get("out").c_str(), "wb") : nullptr;
    if ((args.has("video") && !in) || (args.has("out") && !out)) return fail("cannot open the files");
    for (long n = 0;; ++n) {
        if (in && fread(source.data(), 1, source.size(), in) != source.size()) {
            if (n == 0) return fail("cannot read a BGRA frame of that size");
            break;
        }
        bgraToI420(source.data(), size_t(width) * 4, width, height, ours.data());
        if (out && fwrite(ours.data(), 1, ours.size(), out) != ours.size()) return fail("cannot write the output");
        if (!in || !out) break;
    }
    if (in) fclose(in);
    if (out) fclose(out);
    if (args.has("no-system")) return 0;

    CVPixelBufferRef from = surfaceBuffer(width, height, kCVPixelFormatType_32BGRA);
    CVPixelBufferRef to = surfaceBuffer(width, height, kCVPixelFormatType_420YpCbCr8BiPlanarVideoRange);
    VTPixelTransferSessionRef transfer = nullptr;
    if (!from || !to || VTPixelTransferSessionCreate(nullptr, &transfer) != noErr) return fail("the system converter did not start");
    CVPixelBufferLockBaseAddress(from, 0);
    for (int y = 0; y < height; ++y) memcpy(static_cast<uint8_t*>(CVPixelBufferGetBaseAddress(from)) + size_t(y) * CVPixelBufferGetBytesPerRow(from), &source[size_t(y) * size_t(width) * 4], size_t(width) * 4);
    CVPixelBufferUnlockBaseAddress(from, 0);
    for (CVPixelBufferRef buffer : {from, to}) {
        CVBufferSetAttachment(buffer, kCVImageBufferColorPrimariesKey, kCVImageBufferColorPrimaries_ITU_R_709_2, kCVAttachmentMode_ShouldPropagate);
        CVBufferSetAttachment(buffer, kCVImageBufferTransferFunctionKey, kCVImageBufferTransferFunction_ITU_R_709_2, kCVAttachmentMode_ShouldPropagate);
    }
    CVBufferSetAttachment(to, kCVImageBufferYCbCrMatrixKey, kCVImageBufferYCbCrMatrix_ITU_R_709_2, kCVAttachmentMode_ShouldPropagate);
    VTSessionSetProperty(transfer, kVTPixelTransferPropertyKey_DestinationYCbCrMatrix, kCVImageBufferYCbCrMatrix_ITU_R_709_2);
    if (VTPixelTransferSessionTransferImage(transfer, from, to) != noErr) return fail("the system converter failed");
    Frame frame;
    frame.native = std::shared_ptr<void>(to, [](void* p) { CFRelease(p); });
    frame.width = width;
    frame.height = height;
    std::vector<uint8_t> system;
    if (!copyI420(frame, system) || system.size() != ours.size()) return fail("the system converter gave an unexpected picture");
    CFRelease(from);
    CFRelease(transfer);
    int worst[3] = {0, 0, 0};
    uint64_t off[3] = {0, 0, 0}, beyond = 0;
    for (size_t i = 0; i < ours.size(); ++i) {
        const int plane = i < luma ? 0 : i < luma + cw * ch ? 1 : 2;
        const int gap = abs(int(ours[i]) - int(system[i]));
        worst[plane] = std::max(worst[plane], gap);
        if (gap) ++off[plane];
        if (gap > 1) ++beyond;
    }
    printf("pixels %dx%d: largest difference Y %d U %d V %d, differing samples Y %llu U %llu V %llu, beyond one code value %llu\n", width, height, worst[0], worst[1], worst[2],
        (unsigned long long)off[0], (unsigned long long)off[1], (unsigned long long)off[2], (unsigned long long)beyond);
    return beyond ? 1 : 0;
}

int encodeSurface(const Args& args) {
    if (args.plain.empty() || !args.has("video") || !args.has("size")) return fail("encode-surface needs an output file, --video raw.bgra and --size WxH");
    int width = 0, height = 0;
    if (sscanf(args.get("size").c_str(), "%dx%d", &width, &height) != 2 || width <= 0 || height <= 0 || width > 16384 || height > 16384) return fail("bad --size");
    FILE* video = fopen(args.get("video").c_str(), "rb");
    if (!video) return fail("cannot open the raw video");
    uint32_t codec = videoCodec(args);
    if (!codec) return fail("--codec is h264, hevc, av1, vp9 or apv");
    int fps = int(args.number("fps", 30));
    if (fps <= 0) return fail("bad --fps");
    EncodeSpec spec;
    spec.width = width;
    spec.height = height;
    spec.timescale = uint32_t(fps);
    spec.fps = fps;
    spec.bitrate = int(args.number("bitrate", 8000000));
    spec.keyInterval = int(args.number("gop", fps));
    spec.codec = codec;
    spec.speed = int(args.number("speed", -1));
    spec.bgra = true;
    std::string error;
    auto encoder = VideoEncoder::open(spec, &error);
    if (!encoder) return fail(error);
    auto writer = openOutput(args.plain[0], &error);
    if (!writer) return fail(error);
    TrackSpec picture;
    picture.kind = Kind::Video;
    picture.codec = codec;
    picture.timescale = uint32_t(fps);
    picture.width = width;
    picture.height = height;
    int track = writer->add(picture);
    bool configured = false;
    auto drain = [&] {
        Packet packet;
        while (encoder->take(packet)) {
            if (!configured) writer->configure(track, encoder->configType(), encoder->config());
            configured = true;
            writer->write(track, packet.data.data(), packet.data.size(), packet.pts, packet.dts, packet.duration ? packet.duration : 1, packet.sync);
        }
    };
    std::vector<uint8_t> row(size_t(width) * 4);
    double start = now();
    long done = 0;
    for (;; ++done) {
        CVPixelBufferRef buffer = surfaceBuffer(width, height, kCVPixelFormatType_32BGRA);
        if (!buffer) return fail("cannot make a surface");
        CVPixelBufferLockBaseAddress(buffer, 0);
        bool whole = true;
        for (int y = 0; y < height && whole; ++y) {
            whole = fread(row.data(), 1, row.size(), video) == row.size();
            if (whole) memcpy(static_cast<uint8_t*>(CVPixelBufferGetBaseAddress(buffer)) + size_t(y) * CVPixelBufferGetBytesPerRow(buffer), row.data(), row.size());
        }
        CVPixelBufferUnlockBaseAddress(buffer, 0);
        bool ok = !whole || encoder->encodeSurface(CVPixelBufferGetIOSurface(buffer), done, 1);
        CFRelease(buffer);
        if (!whole) break;
        if (!ok) return fail("the encoder refused a surface");
        drain();
    }
    if (!encoder->finish()) return fail("the encoder failed");
    drain();
    if (!writer->finish(&error)) return fail(error);
    double spent = now() - start;
    fprintf(stderr, "encoded %ld surfaces with %s in %.3f s (%.1f fps)\n", done, encoder->backend(), spent, done / spent);
    return done > 0 ? 0 : 1;
}

Register videoinfoCommand("videoinfo", videoinfo);
Register pixelsCommand("pixels", pixels);
Register encodeSurfaceCommand("encode-surface", encodeSurface);

}

std::unique_ptr<Writer> openOutput(const std::string& path, std::string* error) {
    if (!endsWith(path, ".ivf")) return Writer::open(path, error);
    std::unique_ptr<IvfWriter> writer(new IvfWriter);
    writer->file = fopen(path.c_str(), "wb");
    if (!writer->file) {
        if (error) *error = "cannot create the output file";
        return nullptr;
    }
    return writer;
}

}
