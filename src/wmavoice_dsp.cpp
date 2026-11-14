#include "wmavoice.h"
#include <cmath>

namespace absent::voice {

float dot(const float* a, const float* b, int n) {
    float p = 0.0;
    for (int i = 0; i < n; i++) p += a[i] * b[i];
    return p;
}

void synthesis(float* out, const float* coefs, const float* in, int length, int order) {
    float a = coefs[0], b = coefs[1], c = coefs[2];
    b -= coefs[0] * coefs[0];
    c -= coefs[1] * coefs[0];
    c -= coefs[0] * b;
    float old0 = out[-4], old1 = out[-3], old2 = out[-2], old3 = out[-1];
    int n;
    for (n = 0; n <= length - 4; n += 4) {
        float out0 = in[0], out1 = in[1], out2 = in[2], out3 = in[3];
        out0 -= coefs[2] * old1;
        out1 -= coefs[2] * old2;
        out2 -= coefs[2] * old3;
        out0 -= coefs[1] * old2;
        out1 -= coefs[1] * old3;
        out0 -= coefs[0] * old3;
        float val = coefs[3];
        out0 -= val * old0;
        out1 -= val * old1;
        out2 -= val * old2;
        out3 -= val * old3;
        for (int i = 5; i < order; i += 2) {
            old3 = out[-i];
            val = coefs[i - 1];
            out0 -= val * old3;
            out1 -= val * old0;
            out2 -= val * old1;
            out3 -= val * old2;
            old2 = out[-i - 1];
            val = coefs[i];
            out0 -= val * old2;
            out1 -= val * old3;
            out2 -= val * old0;
            out3 -= val * old1;
            const float swap = old0;
            old0 = old2;
            old2 = swap;
            old1 = old3;
        }
        const float tmp0 = out0, tmp1 = out1, tmp2 = out2;
        out3 -= a * tmp2;
        out2 -= a * tmp1;
        out1 -= a * tmp0;
        out3 -= b * tmp1;
        out2 -= b * tmp0;
        out3 -= c * tmp0;
        out[0] = out0;
        out[1] = out1;
        out[2] = out2;
        out[3] = out3;
        old0 = out0;
        old1 = out1;
        old2 = out2;
        old3 = out3;
        out += 4;
        in += 4;
    }
    out -= n;
    in -= n;
    for (; n < length; n++) {
        out[n] = in[n];
        for (int i = 1; i <= order; i++) out[n] -= coefs[i - 1] * out[n - i];
    }
}

void zeroSynthesis(float* out, const float* coefs, const float* in, int length, int order) {
    for (int n = 0; n < length; n++) {
        out[n] = in[n];
        for (int i = 1; i <= order; i++) out[n] += coefs[i - 1] * in[n - i];
    }
}

void interpolate(float* out, const float* in, const float* coefs, int precision, int frac, int taps, int length) {
    for (int n = 0; n < length; n++) {
        int idx = 0;
        float v = 0;
        for (int i = 0; i < taps;) {
            v += in[n + i] * coefs[idx + frac];
            idx += precision;
            i++;
            v += in[n - i] * coefs[idx - frac];
        }
        out[n] = v;
    }
}

void tilt(float* mem, float factor, float* samples, int size) {
    const float next = samples[size - 1];
    for (int i = size - 1; i > 0; i--) samples[i] -= factor * samples[i - 1];
    samples[0] -= factor * *mem;
    *mem = next;
}

void secondOrder(float* out, const float* in, const float zeros[2], const float poles[2], float gain, float mem[2], int n) {
    for (int i = 0; i < n; i++) {
        const float tmp = gain * in[i] - poles[0] * mem[0] - poles[1] * mem[1];
        out[i] = tmp + zeros[0] * mem[0] + zeros[1] * mem[1];
        mem[1] = mem[0];
        mem[0] = tmp;
    }
}

void setFixed(float* out, const Fixed& in, float scale, int size) {
    for (int i = 0; i < in.n; i++) {
        int x = in.x[i];
        const int repeats = !((in.noRepeat >> i) & 1);
        float y = in.y[i] * scale;
        if (in.pitchLag > 0 && x >= 0 && x < size) {
            do {
                out[x] += y;
                y *= in.pitchFactor;
                x += in.pitchLag;
            } while (x < size && repeats);
        }
    }
}

void weightedSum(float* out, const float* a, const float* b, float wa, float wb, int length) {
    for (int i = 0; i < length; i++) out[i] = wa * a[i] + wb * b[i];
}

namespace {

void lspPoly(const double* lsp, double* f, int halfOrder) {
    f[0] = 1.0;
    f[1] = -2 * lsp[0];
    lsp -= 2;
    for (int i = 2; i <= halfOrder; i++) {
        const double val = -2 * lsp[2 * i];
        f[i] = val * f[i - 1] + 2 * f[i - 2];
        for (int j = i - 1; j > 1; j--) f[j] += f[j - 1] * val + f[j - 2];
        f[1] += val;
    }
}

int radix(int i, int len, int inv) {
    len >>= 1;
    if (len <= 1) return i & 1;
    if (!(i & len)) return radix(i, len, inv) * 2;
    len >>= 1;
    return radix(i, len, inv) * 4 + 1 - 2 * (!(i & len) ^ inv);
}

int inverseMod(int n, int m) {
    n = n % m;
    for (int x = 1; x < m; x++)
        if ((n * x) % m == 1) return x;
    return 0;
}

void compound(int* map, int n, int m) {
    const int len = n * m, mInv = inverseMod(m, n), nInv = inverseMod(n, m);
    for (int j = 0; j < m; j++)
        for (int i = 0; i < n; i++) {
            map[j * n + i] = (i * m + j * n) % len;
            map[len + (i * m * mInv + j * n * nInv) % len] = i * m + j;
        }
}

void powerTable(float* tab, int len) {
    const double freq = 2 * M_PI / len;
    for (int i = 0; i < len / 4; i++) tab[i] = float(cos(i * freq));
    tab[len / 4] = 0;
}

void rdftTables(float* fact, float* tcos, float* tsin, int len, int inv, float scale, bool r2r) {
    const int len4 = (len + 3) / 4;
    const double scaleD = scale;
    const double f = 2 * M_PI / len;
    const double m = inv ? 2 * scaleD : scaleD;
    fact[0] = float((inv ? 0.5 : 1.0) * m);
    fact[1] = float(inv ? 0.5 * m : 1.0 * m);
    fact[2] = float(m);
    fact[3] = float(-m);
    fact[4] = float((0.5 - 0.0) * m);
    fact[5] = r2r ? 1 / scale : float((0.0 - 0.5) * m);
    fact[6] = float((0.5 - inv) * m);
    fact[7] = float(-(0.5 - inv) * m);
    for (int i = 0; i < len4; i++) tcos[i] = float(cos(i * f));
    for (int i = 0; i < len4; i++) tsin[i] = float(cos(((len - i * 4) / 4.0) * f) * (inv ? 1 : -1));
}

inline void bf(float& x, float& y, float a, float b) {
    x = a - b;
    y = a + b;
}

inline void cmul(float& dre, float& dim, float are, float aim, float bre, float bim) {
    dre = are * bre - aim * bim;
    dim = are * bim + aim * bre;
}

inline void smul(float& dre, float& dim, float are, float aim, float bre, float bim) {
    dre = are * bre - aim * bim;
    dim = are * bim - aim * bre;
}

struct Butterfly {
    float t1 = 0, t2 = 0, t3 = 0, t4 = 0, t5 = 0, t6 = 0, r0 = 0, i0 = 0, r1 = 0, i1 = 0;

