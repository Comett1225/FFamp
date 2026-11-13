#include "../soft_video.h"
#ifdef ABSENT_OAPV
#include <algorithm>
#include <cstring>
#include <oapv/oapv.h>

namespace absent {

namespace {

const int kMacro = 16;
const int kMostSide = 16384;
const int kSizeField = 4;

int padded(int value) { return (value + kMacro - 1) / kMacro * kMacro; }

struct Image {
    oapv_imgb_t image = {};
    std::vector<uint16_t> planes[3];

    bool shape(int width, int height, int space) {
        const int format = OAPV_CS_GET_FORMAT(space);
        const bool grey = format == OAPV_CF_YCBCR400;
        const bool half = format == OAPV_CF_YCBCR422;
        if (!grey && !half && format != OAPV_CF_YCBCR444) return false;
        if (OAPV_CS_GET_BYTE_DEPTH(space) != 2 || width < 1 || height < 1 || width > kMostSide || height > kMostSide) return false;
        image = {};
        image.cs = space;
        image.np = grey ? 1 : 3;
        for (int i = 0; i < image.np; ++i) {
            const int across = i && half ? (width + 1) / 2 : width;
            const int wide = i && half ? padded(width) / 2 : padded(width);
            planes[i].assign(size_t(wide) * size_t(padded(height)), 0);
            image.w[i] = across;
            image.h[i] = height;
            image.aw[i] = wide;
            image.ah[i] = padded(height);
            image.s[i] = wide * 2;
            image.e[i] = padded(height);
            image.a[i] = image.baddr[i] = planes[i].data();
            image.bsize[i] = int(planes[i].size() * 2);
        }
        return true;
    }
};

struct ApvEncoder : SoftEncoder {
    oapve_t handle = nullptr;
    Image image;
    std::vector<uint8_t> coded;
    std::vector<uint8_t> record;
    int width = 0;
    int height = 0;

    ~ApvEncoder() override { if (handle) oapve_delete(handle); }
    const char* name() const override { return "openapv"; }
    uint32_t configType() const override { return fourcc("apvC"); }
    std::vector<uint8_t> config() const override { return record; }

    void fill(const Picture& p) {
        uint16_t* luma = image.planes[0].data();
        const size_t lumaStride = size_t(image.image.aw[0]);
        for (int y = 0; y < height; ++y) {
            const uint8_t* from = p.plane[0] + size_t(y) * p.stride[0];
            uint16_t* to = luma + size_t(y) * lumaStride;
            for (int x = 0; x < width; ++x) to[x] = uint16_t(from[x] << 2);
        }
        const size_t chromaStride = size_t(image.image.aw[1]);
        const int across = std::min(p.chromaWidth, image.image.w[1]);
        for (int plane = 1; plane < 3; ++plane) {
            uint16_t* chroma = image.planes[plane].data();
            for (int y = 0; y < height; ++y) {
                const int near = std::min(y / 2, p.chromaHeight - 1);
                const int far = std::clamp(y & 1 ? near + 1 : near - 1, 0, p.chromaHeight - 1);
                const uint8_t* a = p.plane[plane] + size_t(near) * p.stride[plane];
                const uint8_t* b = p.plane[plane] + size_t(far) * p.stride[plane];
                uint16_t* to = chroma + size_t(y) * chromaStride;
                for (int x = 0; x < across; ++x) to[x] = uint16_t(3 * a[x] + b[x]);
            }
        }
    }

    bool code(oapve_stat_t& stat) {
        oapv_frms_t frames = {};
        frames.num_frms = 1;
        frames.frm[0].imgb = &image.image;
        frames.frm[0].pbu_type = OAPV_PBU_TYPE_PRIMARY_FRAME;
        frames.frm[0].group_id = 1;
        oapv_bitb_t bits = {};
        bits.addr = coded.data();
        bits.bsize = int(coded.size());
        stat = {};
        return OAPV_SUCCEEDED(oapve_encode(handle, &frames, nullptr, &bits, &stat, nullptr)) && stat.write > kSizeField + 4 && size_t(stat.write) <= coded.size()
            && memcmp(coded.data() + kSizeField, "aPv1", 4) == 0;
    }

    bool encode(const Picture& p, int64_t pts, int64_t duration, const PacketSink& sink) override {
        if (p.bits != 8 || p.interleaved || p.width != width || p.height != height || p.chromaHeight < 1) return false;
        fill(p);
        oapve_stat_t stat;
        if (!code(stat)) return false;
        Packet packet;
        packet.data.assign(coded.begin(), coded.begin() + stat.write);
        packet.pts = packet.dts = pts;
        packet.duration = uint32_t(std::max<int64_t>(duration, 0));
        packet.sync = true;
        sink(std::move(packet));
        return true;
    }

    bool finish(const PacketSink&) override { return true; }

    bool start(const EncodeSpec& spec) {
        oapve_cdesc_t settings = {};
        oapve_param_t& p = settings.param[0];
        if (OAPV_FAILED(oapve_param_default(&p))) return false;
        p.profile_idc = OAPV_PROFILE_422_10;
        p.w = spec.width;
        p.h = spec.height;
        p.fps_num = int(spec.fps * 1000 + 0.5);
        p.fps_den = 1000;
        p.rc_type = OAPV_RC_ABR;
        p.bitrate = std::max(spec.bitrate / 1000, 1);
        p.preset = spec.speed < 0 ? OAPV_PRESET_DEFAULT : std::clamp(OAPV_PRESET_PLACEBO - spec.speed, OAPV_PRESET_FASTEST, OAPV_PRESET_PLACEBO);
        p.color_description_present_flag = 1;
        p.color_primaries = p.transfer_characteristics = p.matrix_coefficients = 1;
        p.full_range_flag = 0;
        settings.max_bs_buf_size = int(coded.size());
        settings.max_num_frms = 1;
        settings.threads = OAPV_CDESC_THREADS_AUTO;
        if (handle) oapve_delete(handle);
        handle = oapve_create(&settings, nullptr);
        return handle != nullptr;
    }

