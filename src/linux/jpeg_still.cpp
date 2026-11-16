#include "../soft_video.h"
#ifdef ABSENT_JPEG
#include <csetjmp>
#include <cstdio>
#include <jpeglib.h>

namespace absent {

namespace {

struct Failure : jpeg_error_mgr {
    jmp_buf jump;
};

void giveUp(j_common_ptr info) { longjmp(static_cast<Failure*>(info->err)->jump, 1); }
void hush(j_common_ptr) {}

bool rawLayout(const jpeg_decompress_struct& info) {
    if (info.num_components == 1) return info.jpeg_color_space == JCS_GRAYSCALE;
    const jpeg_component_info* c = info.comp_info;
    if (info.num_components != 3 || info.jpeg_color_space != JCS_YCbCr || c[1].h_samp_factor != c[2].h_samp_factor || c[1].v_samp_factor != c[2].v_samp_factor) return false;
    if (c[0].h_samp_factor != info.max_h_samp_factor || c[0].v_samp_factor != info.max_v_samp_factor) return false;
    if (info.max_h_samp_factor % c[1].h_samp_factor || info.max_v_samp_factor % c[1].v_samp_factor) return false;
    const int x = info.max_h_samp_factor / c[1].h_samp_factor, y = info.max_v_samp_factor / c[1].v_samp_factor;
    return (y == 1 && (x == 1 || x == 2 || x == 4)) || (y == 2 && x == 2);
}

struct JpegStill : SoftDecoder {
    jpeg_decompress_struct info{};
    Failure failure;
    std::vector<uint8_t> planes;
    std::vector<uint8_t> neutral;
    std::vector<uint8_t> scan;
    std::vector<JSAMPROW> rows;

    JpegStill() {
        info.err = jpeg_std_error(&failure);
        failure.error_exit = giveUp;
        failure.output_message = hush;
        jpeg_create_decompress(&info);
    }
    ~JpegStill() override { jpeg_destroy_decompress(&info); }
    const char* name() const override { return "libjpeg-turbo"; }
    void reset() override {}

    void raw(Picture& p) {
        const int count = info.num_components;
        const int lines = info.max_v_samp_factor * DCTSIZE;
        const size_t bands = (info.output_height + size_t(lines) - 1) / size_t(lines);
        size_t at[3] = {}, total = 0, first[3] = {}, pointers = 0;
        for (int k = 0; k < count; ++k) {
            const jpeg_component_info& c = info.comp_info[k];
            at[k] = total;
            first[k] = pointers;
            const size_t height = bands * size_t(c.v_samp_factor) * DCTSIZE;
            p.stride[k] = size_t(c.width_in_blocks) * DCTSIZE;
            total += p.stride[k] * height;
            pointers += height;
        }
        planes.resize(total);
        rows.resize(pointers);
        for (int k = 0; k < count; ++k) {
            const size_t height = bands * size_t(info.comp_info[k].v_samp_factor) * DCTSIZE;
            for (size_t y = 0; y < height; ++y) rows[first[k] + y] = planes.data() + at[k] + y * p.stride[k];
        }
        for (size_t band = 0; band < bands; ++band) {
            JSAMPARRAY sets[3] = {};
            for (int k = 0; k < count; ++k) sets[k] = rows.data() + first[k] + band * size_t(info.comp_info[k].v_samp_factor) * DCTSIZE;
            jpeg_read_raw_data(&info, sets, JDIMENSION(lines));
        }
        for (int k = 0; k < count; ++k) p.plane[k] = planes.data() + at[k];
        if (count == 3) {
            p.chromaWidth = int(info.comp_info[1].downsampled_width);
            p.chromaHeight = int(info.comp_info[1].downsampled_height);
            return;
        }
        p.chromaWidth = (p.width + 1) / 2;
        p.chromaHeight = (p.height + 1) / 2;
        neutral.assign(size_t(p.chromaWidth) * size_t(p.chromaHeight), 128);
        p.plane[1] = p.plane[2] = neutral.data();
        p.stride[1] = p.stride[2] = size_t(p.chromaWidth);
    }

    void converted(Picture& p) {
        const size_t width = info.output_width, count = width * info.output_height;
        scan.resize(width * 4);
        planes.resize(count * 3);
        uint8_t* luma = planes.data();
        uint8_t* blue = luma + count;
        uint8_t* red = blue + count;
        for (size_t y = 0; info.output_scanline < info.output_height; ++y) {
            JSAMPROW line = scan.data();
            jpeg_read_scanlines(&info, &line, 1);
            const uint8_t* from = scan.data();
            for (size_t i = y * width; i < (y + 1) * width; ++i, from += 4) {
                const int b = from[0], g = from[1], r = from[2];
                luma[i] = uint8_t((11966 * r + 40254 * g + 4064 * b + (16 << 16) + 32768) >> 16);
                blue[i] = uint8_t((-6596 * r - 22188 * g + 28784 * b + (128 << 16) + 32768) >> 16);
                red[i] = uint8_t((28784 * r - 26145 * g - 2639 * b + (128 << 16) + 32768) >> 16);
            }
        }
        p.chromaWidth = p.width;
        p.chromaHeight = p.height;
        p.plane[0] = luma;
        p.plane[1] = blue;
        p.plane[2] = red;
        p.stride[0] = p.stride[1] = p.stride[2] = width;
        p.color.matrix = 1;
        p.color.full = false;
    }

    bool decode(const uint8_t* data, size_t size, int64_t stamp, const PictureSink& sink) override {
        if (setjmp(failure.jump)) {
            jpeg_abort_decompress(&info);
            return false;
        }
        jpeg_mem_src(&info, data, static_cast<unsigned long>(size));
        if (jpeg_read_header(&info, TRUE) != JPEG_HEADER_OK || info.image_width > 32768 || info.image_height > 32768) {
            jpeg_abort_decompress(&info);
            return false;
        }
        const bool planar = rawLayout(info);
        info.raw_data_out = planar;
        info.out_color_space = !planar ? JCS_EXT_BGRX : info.num_components == 1 ? JCS_GRAYSCALE : JCS_YCbCr;
        jpeg_start_decompress(&info);
        Picture p;
        p.stamp = stamp;
        p.width = int(info.output_width);
        p.height = int(info.output_height);
        p.color.known = true;
        p.color.matrix = 6;
        p.color.full = true;
        if (planar) raw(p);
        else converted(p);
        jpeg_abort_decompress(&info);
        sink(p);
        return true;
    }
};

}

std::unique_ptr<SoftDecoder> openStillDecoder(std::string*) { return std::make_unique<JpegStill>(); }

}
#else

namespace absent {

std::unique_ptr<SoftDecoder> openStillDecoder(std::string* error) {
    if (error) *error = "Motion JPEG is not decoded on this system yet";
    return nullptr;
}

}
#endif
