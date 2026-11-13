#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <xvid.h>

static int has(int argc, char** argv, const char* word) {
    for (int i = 6; i < argc; ++i) if (strcmp(argv[i], word) == 0) return 1;
    return 0;
}

static int number(int argc, char** argv, const char* key, int fallback) {
    size_t length = strlen(key);
    for (int i = 6; i < argc; ++i) if (strncmp(argv[i], key, length) == 0 && argv[i][length] == '=') return atoi(argv[i] + length + 1);
    return fallback;
}

static void put(FILE* out, const unsigned char* data, int size, int key) {
    unsigned char head[5] = {(unsigned char)size, (unsigned char)(size >> 8), (unsigned char)(size >> 16), (unsigned char)(size >> 24), (unsigned char)key};
    fwrite(head, 1, 5, out);
    fwrite(data, 1, (size_t)size, out);
}

int main(int argc, char** argv) {
    if (argc < 6) {
        fprintf(stderr, "usage: xvid_encode width height rate frames out [bf=N] [q=N] [key=N] [qpel] [gmc] [interlaced] [matrix] [packed]\n");
        return 2;
    }
    int width = atoi(argv[1]), height = atoi(argv[2]), rate = atoi(argv[3]), count = atoi(argv[4]);
    FILE* out = fopen(argv[5], "wb");
    if (!out || width <= 0 || height <= 0 || rate <= 0) return 2;
    xvid_gbl_init_t init;
    memset(&init, 0, sizeof init);
    init.version = XVID_VERSION;
    xvid_global(NULL, XVID_GBL_INIT, &init, NULL);
    xvid_enc_create_t create;
    memset(&create, 0, sizeof create);
    create.version = XVID_VERSION;
    create.profile = XVID_PROFILE_AS_L4;
    create.width = width;
    create.height = height;
    create.fincr = 1;
    create.fbase = rate;
    create.max_key_interval = number(argc, argv, "key", 12);
    create.max_bframes = number(argc, argv, "bf", 2);
    create.bquant_ratio = 150;
    create.bquant_offset = 100;
    if (has(argc, argv, "packed")) create.global |= XVID_GLOBAL_PACKED | XVID_GLOBAL_DIVX5_USERDATA;
    if (xvid_encore(NULL, XVID_ENC_CREATE, &create, NULL) < 0) return 1;
    unsigned char intra[64], inter[64];
    for (int i = 0; i < 64; ++i) {
        intra[i] = (unsigned char)(i ? 9 + i / 2 + (i % 7) : 8);
        inter[i] = (unsigned char)(14 + i / 3 + (i % 5));
    }
    size_t picture = (size_t)width * height + 2 * (size_t)((width + 1) / 2) * ((height + 1) / 2);
    unsigned char* raw = malloc(picture);
    unsigned char* stream = malloc(picture * 4 + 1024);
    for (int n = 0; n < count + 64; ++n) {
        int more = n < count && fread(raw, 1, picture, stdin) == picture;
        xvid_enc_frame_t frame;
        memset(&frame, 0, sizeof frame);
        frame.version = XVID_VERSION;
        frame.bitstream = stream;
        frame.length = -1;
        frame.quant = number(argc, argv, "q", 4);
        frame.type = n == count - 1 ? XVID_TYPE_PVOP : XVID_TYPE_AUTO;
        frame.vop_flags = XVID_VOP_HALFPEL | XVID_VOP_INTER4V | XVID_VOP_HQACPRED | XVID_VOP_TRELLISQUANT | XVID_VOP_MODEDECISION_RD;
        frame.motion = XVID_ME_ADVANCEDDIAMOND16 | XVID_ME_HALFPELREFINE16 | XVID_ME_EXTSEARCH16 | XVID_ME_ADVANCEDDIAMOND8 | XVID_ME_HALFPELREFINE8 | XVID_ME_CHROMA_PVOP | XVID_ME_CHROMA_BVOP;
        if (has(argc, argv, "qpel")) {
            frame.vol_flags |= XVID_VOL_QUARTERPEL;
            frame.motion |= XVID_ME_QUARTERPELREFINE16 | XVID_ME_QUARTERPELREFINE8;
        }
        if (has(argc, argv, "gmc")) {
            frame.vol_flags |= XVID_VOL_GMC;
            frame.motion |= XVID_ME_GME_REFINE;
        }
        if (has(argc, argv, "interlaced")) {
            frame.vol_flags |= XVID_VOL_INTERLACING;
            frame.vop_flags |= XVID_VOP_TOPFIELDFIRST;
        }
        if (has(argc, argv, "matrix")) {
            frame.vol_flags |= XVID_VOL_MPEGQUANT;
            frame.quant_intra_matrix = intra;
            frame.quant_inter_matrix = inter;
        }
        if (more) {
            frame.input.csp = XVID_CSP_I420;
            frame.input.plane[0] = raw;
            frame.input.stride[0] = width;
        } else {
            frame.input.csp = XVID_CSP_NULL;
        }
        xvid_enc_stats_t stats;
        memset(&stats, 0, sizeof stats);
        stats.version = XVID_VERSION;
        int size = xvid_encore(create.handle, XVID_ENC_ENCODE, &frame, &stats);
        if (size == XVID_ERR_END) break;
        if (size < 0) return 1;
        if (size > 0) put(out, stream, size, (frame.out_flags & XVID_KEYFRAME) != 0);
    }
    xvid_encore(create.handle, XVID_ENC_DESTROY, NULL, NULL);
    free(raw);
    free(stream);
    return fclose(out) == 0 ? 0 : 1;
}