    template <typename C>
    void butterflies(C& a0, C& a1, C& a2, C& a3) {
        r0 = a0.re;
        i0 = a0.im;
        r1 = a1.re;
        i1 = a1.im;
        bf(t3, t5, t5, t1);
        bf(a2.re, a0.re, r0, t5);
        bf(a3.im, a1.im, i1, t3);
        bf(t4, t6, t2, t6);
        bf(a3.re, a1.re, r1, t4);
        bf(a2.im, a0.im, i0, t6);
    }

    template <typename C>
    void transform(C& a0, C& a1, C& a2, C& a3, float wre, float wim) {
        cmul(t1, t2, a2.re, a2.im, wre, -wim);
        cmul(t5, t6, a3.re, a3.im, wre, wim);
        butterflies(a0, a1, a2, a3);
    }
};

template <typename C>
void fft4(C* dst, const C* src) {
    float t1, t2, t3, t4, t5, t6, t7, t8;
    bf(t3, t1, src[0].re, src[1].re);
    bf(t8, t6, src[3].re, src[2].re);
    bf(dst[2].re, dst[0].re, t1, t6);
    bf(t4, t2, src[0].im, src[1].im);
    bf(t7, t5, src[2].im, src[3].im);
    bf(dst[3].im, dst[1].im, t4, t8);
    bf(dst[3].re, dst[1].re, t3, t7);
    bf(dst[2].im, dst[0].im, t2, t5);
}

template <typename C>
void fft8(C* dst, const C* src, const float* tab8) {
    Butterfly b;
    const float c = tab8[1];
    fft4(dst, src);
    bf(b.t1, dst[5].re, src[4].re, -src[5].re);
    bf(b.t2, dst[5].im, src[4].im, -src[5].im);
    bf(b.t5, dst[7].re, src[6].re, -src[7].re);
    bf(b.t6, dst[7].im, src[6].im, -src[7].im);
    b.butterflies(dst[0], dst[2], dst[4], dst[6]);
    b.transform(dst[1], dst[3], dst[5], dst[7], c, c);
}

template <typename C>
void fft16(C* dst, const C* src, const float* tab8, const float* tab16) {
    Butterfly b;
    const float c1 = tab16[1], c2 = tab16[2], c3 = tab16[3];
    fft8(dst, src, tab8);
    fft4(dst + 8, src + 8);
    fft4(dst + 12, src + 12);
    b.t1 = dst[8].re;
    b.t2 = dst[8].im;
    b.t5 = dst[12].re;
    b.t6 = dst[12].im;
    b.butterflies(dst[0], dst[4], dst[8], dst[12]);
    b.transform(dst[2], dst[6], dst[10], dst[14], c2, c2);
    b.transform(dst[1], dst[5], dst[9], dst[13], c1, c3);
    b.transform(dst[3], dst[7], dst[11], dst[15], c3, c1);
}

template <typename C>
void combine(C* z, const float* cos, int len) {
    Butterfly b;
    const int o1 = 2 * len, o2 = 4 * len, o3 = 6 * len;
    const float* wim = cos + o1 - 7;
    for (int i = 0; i < len; i += 4) {
        b.transform(z[0], z[o1 + 0], z[o2 + 0], z[o3 + 0], cos[0], wim[7]);
        b.transform(z[2], z[o1 + 2], z[o2 + 2], z[o3 + 2], cos[2], wim[5]);
        b.transform(z[4], z[o1 + 4], z[o2 + 4], z[o3 + 4], cos[4], wim[3]);
        b.transform(z[6], z[o1 + 6], z[o2 + 6], z[o3 + 6], cos[6], wim[1]);
        b.transform(z[1], z[o1 + 1], z[o2 + 1], z[o3 + 1], cos[1], wim[6]);
        b.transform(z[3], z[o1 + 3], z[o2 + 3], z[o3 + 3], cos[3], wim[4]);
        b.transform(z[5], z[o1 + 5], z[o2 + 5], z[o3 + 5], cos[5], wim[2]);
        b.transform(z[7], z[o1 + 7], z[o2 + 7], z[o3 + 7], cos[7], wim[0]);
        z += 8;
        cos += 8;
        wim -= 8;
    }
}

template <typename C>
void fft5(C* out, const C* in, const float* tab, int stride) {
    C dc, z0[4], t[6];
    dc = in[0];
    bf(t[1].im, t[0].re, in[1].re, in[4].re);
    bf(t[1].re, t[0].im, in[1].im, in[4].im);
    bf(t[3].im, t[2].re, in[2].re, in[3].re);
    bf(t[3].re, t[2].im, in[2].im, in[3].im);
    out[0].re = dc.re + t[0].re + t[2].re;
    out[0].im = dc.im + t[0].im + t[2].im;
    smul(t[4].re, t[0].re, tab[0], tab[2], t[2].re, t[0].re);
    smul(t[4].im, t[0].im, tab[0], tab[2], t[2].im, t[0].im);
    cmul(t[5].re, t[1].re, tab[4], tab[6], t[3].re, t[1].re);
    cmul(t[5].im, t[1].im, tab[4], tab[6], t[3].im, t[1].im);
    bf(z0[0].re, z0[3].re, t[0].re, t[1].re);
    bf(z0[0].im, z0[3].im, t[0].im, t[1].im);
    bf(z0[2].re, z0[1].re, t[4].re, t[5].re);
    bf(z0[2].im, z0[1].im, t[4].im, t[5].im);
    out[1 * stride].re = dc.re + z0[3].re;
    out[1 * stride].im = dc.im + z0[0].im;
    out[2 * stride].re = dc.re + z0[2].re;
    out[2 * stride].im = dc.im + z0[1].im;
    out[3 * stride].re = dc.re + z0[1].re;
    out[3 * stride].im = dc.im + z0[2].im;
    out[4 * stride].re = dc.re + z0[0].re;
    out[4 * stride].im = dc.im + z0[3].im;
}

template <typename C>
void fft7(C* out, const C* in, const C* tab, int stride) {
    C dc, t[6], z[3];
    dc = in[0];
    bf(t[1].re, t[0].re, in[1].re, in[6].re);
    bf(t[1].im, t[0].im, in[1].im, in[6].im);
    bf(t[3].re, t[2].re, in[2].re, in[5].re);
    bf(t[3].im, t[2].im, in[2].im, in[5].im);
    bf(t[5].re, t[4].re, in[3].re, in[4].re);
    bf(t[5].im, t[4].im, in[3].im, in[4].im);
    out[0].re = dc.re + t[0].re + t[2].re + t[4].re;
    out[0].im = dc.im + t[0].im + t[2].im + t[4].im;
    z[0].re = tab[0].re * t[0].re - tab[2].re * t[4].re - tab[1].re * t[2].re;
    z[1].re = tab[0].re * t[4].re - tab[1].re * t[0].re - tab[2].re * t[2].re;
    z[2].re = tab[0].re * t[2].re - tab[2].re * t[0].re - tab[1].re * t[4].re;
    z[0].im = tab[0].re * t[0].im - tab[1].re * t[2].im - tab[2].re * t[4].im;
    z[1].im = tab[0].re * t[4].im - tab[1].re * t[0].im - tab[2].re * t[2].im;
    z[2].im = tab[0].re * t[2].im - tab[2].re * t[0].im - tab[1].re * t[4].im;
    t[0].re = tab[2].im * t[1].im + tab[1].im * t[5].im - tab[0].im * t[3].im;
    t[2].re = tab[0].im * t[5].im + tab[2].im * t[3].im - tab[1].im * t[1].im;
    t[4].re = tab[2].im * t[5].im + tab[1].im * t[3].im + tab[0].im * t[1].im;
    t[0].im = tab[0].im * t[1].re + tab[1].im * t[3].re + tab[2].im * t[5].re;
    t[2].im = tab[2].im * t[3].re + tab[0].im * t[5].re - tab[1].im * t[1].re;
    t[4].im = tab[2].im * t[1].re + tab[1].im * t[5].re - tab[0].im * t[3].re;
    bf(t[1].re, z[0].re, z[0].re, t[4].re);
    bf(t[3].re, z[1].re, z[1].re, t[2].re);
    bf(t[5].re, z[2].re, z[2].re, t[0].re);
    bf(t[1].im, z[0].im, z[0].im, t[0].im);
    bf(t[3].im, z[1].im, z[1].im, t[2].im);
    bf(t[5].im, z[2].im, z[2].im, t[4].im);
    out[1 * stride].re = dc.re + z[0].re;
    out[1 * stride].im = dc.im + t[1].im;
    out[2 * stride].re = dc.re + t[3].re;
    out[2 * stride].im = dc.im + z[1].im;
    out[3 * stride].re = dc.re + z[2].re;
    out[3 * stride].im = dc.im + t[5].im;
    out[4 * stride].re = dc.re + t[5].re;
    out[4 * stride].im = dc.im + z[2].im;
    out[5 * stride].re = dc.re + z[1].re;
    out[5 * stride].im = dc.im + t[3].im;
    out[6 * stride].re = dc.re + t[1].re;
    out[6 * stride].im = dc.im + z[0].im;
}

template <typename C>
void fft9(C* out, const C* in, const C* tab, int stride) {
    C dc, t[16], w[4], x[5], y[5], z[2];
    dc = in[0];
    bf(t[1].re, t[0].re, in[1].re, in[8].re);
    bf(t[1].im, t[0].im, in[1].im, in[8].im);
    bf(t[3].re, t[2].re, in[2].re, in[7].re);
    bf(t[3].im, t[2].im, in[2].im, in[7].im);
    bf(t[5].re, t[4].re, in[3].re, in[6].re);
    bf(t[5].im, t[4].im, in[3].im, in[6].im);
    bf(t[7].re, t[6].re, in[4].re, in[5].re);
    bf(t[7].im, t[6].im, in[4].im, in[5].im);
    w[0].re = t[0].re - t[6].re;
    w[0].im = t[0].im - t[6].im;
    w[1].re = t[2].re - t[6].re;
    w[1].im = t[2].im - t[6].im;
    w[2].re = t[1].re - t[7].re;
    w[2].im = t[1].im - t[7].im;
    w[3].re = t[3].re + t[7].re;
    w[3].im = t[3].im + t[7].im;
    z[0].re = dc.re + t[4].re;
    z[0].im = dc.im + t[4].im;
    z[1].re = t[0].re + t[2].re + t[6].re;
    z[1].im = t[0].im + t[2].im + t[6].im;
    out[0].re = z[0].re + z[1].re;
    out[0].im = z[0].im + z[1].im;
    y[3].re = tab[0].im * (t[1].re - t[3].re + t[7].re);
    y[3].im = tab[0].im * (t[1].im - t[3].im + t[7].im);
    x[3].re = z[0].re + tab[0].re * z[1].re;
    x[3].im = z[0].im + tab[0].re * z[1].im;
    z[0].re = dc.re + tab[0].re * t[4].re;
    z[0].im = dc.im + tab[0].re * t[4].im;
    x[1].re = tab[1].re * w[0].re + tab[2].im * w[1].re;
    x[1].im = tab[1].re * w[0].im + tab[2].im * w[1].im;
    x[2].re = tab[2].im * w[0].re - tab[3].re * w[1].re;
    x[2].im = tab[2].im * w[0].im - tab[3].re * w[1].im;
    y[1].re = tab[1].im * w[2].re + tab[2].re * w[3].re;
    y[1].im = tab[1].im * w[2].im + tab[2].re * w[3].im;
    y[2].re = tab[2].re * w[2].re - tab[3].im * w[3].re;
    y[2].im = tab[2].re * w[2].im - tab[3].im * w[3].im;
    y[0].re = tab[0].im * t[5].re;
    y[0].im = tab[0].im * t[5].im;
    x[4].re = x[1].re + x[2].re;
    x[4].im = x[1].im + x[2].im;
    y[4].re = y[1].re - y[2].re;
    y[4].im = y[1].im - y[2].im;
    x[1].re = z[0].re + x[1].re;
    x[1].im = z[0].im + x[1].im;
    y[1].re = y[0].re + y[1].re;
    y[1].im = y[0].im + y[1].im;
    x[2].re = z[0].re + x[2].re;
    x[2].im = z[0].im + x[2].im;
    y[2].re = y[2].re - y[0].re;
    y[2].im = y[2].im - y[0].im;
    x[4].re = z[0].re - x[4].re;
    x[4].im = z[0].im - x[4].im;
    y[4].re = y[0].re - y[4].re;
    y[4].im = y[0].im - y[4].im;
    out[1 * stride] = C{x[1].re + y[1].im, x[1].im - y[1].re};
    out[2 * stride] = C{x[2].re + y[2].im, x[2].im - y[2].re};
    out[3 * stride] = C{x[3].re + y[3].im, x[3].im - y[3].re};
    out[4 * stride] = C{x[4].re + y[4].im, x[4].im - y[4].re};
    out[5 * stride] = C{x[4].re - y[4].im, x[4].im + y[4].re};
    out[6 * stride] = C{x[3].re - y[3].im, x[3].im + y[3].re};
    out[7 * stride] = C{x[2].re - y[2].im, x[2].im + y[2].re};
    out[8 * stride] = C{x[1].re - y[1].im, x[1].im + y[1].re};
}

}

void lspToLpc(const double* lsp, float* lpc, int halfOrder) {
    double pa[9], qa[9];
    float* lpc2 = lpc + (halfOrder << 1) - 1;
    lspPoly(lsp, pa, halfOrder);
    lspPoly(lsp + 1, qa, halfOrder);
    while (halfOrder--) {
        const double paf = pa[halfOrder + 1] + pa[halfOrder];
        const double qaf = qa[halfOrder + 1] - qa[halfOrder];
        lpc[halfOrder] = float(0.5 * (paf + qaf));
        lpc2[-halfOrder] = float(0.5 * (paf - qaf));
    }
}

Transforms::Transforms() {
    powerTable(tab8, 8);
    powerTable(tab16, 16);
    powerTable(tab32, 32);
    powerTable(tab64, 64);
    tab53[0] = tab53[1] = float(cos(2 * M_PI / 5));
    tab53[2] = tab53[3] = float(cos(2 * M_PI / 10));
    tab53[4] = tab53[5] = float(sin(2 * M_PI / 5));
    tab53[6] = tab53[7] = float(sin(2 * M_PI / 10));
    tab53[8] = tab53[9] = float(cos(2 * M_PI / 12));
    tab53[10] = float(cos(2 * M_PI / 6));
    tab53[11] = float(cos(8 * M_PI / 6));
    tab7[0] = float(cos(2 * M_PI / 7));
    tab7[1] = float(sin(2 * M_PI / 7));
    tab7[2] = float(sin(2 * M_PI / 28));
    tab7[3] = float(cos(2 * M_PI / 28));
    tab7[4] = float(cos(2 * M_PI / 14));
    tab7[5] = float(sin(2 * M_PI / 14));
    tab9[0] = float(cos(2 * M_PI / 3));
    tab9[1] = float(sin(2 * M_PI / 3));
    tab9[2] = float(cos(2 * M_PI / 9));
    tab9[3] = float(sin(2 * M_PI / 9));
    tab9[4] = float(cos(2 * M_PI / 36));
    tab9[5] = float(sin(2 * M_PI / 36));
    tab9[6] = tab9[2] + tab9[5];
    tab9[7] = tab9[3] - tab9[4];
    for (int i = 0; i < 64; i++) {
        forward[i] = -radix(i, 64, 0) & 63;
        inverse[i] = -radix(i, 64, 1) & 63;
    }
    rdftTables(rdftFact[0], rdftCos[0], rdftSin[0], 128, 0, 1.0f, false);
    rdftTables(rdftFact[1], rdftCos[1], rdftSin[1], 128, 1, 1.0f, false);
    rdftTables(dctFact, dctCos, dctSin, 126, 0, 1.0f / 64, true);
    rdftTables(dstFact, dstCos, dstSin, 130, 0, 1.0f / 64, false);
    compound(map63, 7, 9);
    compound(map65, 13, 5);
    const double phase = -2.0 * M_PI / 13;
    for (int i = 0; i < 13; i++)
        for (int j = 0; j < 13; j++) {
            const double factor = phase * i * j;
            naive13[i * j] = Complex{float(cos(factor)), float(sin(factor))};
        }
}

void Transforms::fft64(Complex* out, const Complex* in, const int* map) {
    for (int i = 0; i < 64; i++) out[i] = in[map[i]];
    fft16(out, out, tab8, tab16);
    fft8(out + 16, out + 16, tab8);
    fft8(out + 24, out + 24, tab8);
    combine(out, tab32, 4);
    fft16(out + 32, out + 32, tab8, tab16);
    fft16(out + 48, out + 48, tab8, tab16);
    combine(out, tab64, 8);
}

void Transforms::rdft(float* outF, const float* inF) {
    Complex* data = reinterpret_cast<Complex*>(outF);
    fft64(data, reinterpret_cast<const Complex*>(inF), forward);
    const float* fact = rdftFact[0];
    const float* tcos = rdftCos[0];
    const float* tsin = rdftSin[0];
    Complex t[3];
    t[0].re = data[0].re;
    data[0].re = t[0].re + data[0].im;
    data[0].im = t[0].re - data[0].im;
    data[0].re = fact[0] * data[0].re;
    data[0].im = fact[1] * data[0].im;
    data[32].re = fact[2] * data[32].re;
    data[32].im = fact[3] * data[32].im;
    for (int i = 1; i < 32; i++) {
        t[0].re = fact[4] * (data[i].re + data[64 - i].re);
        t[0].im = fact[5] * (data[i].im - data[64 - i].im);
        t[1].re = fact[6] * (data[i].im + data[64 - i].im);
        t[1].im = fact[7] * (data[i].re - data[64 - i].re);
        cmul(t[2].re, t[2].im, t[1].re, t[1].im, tcos[i], tsin[i]);
        data[i].re = t[0].re + t[2].re;
        data[i].im = t[2].im - t[0].im;
        data[64 - i].re = t[0].re - t[2].re;
        data[64 - i].im = t[2].im + t[0].im;
    }
    data[64].re = data[0].im;
    data[0].im = data[64].im = 0;
}

void Transforms::irdft(float* outF, float* inF) {
    Complex* data = reinterpret_cast<Complex*>(inF);
    const float* fact = rdftFact[1];
    const float* tcos = rdftCos[1];
    const float* tsin = rdftSin[1];
    Complex t[3];
    data[0].im = data[64].re;
    t[0].re = data[0].re;
    data[0].re = t[0].re + data[0].im;
    data[0].im = t[0].re - data[0].im;
    data[0].re = fact[0] * data[0].re;
    data[0].im = fact[1] * data[0].im;
    data[32].re = fact[2] * data[32].re;
    data[32].im = fact[3] * data[32].im;
    for (int i = 1; i < 32; i++) {
        t[0].re = fact[4] * (data[i].re + data[64 - i].re);
        t[0].im = fact[5] * (data[i].im - data[64 - i].im);
        t[1].re = fact[6] * (data[i].im + data[64 - i].im);
        t[1].im = fact[7] * (data[i].re - data[64 - i].re);
        cmul(t[2].re, t[2].im, t[1].re, t[1].im, tcos[i], tsin[i]);
        data[i].re = t[0].re + t[2].re;
        data[i].im = t[2].im - t[0].im;
        data[64 - i].re = t[0].re - t[2].re;
        data[64 - i].im = t[2].im + t[0].im;
    }
    fft64(reinterpret_cast<Complex*>(outF), data, inverse);
}

void Transforms::pfa(Complex* out, const Complex* in, int n, int m, const int* inMap, const int* outMap, bool naive) {
    const Complex* t7 = reinterpret_cast<const Complex*>(tab7);
    const Complex* t9 = reinterpret_cast<const Complex*>(tab9);
    for (int i = 0; i < m; i++) {
        for (int j = 0; j < n; j++) work[j] = in[inMap[i * n + j]];
        Complex* dst = scratch + i;
        if (naive) {
            for (int k = 0; k < n; k++) {
                Complex tmp = {0, 0};
                for (int j = 0; j < n; j++) {
                    Complex res;
                    const Complex mult = naive13[k * j];
                    cmul(res.re, res.im, work[j].re, work[j].im, mult.re, mult.im);
                    tmp.re += res.re;
                    tmp.im += res.im;
                }
                dst[k * m] = tmp;
            }
        } else {
            fft7(dst, work, t7, m);
        }
    }
    for (int i = 0; i < n; i++) {
        if (naive) fft5(scratch + m * i, scratch + m * i, tab53, 1);
        else fft9(scratch + m * i, scratch + m * i, t9, 1);
    }
    for (int i = 0; i < n * m; i++) out[i] = scratch[outMap[i]];
}

void Transforms::half(float* out, Complex* data, int len, const float* fact, const float* tcos, const float* tsin, bool real) {
    const int len2 = len >> 1, len4 = len >> 2;
    float tmpDc = data[0].re;
    data[0].re = tmpDc + data[0].im;
    tmpDc = tmpDc - data[0].im;
    data[0].re = fact[0] * data[0].re;
    tmpDc = fact[1] * tmpDc;
    data[len4].re = fact[2] * data[len4].re;
    float tmp[4], mid;
    Complex sf = data[len4], sl = data[len4 + 1];
    if (real) tmp[0] = fact[4] * (sf.re + sl.re);
    else tmp[0] = fact[5] * (sf.im - sl.im);
    tmp[1] = fact[6] * (sf.im + sl.im);
    tmp[2] = fact[7] * (sf.re - sl.re);
    if (real) {
        tmp[3] = tmp[1] * tcos[len4] - tmp[2] * tsin[len4];
        mid = (tmp[0] - tmp[3]);
    } else {
        tmp[3] = tmp[1] * tsin[len4] + tmp[2] * tcos[len4];
        mid = (tmp[0] + tmp[3]);
    }
    for (int i = 1; i <= len4; i++) {
        float u[4];
        const Complex f = data[i];
        const Complex l = data[len2 - i];
        if (real) u[0] = fact[4] * (f.re + l.re);
        else u[0] = fact[5] * (f.im - l.im);
        u[1] = fact[6] * (f.im + l.im);
        u[2] = fact[7] * (f.re - l.re);
        if (real) {
            u[3] = u[1] * tcos[i] - u[2] * tsin[i];
            out[i] = (u[0] + u[3]);
            out[len - i] = (u[0] - u[3]);
        } else {
            u[3] = u[1] * tsin[i] + u[2] * tcos[i];
            out[i - 1] = (u[3] - u[0]);
            out[len - i - 1] = (u[0] + u[3]);
        }
    }
    for (int i = 1; i < (len4 + !real); i++) out[len2 - i] = out[len - i];
    if (real) {
        out[len2] = tmpDc;
        out[len4 + 1] = mid * fact[5];
    } else {
        out[len4] = mid;
    }
}

void Transforms::dct(float* out, const float* in) {
    for (int i = 0; i < 63; i++) buffer[i] = buffer[126 - i] = in[i];
    buffer[63] = in[63];
    Complex* data = reinterpret_cast<Complex*>(out);
    pfa(data, reinterpret_cast<const Complex*>(buffer), 7, 9, map63, map63 + 63, false);
    half(out, data, 126, dctFact, dctCos, dctSin, true);
}

void Transforms::dst(float* out, const float* in) {
    buffer[0] = 0;
    for (int i = 1; i < 65; i++) {
        const float a = in[i - 1];
        buffer[i] = -a;
        buffer[130 - i] = a;
    }
    buffer[65] = 0;
    Complex* data = reinterpret_cast<Complex*>(out);
    pfa(data, reinterpret_cast<const Complex*>(buffer), 13, 5, map65, map65 + 65, true);
    half(out, data, 130, dstFact, dstCos, dstSin, false);
}

}
