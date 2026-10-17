// Software Voodoo rasterizer: draws into the emulated frame buffer RAM exactly as the hardware would.
// It is a port of the pixel pipeline of MAME's Voodoo emulation (src/devices/video/voodoo_render.cpp,
// BSD-3-Clause, copyright Aaron Giles), reduced to one TMU and the Voodoo 1 feature set.
//
// It serves Glide's start-up probes (which draw and read the pixels back), LFB reads, frame dumps, and
// is the reference the GPU renderer is checked against.
#include "swrender.h"
#include "platform.h"

#include <algorithm>
#include <bit>
#include <cmath>
#include <cstring>

namespace rt {
namespace {

inline uint32_t bits(uint32_t v, int pos, int n) { return (v >> pos) & ((1u << n) - 1); }

// ---- small RGBA helper (signed channels, like MAME's rgbaint_t) ----

struct C4 {
    int32_t a, r, g, b;
    static C4 from_argb(uint32_t v) { return {(int32_t)(v >> 24), (int32_t)((v >> 16) & 0xFF), (int32_t)((v >> 8) & 0xFF), (int32_t)(v & 0xFF)}; }
    uint32_t to_argb() const {
        auto c = [](int32_t x) { return (uint32_t)std::clamp(x, 0, 255); };
        return (c(a) << 24) | (c(r) << 16) | (c(g) << 8) | c(b);
    }
    C4 alpha_all() const { return {a, a, a, a}; }
};
inline int32_t clamp8(int32_t v) { return std::clamp(v, 0, 255); }
// this = clamp(((this * scale) >> 8) + add)
inline C4 scale_add_clamp(C4 c, C4 scale, C4 add) {
    return {clamp8(((c.a * scale.a) >> 8) + add.a), clamp8(((c.r * scale.r) >> 8) + add.r),
            clamp8(((c.g * scale.g) >> 8) + add.g), clamp8(((c.b * scale.b) >> 8) + add.b)};
}

// ---- static tables ----

const uint8_t DITHER4[16] = {0, 8, 2, 10, 12, 4, 14, 6, 3, 11, 1, 9, 15, 7, 13, 5};
const uint8_t DITHER2[16] = {8, 10, 8, 10, 11, 9, 11, 9, 8, 10, 8, 10, 11, 9, 11, 9};

struct Tables {
    uint8_t dither4[4][4][2][256], dither2[4][4][2][256], nodither[2][256];
    uint32_t rgb332[256], alpha8[256], int8[256], ai44[256];
    uint32_t rgb565[65536], argb1555[65536], argb4444[65536];
    uint32_t pen565[65536]; // frame buffer 5-6-5 -> ARGB, for blending reads
    Tables() {
        for (int y = 0; y < 4; y++)
            for (int x = 0; x < 4; x++)
                for (int v = 0; v < 256; v++) {
                    auto rb = [&](uint8_t d) { return (uint8_t)(((v << 1) - (v >> 4) + (v >> 7) + d) >> 4); };
                    auto g = [&](uint8_t d) { return (uint8_t)(((v << 2) - (v >> 4) + (v >> 6) + d) >> 4); };
                    dither4[y][x][0][v] = rb(DITHER4[y * 4 + x]);
                    dither4[y][x][1][v] = g(DITHER4[y * 4 + x]);
                    dither2[y][x][0][v] = rb(DITHER2[y * 4 + x]);
                    dither2[y][x][1][v] = g(DITHER2[y * 4 + x]);
                }
        for (int v = 0; v < 256; v++) {
            nodither[0][v] = (uint8_t)(v >> 3);
            nodither[1][v] = (uint8_t)(v >> 2);
        }
        auto ex = [](uint32_t v, int n) { // replicate n bits up to 8
            uint32_t r = v << (8 - n);
            for (int s = n; s < 8; s += n) r |= r >> s;
            return r & 0xFF;
        };
        for (uint32_t v = 0; v < 256; v++) {
            rgb332[v] = 0xFF000000u | (ex(v >> 5, 3) << 16) | (ex((v >> 2) & 7, 3) << 8) | ex(v & 3, 2);
            alpha8[v] = (v << 24) | (v << 16) | (v << 8) | v;
            int8[v] = 0xFF000000u | (v << 16) | (v << 8) | v;
            uint32_t a = ex(v >> 4, 4), i = ex(v & 15, 4);
            ai44[v] = (a << 24) | (i << 16) | (i << 8) | i;
        }
        for (uint32_t v = 0; v < 65536; v++) {
            rgb565[v] = 0xFF000000u | (ex(v >> 11, 5) << 16) | (ex((v >> 5) & 63, 6) << 8) | ex(v & 31, 5);
            pen565[v] = rgb565[v];
            argb1555[v] = ((v & 0x8000) ? 0xFF000000u : 0) | (ex((v >> 10) & 31, 5) << 16) | (ex((v >> 5) & 31, 5) << 8) | ex(v & 31, 5);
            argb4444[v] = (ex(v >> 12, 4) << 24) | (ex((v >> 8) & 15, 4) << 16) | (ex((v >> 4) & 15, 4) << 8) | ex(v & 15, 4);
        }
    }
};
const Tables &tables() {
    static Tables *t = new Tables();
    return *t;
}

struct Dither {
    const uint8_t (*lut)[2][256]; // [x & 3]
    const uint8_t *raw4;          // 4x4 matrix row
    const uint8_t *raw;           // matrix row in use (nullptr when not dithering)
    Dither(int y, uint32_t fbzmode) {
        const Tables &t = tables();
        raw4 = &DITHER4[(y & 3) * 4];
        static uint8_t nod[4][2][256];
        static bool init;
        if (!init) {
            for (int x = 0; x < 4; x++) memcpy(nod[x], t.nodither, sizeof(t.nodither));
            init = true;
        }
        if (!(fbzmode & (1 << 8))) {
            lut = nod;
            raw = nullptr;
        } else if (!(fbzmode & (1 << 11))) {
            lut = t.dither4[y & 3];
            raw = &DITHER4[(y & 3) * 4];
        } else {
            lut = t.dither2[y & 3];
            raw = &DITHER2[(y & 3) * 4];
        }
    }
    uint16_t pixel(int x, C4 c) const {
        const uint8_t(*l)[256] = lut[x & 3];
        return (uint16_t)((l[0][clamp8(c.r)] << 11) | (l[1][clamp8(c.g)] << 5) | l[0][clamp8(c.b)]);
    }
    uint32_t subtract(int x) const { return raw ? (15 - raw[x & 3]) >> 1 : 0; }
};

int32_t fast_log2(double value, int fracbits) {
    if (value < 0) return 0;
    static const uint8_t LOG2[128] = {
        0, 2, 5, 8, 11, 14, 16, 19, 22, 25, 27, 30, 33, 35, 38, 40, 43, 46, 48, 51, 53, 56, 58, 61, 63, 65,
        68, 70, 73, 75, 77, 80, 82, 84, 87, 89, 91, 93, 96, 98, 100, 102, 104, 106, 109, 111, 113, 115, 117,
        119, 121, 123, 125, 127, 129, 132, 134, 136, 138, 140, 141, 143, 145, 147, 149, 151, 153, 155, 157,
        159, 161, 162, 164, 166, 168, 170, 172, 173, 175, 177, 179, 181, 182, 184, 186, 188, 189, 191, 193,
        194, 196, 198, 200, 201, 203, 205, 206, 208, 209, 211, 213, 214, 216, 218, 219, 221, 222, 224, 225,
        227, 229, 230, 232, 233, 235, 236, 238, 239, 241, 242, 244, 245, 247, 248, 250, 251, 253, 254};
    uint64_t i;
    memcpy(&i, &value, 8);
    uint32_t ival = (uint32_t)(i >> 45);
    int32_t exp = (int32_t)(ival >> 7) - 1023 - fracbits;
    return (exp << 8) | LOG2[ival & 127];
}

int32_t compute_wfloat(int64_t iterw) {
    int exp = std::countl_zero((uint64_t)iterw) - 16;
    if (exp < 0) return 0x0000;
    if (exp >= 16) return 0xFFFF;
    return (int32_t)(((exp << 12) | ((iterw >> (35 - exp)) ^ 0x1FFF)) + 1);
}

int32_t clamped_z(int32_t iterz, uint32_t fbzcp) {
    if (fbzcp & (1u << 28)) return std::clamp(iterz >> 12, 0, 0xFFFF);
    uint32_t r = (uint32_t)iterz >> 12;
    if (r == 0xFFFFF) return 0;
    if (r == 0x10000) return 0xFFFF;
    return r & 0xFFFF;
}

int32_t clamped_w(int64_t iterw, uint32_t fbzcp) {
    if (fbzcp & (1u << 28)) return std::clamp((int32_t)(iterw >> 48), 0, 0xFF);
    uint32_t r = (uint32_t)((uint64_t)iterw >> 48);
    if (r == 0xFFFF) return 0;
    if (r == 0x100) return 0xFF;
    return r & 0xFF;
}

C4 clamped_argb(int32_t a, int32_t r, int32_t g, int32_t b, uint32_t fbzcp) {
    int32_t v[4] = {a >> 20, r >> 20, g >> 20, b >> 20};
    for (int32_t &x : v) {
        if (fbzcp & (1u << 28)) x = clamp8(x);
        else {
            int32_t t = x & 0xFFF;
            if (t == 0xFFF) t = 0;
            else if (t == 0x100) t = 0xFF;
            x = t & 0xFF;
        }
    }
    return {v[0], v[1], v[2], v[3]};
}

// ---- texture unit ----

struct Texture {
    const uint8_t *ram;
    const uint32_t *lookup;
    uint32_t ncc[256];
    uint32_t palette_alpha[256];
    uint32_t wmask, hmask;
    int32_t lodmin, lodmax, lodbias;
    uint32_t lodmask;
    uint32_t lodoffset[9];
    int32_t detailmax, detailbias, detailscale;
    uint32_t format;