    bool describe() {
        oapve_stat_t stat;
        if (!code(stat) || stat.aui.num_frms < 1) return false;
        const oapv_frm_info_t& f = stat.aui.frm_info[0];
        auto wide = [&](uint32_t value) { for (int i = 3; i >= 0; --i) record.push_back(uint8_t(value >> (8 * i))); };
        record = {0, 0, 0, 0, 1, 1, OAPV_PBU_TYPE_PRIMARY_FRAME, 1, 2, uint8_t(f.profile_idc), uint8_t(f.level_idc), uint8_t(f.band_idc)};
        wide(uint32_t(f.w));
        wide(uint32_t(f.h));
        record.push_back(uint8_t((f.chroma_format_idc << 4) | (f.bit_depth - 8)));
        record.push_back(uint8_t(f.capture_time_distance));
        record.insert(record.end(), {1, 1, 1, 0});
        return true;
    }
};

struct ApvDecoder : SoftDecoder {
    oapvd_t handle = nullptr;
    Image image;
    int width = 0;
    int height = 0;
    int space = 0;

    ~ApvDecoder() override { if (handle) oapvd_delete(handle); }
    const char* name() const override { return "openapv"; }
    void reset() override {}

    bool decode(const uint8_t* data, size_t size, const PictureSink& sink) override {
        if (size < 8 || size > 0x7FFFFFFFu) return false;
        if (memcmp(data, "aPv1", 4) != 0) {
            data += kSizeField;
            size -= kSizeField;
        }
        oapv_au_info_t info = {};
        if (OAPV_FAILED(oapvd_info(const_cast<uint8_t*>(data), int(size), &info)) || info.num_frms != 1) return false;
        const oapv_frm_info_t& f = info.frm_info[0];
        if (f.w != width || f.h != height || f.cs != space) {
            if (!image.shape(f.w, f.h, f.cs)) return false;
            width = f.w;
            height = f.h;
            space = f.cs;
        }
        oapv_frms_t frames = {};
        frames.num_frms = 1;
        frames.frm[0].imgb = &image.image;
        oapv_bitb_t bits = {};
        bits.addr = const_cast<uint8_t*>(data);
        bits.bsize = bits.ssize = int(size);
        oapvd_stat_t stat = {};
        if (OAPV_FAILED(oapvd_decode(handle, &bits, &frames, nullptr, &stat))) return false;
        Picture p;
        p.width = width;
        p.height = height;
        p.bits = OAPV_CS_GET_BIT_DEPTH(space);
        p.chromaWidth = image.image.np == 3 ? image.image.w[1] : 0;
        p.chromaHeight = image.image.np == 3 ? height : 0;
        for (int i = 0; i < image.image.np; ++i) {
            p.plane[i] = reinterpret_cast<const uint8_t*>(image.planes[i].data());
            p.stride[i] = size_t(image.image.s[i]);
        }
        p.color.known = f.color_description_present_flag != 0;
        p.color.primaries = p.color.known ? f.color_primaries : 2;
        p.color.transfer = p.color.known ? f.transfer_characteristics : 2;
        p.color.matrix = p.color.known ? f.matrix_coefficients : 2;
        p.color.full = p.color.known && f.full_range_flag;
        if (image.image.np != 3) return false;
        sink(p);
        return true;
    }
};

}

std::unique_ptr<SoftEncoder> openApvEncoder(const EncodeSpec& spec, std::string* error) {
    auto fail = [&](const char* text) {
        if (error) *error = text;
        return nullptr;
    };
    if (spec.width < 2 || spec.height < 1 || spec.width > kMostSide || spec.height > kMostSide || (spec.width & 1)) return fail("APV needs an even width");
    std::unique_ptr<ApvEncoder> encoder(new ApvEncoder);
    encoder->width = spec.width;
    encoder->height = spec.height;
    if (!encoder->image.shape(spec.width, spec.height, OAPV_CS_YCBCR422_10LE)) return fail("the APV encoder refused this size");
    for (int i = 0; i < 3; ++i) std::fill(encoder->image.planes[i].begin(), encoder->image.planes[i].end(), uint16_t(i ? 512 : 64));
    encoder->coded.resize(size_t(padded(spec.width)) * size_t(padded(spec.height)) * 6 + 65536);
    if (encoder->coded.size() > 0x7FFFFFFFu) return fail("the APV encoder refused this size");
    if (!encoder->start(spec) || !encoder->describe() || !encoder->start(spec)) return fail("the APV encoder refused these settings");
    return encoder;
}

std::unique_ptr<SoftDecoder> openApvDecoder(std::string* error) {
    std::unique_ptr<ApvDecoder> decoder(new ApvDecoder);
    oapvd_cdesc_t settings = {};
    settings.threads = OAPV_CDESC_THREADS_AUTO;
    decoder->handle = oapvd_create(&settings, nullptr);
    if (!decoder->handle) {
        if (error) *error = "the APV decoder did not start";
        return nullptr;
    }
    return decoder;
}

}
#else

namespace absent {

std::unique_ptr<SoftEncoder> openApvEncoder(const EncodeSpec&, std::string* error) {
    if (error) *error = "built without an APV encoder";
    return nullptr;
}

std::unique_ptr<SoftDecoder> openApvDecoder(std::string* error) {
    if (error) *error = "built without an APV decoder";
    return nullptr;
}

}
#endif
