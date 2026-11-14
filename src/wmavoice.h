#pragma once
#include <cstdint>

namespace absent::voice {

extern const uint8_t kLsp10i[0xf00];
extern const uint8_t kLsp16i1[0x640];
extern const uint8_t kLsp16i2[0x3c0];
extern const uint8_t kLsp16i3[0x300];
extern const uint8_t kLsp10r[0x1400];
extern const uint8_t kLsp16r1[0x500];
extern const uint8_t kLsp16r2[0x500];
extern const uint8_t kLsp16r3[0x600];
extern const float kLsp10InterA[32][2][10];
extern const float kLsp10InterB[32][2][10];
extern const float kLsp16InterA[32][2][16];
extern const float kLsp16InterB[32][2][16];
extern const double kMeanLsf10[2][10];
extern const double kMeanLsf16[2][16];
extern const float kStdCodebook[1000];
extern const float kGainSilence[256];
extern const float kGainUniversal[64];
extern const float kGainAcb[128];
extern const float kGainFcb[128];
extern const float kIpol1[17 * 9];
extern const float kIpol2[32];
extern const float kEnergy[128];
extern const float kDenoisePower[12][64];

struct Fixed {
    int n = 0;
    int x[10] = {};
    float y[10] = {};
    int noRepeat = 0;
    int pitchLag = 0;
    float pitchFactor = 0;
};

float dot(const float* a, const float* b, int n);
void synthesis(float* out, const float* coefs, const float* in, int length, int order);
void zeroSynthesis(float* out, const float* coefs, const float* in, int length, int order);
void interpolate(float* out, const float* in, const float* coefs, int precision, int frac, int taps, int length);
void tilt(float* mem, float factor, float* samples, int size);
void secondOrder(float* out, const float* in, const float zeros[2], const float poles[2], float gain, float mem[2], int n);
void setFixed(float* out, const Fixed& in, float scale, int size);
void weightedSum(float* out, const float* a, const float* b, float wa, float wb, int length);
void lspToLpc(const double* lsp, float* lpc, int halfOrder);

class Transforms {
public:
    Transforms();
    void rdft(float* out, const float* in);
    void irdft(float* out, float* in);
    void dct(float* out, const float* in);
    void dst(float* out, const float* in);

private:
    struct Complex {
        float re, im;
    };
    void fft64(Complex* out, const Complex* in, const int* map);
    void pfa(Complex* out, const Complex* in, int n, int m, const int* inMap, const int* outMap, bool naive);
    void half(float* out, Complex* data, int len, const float* fact, const float* tcos, const float* tsin, bool real);

    float tab8[3], tab16[5], tab32[9], tab64[17];
    float tab53[12], tab7[6], tab9[8];
    int forward[64], inverse[64];
    float rdftFact[2][8], rdftCos[2][32], rdftSin[2][32];
    int map63[126], map65[130];
    Complex naive13[169];
    float dctFact[8], dctCos[32], dctSin[32];
    float dstFact[8], dstCos[33], dstSin[33];
    Complex work[130], scratch[130];
    float buffer[132];
};

}