    void setup(const VoodooState &st) {
        const uint32_t *tmu = st.tmu;
        const Tables &t = tables();
        ram = st.tmu_mem;
        uint32_t texmode = tmu[vTextureMode], tlod = tmu[vTLOD];
        format = bits(texmode, 8, 4);
        lodmin = bits(tlod, 0, 6) << 6;
        lodmax = bits(tlod, 6, 6) << 6;
        lodbias = (int8_t)(bits(tlod, 12, 6) << 2) << 4;
        lodmask = 0x1FF;
        if (tlod & (1u << 19)) lodmask = (tlod & (1u << 18)) ? 0x0AA : 0x155;
        wmask = hmask = 0xFF;
        if (tlod & (1u << 20)) hmask >>= bits(tlod, 21, 2);
        else wmask >>= bits(tlod, 21, 2);
        int bppscale = format >> 3;
        uint32_t base = tmu[vTexBaseAddr] & 0x7FFFF;
        uint32_t lodstart[9], offset = 0;
        for (int lod = 0; lod <= 8; lod++) {
            lodstart[lod] = offset;
            if (lodmask & (1u << lod)) {
                uint32_t size = ((wmask >> lod) + 1) * ((hmask >> lod) + 1);
                if (lod >= 3 && size < 4) size = 4;
                offset += size << bppscale;
            }
        }
        uint32_t lodbase[9];
        for (uint32_t &b : lodbase) b = base;
        if ((tlod & (1u << 24)) && bits(tlod, 28, 4) == 0) {
            lodbase[1] = tmu[vTexBaseAddr1] & 0x7FFFF;
            lodbase[2] = tmu[vTexBaseAddr2] & 0x7FFFF;
            for (int lod = 3; lod <= 8; lod++) lodbase[lod] = tmu[vTexBaseAddr38] & 0x7FFFF;
        }
        for (int lod = 0; lod <= 8; lod++) lodoffset[lod] = ((lodbase[lod] << 3) + lodstart[lod]) & 0x3FFFFF;
        uint32_t det = tmu[vTDetail];
        detailmax = bits(det, 0, 8);
        detailbias = (int8_t)(bits(det, 8, 6) << 2) << 6;
        detailscale = bits(det, 14, 3);

        switch (format) {
        case 0: case 8: lookup = t.rgb332; break;
        case 2: lookup = t.alpha8; break;
        case 3: case 13: lookup = t.int8; break;
        case 4: lookup = t.ai44; break;
        case 10: lookup = t.rgb565; break;
        case 11: lookup = t.argb1555; break;
        case 12: lookup = t.argb4444; break;
        case 1: case 9: {
            // NCC: Y from the first four registers (bytes), I and Q as 9-bit signed triples.
            const uint32_t *regs = tmu + vNccTable0 + 12 * bits(texmode, 5, 1);
            const uint8_t *y = (const uint8_t *)regs;
            for (int index = 0; index < 256; index++) {
                int32_t yy = y[bits(index, 4, 4)];
                int32_t i = (int32_t)regs[4 + bits(index, 2, 2)], q = (int32_t)regs[8 + bits(index, 0, 2)];
                auto s9 = [](int32_t v) { return (v << 23) >> 23; };
                int32_t r = std::clamp(yy + s9(i >> 18) + s9(q >> 18), 0, 255);
                int32_t g = std::clamp(yy + s9(i >> 9) + s9(q >> 9), 0, 255);
                int32_t b = std::clamp(yy + s9(i) + s9(q), 0, 255);
                ncc[index] = 0xFF000000u | (r << 16) | (g << 8) | b;
            }
            lookup = ncc;
            break;
        }
        default: // 5, 14: palette
            lookup = st.palette;
            break;
        }
    }

