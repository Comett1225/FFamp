#include <opencore-amrnb/interf_enc.h>
#include <vo-amrwbenc/enc_if.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

int main(int argc, char** argv) {
    if (argc != 4) return 1;
    int wide = strcmp(argv[1], "wb") == 0;
    FILE* in = fopen(argv[2], "rb");
    FILE* out = fopen(argv[3], "wb");
    if (!in || !out) return 1;
    fseek(in, 44, SEEK_SET);
    fputs(wide ? "#!AMR-WB\n" : "#!AMR\n", out);
    void* state = wide ? E_IF_init() : Encoder_Interface_init(0);
    int step = wide ? 320 : 160;
    short pcm[320];
    unsigned char packet[128];
    int frame = 0;
    while (fread(pcm, 2, step, in) == (size_t)step) {
        int size = wide ? E_IF_encode(state, 8 - frame / 20 % 4 * 2, pcm, packet, 0) : Encoder_Interface_Encode(state, frame / 20 % 2 ? MR122 : MR795, pcm, packet, 0);
        fwrite(packet, 1, size, out);
        ++frame;
    }
    fclose(out);
    return 0;
}