    uint32_t texel(uint32_t texbase, int32_t s, int32_t t) const {
        if (format < 8) return lookup[ram[(texbase + t + s) & 0x3FFFFF]];
        uint16_t v;
        memcpy(&v, &ram[(texbase + 2 * (t + s)) & 0x3FFFFE], 2);
        if (format >= 10 && format <= 12) return lookup[v];
        return (lookup[v & 0xFF] & 0xFFFFFF) | ((uint32_t)(v & 0xFF00) << 16);
    }

    C4 fetch(uint32_t texmode, const Dither &dither, int x, double iters, double itert, double iterw, int32_t &lod) const {
        int32_t s, t;
        if (texmode & 1) {
            double recip = 256.0 / iterw;
            s = (int32_t)(iters * recip);
            t = (int32_t)(itert * recip);
            lod -= fast_log2(iterw, 32);
        } else {
            s = (int32_t)(iters * (1.0 / (1 << 24)));
            t = (int32_t)(itert * (1.0 / (1 << 24)));
        }
        if ((texmode & (1 << 3)) && iterw < 0) s = t = 0;
        lod += lodbias;
        if (texmode & (1 << 4)) lod += dither.raw4[x & 3] << 4;
        lod = std::clamp(lod, lodmin, lodmax);
        int32_t ilod = lod >> 8;
        ilod += (~lodmask >> ilod) & 1;
        uint32_t texbase = lodoffset[ilod];
        int32_t smax = wmask >> ilod, tmax = hmask >> ilod;
        bool point = (lod == lodmin && !(texmode & (1 << 2))) || (lod != lodmin && !(texmode & (1 << 1)));
        if (point) {
            s >>= ilod + 8;
            t >>= ilod + 8;
            if (texmode & (1 << 6)) s = std::clamp(s, 0, smax);
            if (texmode & (1 << 7)) t = std::clamp(t, 0, tmax);
            s &= smax;
            t &= tmax;
            return C4::from_argb(texel(texbase, s, t * (smax + 1)));
        }
        s >>= ilod;
        t >>= ilod;
        s -= 0x80;
        t -= 0x80;
        uint32_t sfrac = s & 0xF0, tfrac = t & 0xF0;
        s >>= 8;
        t >>= 8;
        int32_t s1 = s + 1, t1 = t + 1;
        if (texmode & (1 << 6)) {
            if (s < 0) s = s1 = 0;
            else if (s >= smax) s = s1 = smax;
        }
        s &= smax;
        s1 &= smax;
        if (texmode & (1 << 7)) {
            if (t < 0) t = t1 = 0;
            else if (t >= tmax) t = t1 = tmax;
        }
        t &= tmax;
        t1 &= tmax;
        t *= smax + 1;
        t1 *= smax + 1;
        uint32_t c0 = texel(texbase, s, t), c1 = texel(texbase, s1, t), c2 = texel(texbase, s, t1), c3 = texel(texbase, s1, t1);
        auto ch = [&](int sh) {
            int32_t a = (c0 >> sh) & 0xFF, b = (c1 >> sh) & 0xFF, c = (c2 >> sh) & 0xFF, d = (c3 >> sh) & 0xFF;
            int32_t top = a + (((b - a) * (int32_t)sfrac) >> 8);
            int32_t bot = c + (((d - c) * (int32_t)sfrac) >> 8);
            return top + (((bot - top) * (int32_t)tfrac) >> 8);
        };
        return {ch(24), ch(16), ch(8), ch(0)};
    }

    // Texture colour combine unit (c_other is zero with a single TMU).
    C4 combine(uint32_t texmode, C4 local, C4 other, int32_t lod) const {
        C4 blend = (texmode & (1 << 12)) ? C4{other.a, 0, 0, 0} : other;
        if (texmode & (1 << 21)) blend.a = 0;
        else blend.a = other.a;
        if (texmode & (1 << 13)) { blend.r -= local.r; blend.g -= local.g; blend.b -= local.b; }
        if (texmode & (1 << 22)) blend.a -= local.a;
        auto detail = [&]() -> int32_t {
            if (detailbias <= lod) return 0;
            int32_t v = (uint8_t)(((detailbias - lod) << detailscale) >> 8);
            return std::min(v, detailmax);
        };
        C4 f{0, 0, 0, 0};
        switch (bits(texmode, 14, 3)) {
        case 1: f.r = local.r; f.g = local.g; f.b = local.b; break;
        case 2: f.r = f.g = f.b = other.a; break;
        case 3: f.r = f.g = f.b = local.a; break;
        case 4: f.r = f.g = f.b = detail(); break;
        case 5: f.r = f.g = f.b = lod & 0xFF; break;
        }
        switch (bits(texmode, 23, 3)) {
        case 1: case 3: f.a = local.a; break;
        case 2: f.a = other.a; break;
        case 4: f.a = detail(); break;
        case 5: f.a = lod & 0xFF; break;
        }
        if (!(texmode & (1 << 17))) { f.r ^= 0xFF; f.g ^= 0xFF; f.b ^= 0xFF; }
        if (!(texmode & (1 << 26))) f.a ^= 0xFF;
        f.a++; f.r++; f.g++; f.b++;
        C4 add{0, 0, 0, 0};
        switch (bits(texmode, 18, 2)) {
        case 1: add.r = local.r; add.g = local.g; add.b = local.b; break;
        case 2: add.r = add.g = add.b = local.a; break;
        }
        if (texmode & (3u << 27)) add.a = local.a;
        C4 out = scale_add_clamp(blend, f, add);
        if (texmode & (1 << 20)) { out.r ^= 0xFF; out.g ^= 0xFF; out.b ^= 0xFF; }
        if (texmode & (1 << 29)) out.a ^= 0xFF;
        return out;
    }
};

int32_t compute_lodbase(int64_t dsdx, int64_t dsdy, int64_t dtdx, int64_t dtdy) {
    double a = (double)dsdx, b = (double)dsdy, c = (double)dtdx, d = (double)dtdy;
    double texdx = a * a + c * c, texdy = b * b + d * d;
    return fast_log2(std::max(texdx, texdy), 64) / 2;
}

bool depth_test(uint32_t fbzmode, int32_t dest, int32_t src) {
    switch (bits(fbzmode, 5, 3)) {
    case 0: return false;
    case 1: return src < dest;
    case 2: return src == dest;
    case 3: return src <= dest;
    case 4: return src > dest;
    case 5: return src != dest;
    case 6: return src >= dest;
    default: return true;
    }
}

bool alpha_test(uint32_t alphamode, int32_t a, int32_t ref) {
    switch (bits(alphamode, 1, 3)) {
    case 0: return false;
    case 1: return a < ref;
    case 2: return a == ref;
    case 3: return a <= ref;
    case 4: return a > ref;
    case 5: return a != ref;
    case 6: return a >= ref;
    default: return true;
    }
}

struct Pipeline {
    VoodooState st;
    uint32_t fbzcp, fbzmode, alphamode, fogmode, texmode;
    C4 color0, color1;
    uint32_t chromakey, zacolor, stipple;
    C4 fogcolor;
    uint8_t fogblend[64], fogdelta[64];
    bool textured;
    Texture tex;
    uint16_t *dest, *depth;
    int rowpixels, yorigin;

    Pipeline(const VoodooState &s) : st(s) {
        const uint32_t *fbi = s.fbi;
        fbzcp = fbi[vFbzColorPath];
        fbzmode = fbi[vFbzMode];
        alphamode = fbi[vAlphaMode];
        fogmode = fbi[vFogMode];
        texmode = s.tmu[vTextureMode];
        color0 = C4::from_argb(fbi[vColor0]);
        color1 = C4::from_argb(fbi[vColor1]);
        chromakey = fbi[vChromaKey];
        zacolor = fbi[vZaColor];
        stipple = fbi[vStipple];
        fogcolor = C4::from_argb(fbi[vFogColor]);
        for (int i = 0; i < 32; i++) {
            uint32_t d = fbi[vFogTable + i];
            fogdelta[2 * i] = d & 0xFF;
            fogblend[2 * i] = (d >> 8) & 0xFF;
            fogdelta[2 * i + 1] = (d >> 16) & 0xFF;
            fogblend[2 * i + 1] = d >> 24;
        }
        textured = (fbzcp & (1u << 27)) != 0;
        if (textured) tex.setup(s);
        int buf = bits(fbzmode, 14, 2);
        uint32_t off = buf == 0 ? s.fb.front : buf == 1 ? s.fb.back : ~0u;
        dest = off == ~0u ? nullptr : (uint16_t *)(s.fb.ram + off);
        depth = s.fb.aux == ~0u ? nullptr : (uint16_t *)(s.fb.ram + s.fb.aux);
        rowpixels = (int)s.fb.rowpixels;
        yorigin = s.yorigin;
    }

    bool row_ok(int scry) const {
        return scry >= 0 && rowpixels > 0 && (size_t)((scry + 1) * rowpixels) * 2 <= (2u << 20);
    }

    int32_t depthval(int32_t wfloat, int32_t iterz) const {
        int32_t r = (fbzmode & (1 << 3)) ? wfloat : clamped_z(iterz, fbzcp);
        if (fbzmode & (1 << 16)) r = std::clamp(r + (int16_t)zacolor, 0, 0xFFFF);
        return r;
    }

    bool combine_color(C4 &color, C4 texel, int32_t iterz, int64_t iterw) const {
        C4 other;
        switch (bits(fbzcp, 0, 2)) {
        case 0: other = color; break;
        case 1: other = texel; break;
        case 2: other = color1; break;
        default: other = {0, 0, 0, 0}; break;
        }
        if ((fbzmode & (1 << 1)) && ((other.to_argb() ^ chromakey) & 0xFFFFFF) == 0) return false;
        switch (bits(fbzcp, 2, 2)) {
        case 0: other.a = color.a; break;
        case 1: other.a = texel.a; break;
        case 2: other.a = color1.a; break;
        default: other.a = 0; break;
        }
        if ((fbzmode & (1 << 13)) && !(other.a & 1)) return false;
        C4 local;
        bool use_color0 = (fbzcp & (1 << 7)) ? (texel.a & 0x80) != 0 : (fbzcp & (1 << 4)) != 0;
        local = use_color0 ? color0 : color;
        switch (bits(fbzcp, 5, 2)) {
        case 0: local.a = color.a; break;
        case 1: local.a = color0.a; break;
        case 2: local.a = (uint8_t)(clamped_z(iterz, fbzcp) >> 8); break;
        case 3: local.a = (uint8_t)clamped_w(iterw, fbzcp); break;
        }
        C4 blend = (fbzcp & (1 << 8)) ? C4{0, 0, 0, 0} : other;
        blend.a = (fbzcp & (1 << 17)) ? 0 : other.a;
        if (fbzcp & (1 << 9)) { blend.r -= local.r; blend.g -= local.g; blend.b -= local.b; }
        if (fbzcp & (1 << 18)) blend.a -= local.a;
        C4 f{0, 0, 0, 0};
        switch (bits(fbzcp, 10, 3)) {
        case 1: f.r = local.r; f.g = local.g; f.b = local.b; break;
        case 2: f.r = f.g = f.b = other.a; break;
        case 3: f.r = f.g = f.b = local.a; break;
        case 4: f.r = f.g = f.b = texel.a; break;
        case 5: f.r = texel.r; f.g = texel.g; f.b = texel.b; break;
        }
        switch (bits(fbzcp, 19, 3)) {
        case 1: case 3: f.a = local.a; break;
        case 2: f.a = other.a; break;
        case 4: f.a = texel.a; break;
        }
        if (!(fbzcp & (1 << 13))) { f.r ^= 0xFF; f.g ^= 0xFF; f.b ^= 0xFF; }
        if (!(fbzcp & (1 << 22))) f.a ^= 0xFF;
        C4 add{0, 0, 0, 0};
        switch (bits(fbzcp, 14, 2)) {
        case 1: add.r = local.r; add.g = local.g; add.b = local.b; break;
        case 2: add.r = add.g = add.b = local.a; break;
        }
        if (fbzcp & (3u << 23)) add.a = local.a;
        f.a++; f.r++; f.g++; f.b++;
        C4 out = scale_add_clamp(blend, f, add);
        if (fbzcp & (1 << 25)) out.a ^= 0xFF;
        if (fbzcp & (1 << 16)) { out.r ^= 0xFF; out.g ^= 0xFF; out.b ^= 0xFF; }
        color = out;
        return true;
    }

    void fog(C4 &color, const Dither &dither, int x, int32_t wfloat, int32_t iterz, int64_t iterw, int32_t itera) const {
        C4 fc = fogcolor;
        if (fogmode & (1 << 5)) {
            if (!(fogmode & (1 << 2))) { fc.r = clamp8(fc.r + color.r); fc.g = clamp8(fc.g + color.g); fc.b = clamp8(fc.b + color.b); }
        } else {
            int32_t blend = 0;
            if (fogmode & (1 << 1)) fc = {0, 0, 0, 0};
            if (!(fogmode & (1 << 2))) { fc.r -= color.r; fc.g -= color.g; fc.b -= color.b; }
            switch (bits(fogmode, 3, 2)) {
            case 0: {
                int32_t d = wfloat;
                if (fbzmode & (1 << 16)) d = std::clamp(d + (int16_t)zacolor, 0, 0xFFFF);
                int32_t delta = fogdelta[d >> 10];
                int32_t dv = (delta & 0xFF) * ((d >> 2) & 0xFF);
                dv >>= 6;
                dv >>= 4;
                blend = fogblend[d >> 10] + dv;
                break;
            }
            case 1: blend = itera; break;
            case 2: blend = clamped_z(iterz, fbzcp) >> 8; break;
            case 3: blend = clamped_w(iterw, fbzcp); break;
            }
            blend++;
            fc.r = clamp8((fc.r * (int16_t)blend) >> 8);
            fc.g = clamp8((fc.g * (int16_t)blend) >> 8);
            fc.b = clamp8((fc.b * (int16_t)blend) >> 8);
            if (!(fogmode & (1 << 2))) { fc.r = clamp8(fc.r + color.r); fc.g = clamp8(fc.g + color.g); fc.b = clamp8(fc.b + color.b); }
        }
        (void)dither;
        (void)x;
        fc.a = color.a;
        color = fc;
    }

    void blend(C4 &color, const Dither &dither, int x, uint16_t dpix, const uint16_t *depthrow, C4 prefog) const {
        C4 dst = C4::from_argb(tables().pen565[dpix]);
        int32_t da = 0xFF;
        if (fbzmode & (1 << 18)) dst.a = da = depthrow ? depthrow[x] : 0xFF;
        if (fbzmode & (1 << 19)) {
            int32_t d = (int32_t)dither.subtract(x);
            dst.r += d; dst.g += d >> 1; dst.b += d;
        }
        int32_t sa = color.a;
        auto factor = [&](int sel, bool src) -> C4 {
            int32_t t;
            switch (sel) {
            case 1: t = sa + 1; return {t, t, t, t};
            case 2: { C4 c = src ? dst : color; return {c.a + 1, c.r + 1, c.g + 1, c.b + 1}; }
            case 3: t = da + 1; return {t, t, t, t};
            case 4: return {256, 256, 256, 256};
            case 5: t = 0x100 - sa; return {t, t, t, t};
            case 6: { C4 c = src ? dst : color; return {0x100 - c.a, 0x100 - c.r, 0x100 - c.g, 0x100 - c.b}; }
            case 7: t = 0x100 - da; return {t, t, t, t};
            case 15:
                if (src) {
                    t = std::min(sa, 0x100 - da) + 1;
                    return {t, t, t, t};
                }
                return {prefog.a + 1, prefog.r + 1, prefog.g + 1, prefog.b + 1};
            default: return {0, 0, 0, 0};
            }
        };
        C4 ss = factor(bits(alphamode, 8, 4), true), ds = factor(bits(alphamode, 12, 4), false);
        ss.a = bits(alphamode, 16, 4) == 4 ? 256 : 0;
        ds.a = bits(alphamode, 20, 4) == 4 ? 256 : 0;
        color = {clamp8((color.a * ss.a + dst.a * ds.a) >> 8), clamp8((color.r * ss.r + dst.r * ds.r) >> 8),
                 clamp8((color.g * ss.g + dst.g * ds.g) >> 8), clamp8((color.b * ss.b + dst.b * ds.b) >> 8)};
    }

    void scanline(const VoodooTriangle &t, int y, int startx, int stopx, int32_t lodbase) {
        int scry = (fbzmode & (1 << 17)) ? yorigin - y : y;
        if (fbzmode & 1) {
            uint32_t clr = st.fbi[vClipLeftRight], cly = st.fbi[vClipLowYHighY];
            int cl = bits(clr, 16, 10), cr = bits(clr, 0, 10), ct = bits(cly, 16, 10), cb = bits(cly, 0, 10);
            if (scry < ct || scry >= cb) return;
            stopx = std::min(stopx, cr);
            startx = std::max(startx, cl);
        }
        startx = std::max(startx, 0);
        stopx = std::min(stopx, rowpixels);
        if (startx >= stopx || !dest || !row_ok(scry)) return;
        uint16_t *drow = dest + scry * rowpixels;
        uint16_t *zrow = depth && row_ok(scry) && (st.fb.aux + (size_t)(scry + 1) * rowpixels * 2 <= (2u << 20)) ? depth + scry * rowpixels : nullptr;

        int32_t dx = startx - (t.ax >> 4), dy = y - (t.ay >> 4);
        int32_t ir = (t.startr + dy * t.drdy + dx * t.drdx) << 8;
        int32_t ig = (t.startg + dy * t.dgdy + dx * t.dgdx) << 8;
        int32_t ib = (t.startb + dy * t.dbdy + dx * t.dbdx) << 8;
        int32_t ia = (t.starta + dy * t.dady + dx * t.dadx) << 8;
        int32_t iz = t.startz + dy * t.dzdy + dx * t.dzdx;
        int64_t iw = (t.startw + dy * t.dwdy + dx * t.dwdx) << 16;
        double is = (double)(t.starts + dy * t.dsdy + dx * t.dsdx);
        double it = (double)(t.startt + dy * t.dtdy + dx * t.dtdx);
        double iw0 = (double)(t.startw0 + dy * t.dw0dy + dx * t.dw0dx);
        Dither dither(scry, fbzmode);
        uint32_t stip = stipple;
        for (int x = startx; x < stopx; x++) {
            do {
                if (fbzmode & (1 << 2)) {
                    if (!(fbzmode & (1 << 12))) {
                        stip = std::rotr(stip, 1);
                        if ((int32_t)stip >= 0) break;
                    } else if (!((stip >> (((scry & 3) << 3) | (~x & 7))) & 1)) {
                        break;
                    }
                }
                int32_t wfloat = compute_wfloat(iw);
                int32_t dval = depthval(wfloat, iz);
                if ((fbzmode & (1 << 4)) && zrow &&
                    !depth_test(fbzmode, zrow[x], (fbzmode & (1 << 20)) ? (uint16_t)zacolor : dval))
                    break;
                C4 texel{0, 0, 0, 0};
                if (textured) {
                    int32_t lod = lodbase;
                    C4 t0 = tex.fetch(texmode, dither, x, is, it, iw0, lod);
                    texel = tex.combine(texmode, t0, C4{0, 0, 0, 0}, lod);
                }
                C4 color = clamped_argb(ia, ir, ig, ib, fbzcp);
                if (!combine_color(color, texel, iz, iw)) break;
                if ((alphamode & 1) && !alpha_test(alphamode, color.a, alphamode >> 24)) break;
                C4 prefog = color;
                if (fogmode & 1) fog(color, dither, x, wfloat, iz, iw, clamped_argb(ia, ir, ig, ib, fbzcp).a);
                if (alphamode & (1 << 4)) blend(color, dither, x, drow[x], zrow, prefog);
                if (fbzmode & (1 << 9)) drow[x] = dither.pixel(x, color);
                if ((fbzmode & (1 << 10)) && zrow) zrow[x] = (fbzmode & (1 << 18)) ? (uint16_t)color.a : (uint16_t)dval;
            } while (0);
            ir += t.drdx << 8;
            ig += t.dgdx << 8;
            ib += t.dbdx << 8;
            ia += t.dadx << 8;
            iz += t.dzdx;
            iw += t.dwdx << 16;
            is += (double)t.dsdx;
            it += (double)t.dtdx;
            iw0 += (double)t.dw0dx;
        }
    }
};

int round_coord(float v) {
    float ip = std::floor(v);
    return (int)ip + ((v - ip) > 0.5f ? 1 : 0);
}

} // namespace

void sw_triangle(const VoodooTriangle &t, const VoodooState &st) {
    Pipeline p(st);
    if (!p.dest) return;
    struct V { float x, y; } v[3] = {{t.ax / 16.0f, t.ay / 16.0f}, {t.bx / 16.0f, t.by / 16.0f}, {t.cx / 16.0f, t.cy / 16.0f}};
    V *v1 = &v[0], *v2 = &v[1], *v3 = &v[2];
    if (v2->y < v1->y) std::swap(v1, v2);
    if (v3->y < v2->y) {
        std::swap(v2, v3);
        if (v2->y < v1->y) std::swap(v1, v2);
    }
    int y1 = round_coord(v1->y), y3 = round_coord(v3->y);
    y1 = std::max(y1, -4096);
    y3 = std::min(y3, 4096);
    if (y3 <= y1) return;
    float d13 = v3->y == v1->y ? 0 : (v3->x - v1->x) / (v3->y - v1->y);
    float d12 = v2->y == v1->y ? 0 : (v2->x - v1->x) / (v2->y - v1->y);
    float d23 = v3->y == v2->y ? 0 : (v3->x - v2->x) / (v3->y - v2->y);
    int32_t lodbase = p.textured ? compute_lodbase(t.dsdx, t.dsdy, t.dtdx, t.dtdy) : 0;
    for (int y = y1; y < y3; y++) {
        float fy = y + 0.5f;
        float sx = v1->x + (fy - v1->y) * d13;
        float ex = fy < v2->y ? v1->x + (fy - v1->y) * d12 : v2->x + (fy - v2->y) * d23;
        int is = round_coord(sx), ie = round_coord(ex);
        if (is > ie) std::swap(is, ie);
        is = std::max(is, -4096);
        ie = std::min(ie, 4096);
        if (is < ie) p.scanline(t, y, is, ie, lodbase);
    }
}

// One pixel through the pipeline (LFB write with the pixel pipeline enabled): no colour combine.
volatile int sw_counts[4];

void sw_pixel(const VoodooState &st, int buffer, int x, int y, uint32_t argb, uint16_t z) {
    static Pipeline *cached;
    if (!cached || cached->st.generation != st.generation) {
        delete cached;
        cached = new Pipeline(st);
    }
    Pipeline &p = *cached;
    uint32_t off = buffer == 0 ? st.fb.front : st.fb.back;
    if (off == ~0u) return;
    uint16_t *dest = (uint16_t *)(st.fb.ram + off);
    uint32_t fbzmode = p.fbzmode;
    int scry = (fbzmode & (1 << 17)) ? p.yorigin - y : y;
    if (fbzmode & 1) {
        uint32_t clr = st.fbi[vClipLeftRight], cly = st.fbi[vClipLowYHighY];
        if (x < (int)bits(clr, 16, 10) || x >= (int)bits(clr, 0, 10) || scry < (int)bits(cly, 16, 10) ||
            scry >= (int)bits(cly, 0, 10))
            return;
    }
    if (x < 0 || x >= p.rowpixels || !p.row_ok(scry)) return;
    if (fbzmode & (1 << 2)) {
        uint32_t stip = p.stipple;
        if (!(fbzmode & (1 << 12))) {
            stip = std::rotr(stip, 1);
            if ((int32_t)stip >= 0) return;
        } else if (!((stip >> (((scry & 3) << 3) | (~x & 7))) & 1)) {
            return;
        }
    }
    uint16_t *drow = dest + scry * p.rowpixels;
    uint16_t *zrow = nullptr;
    if (p.depth && st.fb.aux + (size_t)(scry + 1) * p.rowpixels * 2 <= (2u << 20)) zrow = p.depth + scry * p.rowpixels;
    int32_t dval = z;
    if ((fbzmode & (1 << 4)) && zrow && !depth_test(fbzmode, zrow[x], (fbzmode & (1 << 20)) ? (uint16_t)p.zacolor : dval))
        return;
    C4 color = C4::from_argb(argb);
    if ((fbzmode & (1 << 1)) && ((argb ^ p.chromakey) & 0xFFFFFF) == 0) return;
    if ((fbzmode & (1 << 13)) && !(color.a & 1)) return;
    if ((p.alphamode & 1) && !alpha_test(p.alphamode, color.a, p.alphamode >> 24)) return;
    Dither dither(scry, fbzmode);
    C4 prefog = color;
    if (p.fogmode & 1) {
        int32_t iterz = z << 12;
        int64_t iterw = (st.fbi[vLfbMode] & (1 << 14)) ? (uint32_t)(p.zacolor << 16) : (uint32_t)(z << 16);
        p.fog(color, dither, x, dval, iterz, iterw, 0);
    }
    if (p.alphamode & (1 << 4)) p.blend(color, dither, x, drow[x], zrow, prefog);
    if (fbzmode & (1 << 9)) drow[x] = dither.pixel(x, color);
    if ((fbzmode & (1 << 10)) && zrow) zrow[x] = (fbzmode & (1 << 18)) ? (uint16_t)color.a : (uint16_t)dval;
}

void sw_fastfill(const VoodooState &st) {
    const uint32_t *fbi = st.fbi;
    uint32_t fbzmode = fbi[vFbzMode];
    static int n;

    if (n++ % 1 == 0 && n > 600 && n < 606)
        log("fastfill #%d fbz %08x clip %08x %08x color1 %08x za %08x front %x back %x", n, fbzmode, fbi[vClipLeftRight],
            fbi[vClipLowYHighY], fbi[vColor1], fbi[vZaColor], st.fb.front, st.fb.back);
    bool rgb = fbzmode & (1 << 9), aux = fbzmode & (1 << 10);
    if (!rgb && !aux) return;
    uint32_t clr = fbi[vClipLeftRight], cly = fbi[vClipLowYHighY];
    int cl = bits(clr, 16, 10), cr = bits(clr, 0, 10), ct = bits(cly, 16, 10), cb = bits(cly, 0, 10);
    int buf = bits(fbzmode, 14, 2);
    uint32_t off = buf == 0 ? st.fb.front : buf == 1 ? st.fb.back : ~0u;
    int rowpixels = (int)st.fb.rowpixels;
    if (!rowpixels) return;
    cr = std::min(cr, rowpixels);
    C4 c1 = C4::from_argb(fbi[vColor1]);
    uint16_t z = fbi[vZaColor] & 0xFFFF;
    for (int y = ct; y < cb; y++) {
        int scry = (fbzmode & (1 << 17)) ? st.yorigin - y : y;
        if (scry < 0) continue;
        Dither dither(y, fbzmode);
        if (rgb && off != ~0u && off + (size_t)(scry + 1) * rowpixels * 2 <= (2u << 20)) {
            uint16_t *row = (uint16_t *)(st.fb.ram + off) + scry * rowpixels;
            for (int x = cl; x < cr; x++) row[x] = dither.pixel(x, c1);
        }
        if (aux && st.fb.aux != ~0u && st.fb.aux + (size_t)(scry + 1) * rowpixels * 2 <= (2u << 20)) {
            uint16_t *row = (uint16_t *)(st.fb.ram + st.fb.aux) + scry * rowpixels;
            for (int x = cl; x < cr; x++) row[x] = z;
        }
    }
}

} // namespace rt

// ---- frame dumps ----

#include <cstdio>
#include <vector>

namespace rt {

void write_png(const std::string &path, int w, int h, const uint8_t *rgb) {
    // Uncompressed PNG: zlib stream of stored deflate blocks.
    std::vector<uint8_t> raw;
    for (int y = 0; y < h; y++) {
        raw.push_back(0);
        raw.insert(raw.end(), rgb + (size_t)y * w * 3, rgb + (size_t)(y + 1) * w * 3);
    }
    std::vector<uint8_t> z = {0x78, 0x01};
    for (size_t i = 0; i < raw.size(); i += 65535) {
        size_t n = std::min<size_t>(65535, raw.size() - i);
        z.push_back(i + n == raw.size());
        z.push_back(n & 0xFF); z.push_back(n >> 8);
        z.push_back(~n & 0xFF); z.push_back((~n >> 8) & 0xFF);
        z.insert(z.end(), raw.begin() + i, raw.begin() + i + n);
    }
    uint32_t a = 1, b = 0;
    for (uint8_t c : raw) { a = (a + c) % 65521; b = (b + a) % 65521; }
    uint32_t adler = (b << 16) | a;
    for (int i = 3; i >= 0; i--) z.push_back((adler >> (8 * i)) & 0xFF);
    static uint32_t crctab[256];
    if (!crctab[1])
        for (uint32_t n = 0; n < 256; n++) {
            uint32_t c = n;
            for (int k = 0; k < 8; k++) c = (c & 1) ? 0xEDB88320u ^ (c >> 1) : c >> 1;
            crctab[n] = c;
        }
    FILE *f = fopen(path.c_str(), "wb");
    if (!f) return;
    auto be32 = [&](uint32_t v) { uint8_t q[4] = {(uint8_t)(v >> 24), (uint8_t)(v >> 16), (uint8_t)(v >> 8), (uint8_t)v}; fwrite(q, 1, 4, f); };
    auto chunk = [&](const char *type, const std::vector<uint8_t> &data) {
        be32((uint32_t)data.size());
        uint32_t c = 0xFFFFFFFFu;
        auto upd = [&](uint8_t x) { c = crctab[(c ^ x) & 0xFF] ^ (c >> 8); };
        for (int i = 0; i < 4; i++) upd((uint8_t)type[i]);
        for (uint8_t x : data) upd(x);
        fwrite(type, 1, 4, f);
        fwrite(data.data(), 1, data.size(), f);
        be32(c ^ 0xFFFFFFFFu);
    };
    static const uint8_t sig[8] = {0x89, 'P', 'N', 'G', 13, 10, 26, 10};
    fwrite(sig, 1, 8, f);
    std::vector<uint8_t> ihdr = {(uint8_t)(w >> 24), (uint8_t)(w >> 16), (uint8_t)(w >> 8), (uint8_t)w,
                                 (uint8_t)(h >> 24), (uint8_t)(h >> 16), (uint8_t)(h >> 8), (uint8_t)h, 8, 2, 0, 0, 0};
    chunk("IHDR", ihdr);
    chunk("IDAT", z);
    chunk("IEND", {});
    fclose(f);
}

void SoftwareRenderer::swap(const VoodooState &) {}

void SoftwareRenderer::vblank(const VoodooState &st) {
    frame++;
    if (st.fb.front == ~0u || !st.fb.rowpixels) return;
    uint32_t dims = st.fbi[vVideoDimensions];
    int w = (int)(dims & 0x3FF), h = (int)((dims >> 16) & 0x3FF);
    if (w <= 0 || h <= 0 || w > (int)st.fb.rowpixels) { w = (int)st.fb.rowpixels; h = st.yorigin + 1; }
    if (st.fb.front + (size_t)h * st.fb.rowpixels * 2 <= (2u << 20))
        platform_present_565((const uint16_t *)(st.fb.ram + st.fb.front), w, h, (int)st.fb.rowpixels);
    if (dump_dir.empty() || frame % dump_every) return;
    std::vector<uint8_t> rgb(w * h * 3);
    const uint16_t *src = (const uint16_t *)(st.fb.ram + st.fb.front);
    for (int y = 0; y < h; y++)
        for (int x = 0; x < w; x++) {
            size_t o = (size_t)y * st.fb.rowpixels + x;
            uint16_t p = (st.fb.front + o * 2 + 2 <= (2u << 20)) ? src[o] : 0;
            uint8_t *d = &rgb[((size_t)y * w + x) * 3];
            d[0] = (uint8_t)(((p >> 11) & 31) * 255 / 31);
            d[1] = (uint8_t)(((p >> 5) & 63) * 255 / 63);
            d[2] = (uint8_t)((p & 31) * 255 / 31);
        }
    char name[64];
    snprintf(name, sizeof(name), "/frame%05d.png", frame);
    write_png(dump_dir + name, w, h, rgb.data());
}

} // namespace rt
