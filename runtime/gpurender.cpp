// GPU renderer for the emulated Voodoo: each triangle the game sets up is drawn on the host GPU, at
// an internal resolution of `scale` times the original.
//
// - Vertex values (colour, Z, W, S/T/W) are rebuilt from the hardware's start values and gradients,
//   and interpolated linearly in screen space as the Voodoo iterates them.
// - The fragment shader ports MAME's Voodoo pipeline (colour combine, texture combine, fog, alpha
//   test, chroma key, stipple; see swrender.cpp) in integer arithmetic; depth test, blending and the
//   clip rectangle use the GPU's.
// - Textures are decoded from texture memory into GPU textures (with their LODs as mipmaps) and kept
//   until the memory they came from is written.
// - LFB writes are composited into the GPU buffers; LFB reads copy the GPU buffers back into the
//   emulated frame buffer RAM first.
#include "gpurender.h"
#include "gl.h"
#include "platform.h"
#include "swrender.h"

#include <algorithm>
#include <cmath>
#include <cstring>
#include <string>
#include <unordered_map>
#include <vector>

namespace rt {
namespace {

inline uint32_t bits(uint32_t v, int pos, int n) { return (v >> pos) & ((1u << n) - 1); }

const char *VERTEX_SHADER = R"(#version 330 core
layout(location = 0) in vec2 a_pos;   // pixels, buffer rows from the top
layout(location = 1) in vec4 a_rgba;  // iterated colour, 8-bit units with fraction
layout(location = 2) in vec2 a_zw;    // Z (16-bit units), FBI W
layout(location = 3) in vec3 a_stw;   // TMU S, T, W
layout(location = 4) in vec4 a_strect; // the triangle's S, T range in texels (min, max), or min > max: none
uniform vec3 u_size;                  // buffer size in pixels, point size
noperspective out vec4 v_rgba;
noperspective out vec2 v_zw;
noperspective out vec3 v_stw;
flat out vec4 v_strect;
void main() {
    v_rgba = a_rgba;
    v_zw = a_zw;
    v_stw = a_stw;
    v_strect = a_strect;
    gl_Position = vec4(a_pos.x / u_size.x * 2.0 - 1.0, 1.0 - a_pos.y / u_size.y * 2.0, 0.0, 1.0);
    gl_PointSize = u_size.z;
}
)";

const char *FRAGMENT_SHADER = R"(#version 330 core
noperspective in vec4 v_rgba;
noperspective in vec2 v_zw;
noperspective in vec3 v_stw;
flat in vec4 v_strect;
layout(location = 0, index = 0) out vec4 o_color;
layout(location = 0, index = 1) out vec4 o_prefog;

uniform uint u_fbzcp, u_fbzmode, u_alphamode, u_fogmode, u_texmode;
uniform uint u_color0, u_color1, u_chromakey, u_zacolor, u_fogcolor, u_stipple;
uniform int u_textured, u_lfb;        // u_lfb: LFB pixel through the pipeline (colour given as is)
uniform int u_fogblend[64], u_fogdelta[64];
uniform vec2 u_texsize;               // LOD 0 size in texels
uniform float u_scale;
uniform float u_size_rows;
uniform sampler2D u_tex;

uint bf(uint v, int pos, int n) { return (v >> pos) & ((1u << n) - 1u); }
ivec4 argb(uint v) { return ivec4(int(bf(v, 24, 8)), int(bf(v, 16, 8)), int(bf(v, 8, 8)), int(bf(v, 0, 8))); } // a r g b
int clamp8(int v) { return clamp(v, 0, 255); }

int clamped_z(int iterz_int) {    // iterz >> 12, already the integer part
    if ((u_fbzcp & (1u << 28)) != 0u) return clamp(iterz_int, 0, 0xffff);
    int r = iterz_int & 0xfffff;
    if (r == 0xfffff) return 0;
    if (r == 0x10000) return 0xffff;
    return r & 0xffff;
}

int wfloat(float w) {
    if (w < 0.0 || w >= 1.0) return 0;
    if (w == 0.0) return 0xffff;
    int e = int(floor(-log2(w)));
    if (w * exp2(float(e)) >= 1.0) e--;       // guard against log2 rounding
    if (w * exp2(float(e + 1)) < 1.0) e++;
    if (e >= 16) return 0xffff;
    int m = int(floor((w * exp2(float(e + 1)) - 1.0) * 4096.0));
    m = clamp(m, 0, 4095);
    return ((e << 12) | (m ^ 0xfff)) + 1;
}

ivec4 clamped_argb(vec4 c) {      // c: a r g b iterated values
    ivec4 v = ivec4(floor(c));
    if ((u_fbzcp & (1u << 28)) != 0u) return clamp(v, 0, 255);
    ivec4 t = v & 0xfff;
    for (int i = 0; i < 4; i++) {
        if (t[i] == 0xfff) t[i] = 0;
        else if (t[i] == 0x100) t[i] = 0xff;
    }
    return t & 0xff;
}

// ivec4 colours are (a, r, g, b), like MAME's rgbaint_t.
ivec4 scale_add_clamp(ivec4 c, ivec4 s, ivec4 add) { return clamp(((c * s) >> 8) + add, 0, 255); }

ivec4 combine_texture(uint tm, ivec4 local, ivec4 other) {
    ivec4 blend = ((tm & (1u << 12)) != 0u) ? ivec4(other.a, 0, 0, 0) : other;
    blend.x = ((tm & (1u << 21)) != 0u) ? 0 : other.x;
    if ((tm & (1u << 13)) != 0u) blend.yzw -= local.yzw;
    if ((tm & (1u << 22)) != 0u) blend.x -= local.x;
    ivec4 f = ivec4(0);
    uint ms = bf(tm, 14, 3);
    if (ms == 1u) f.yzw = local.yzw;
    else if (ms == 2u) f.yzw = ivec3(other.x);
    else if (ms == 3u) f.yzw = ivec3(local.x);
    uint ma = bf(tm, 23, 3);
    if (ma == 1u || ma == 3u) f.x = local.x;
    else if (ma == 2u) f.x = other.x;
    if ((tm & (1u << 17)) == 0u) f.yzw ^= ivec3(0xff);
    if ((tm & (1u << 26)) == 0u) f.x ^= 0xff;
    f += 1;
    ivec4 add = ivec4(0);
    uint aa = bf(tm, 18, 2);
    if (aa == 1u) add.yzw = local.yzw;
    else if (aa == 2u) add.yzw = ivec3(local.x);
    if (bf(tm, 27, 2) != 0u) add.x = local.x;
    ivec4 o = scale_add_clamp(blend, f, add);
    if ((tm & (1u << 20)) != 0u) o.yzw ^= ivec3(0xff);
    if ((tm & (1u << 29)) != 0u) o.x ^= 0xff;
    return o;
}

bool keyed(ivec4 c) { return (uint(c.y << 16 | c.z << 8 | c.w) ^ (u_chromakey & 0xffffffu)) == 0u; }

// Bilinear magnification of a chroma-keyed texture, keying each texel before filtering: the
// hardware samples at pixel centres, where texels blend little, but at a higher resolution
// blends of keyed and opaque texels would no longer match the key and show as dark fringes.
ivec4 keyed_bilinear(vec2 st) {
    ivec2 size = textureSize(u_tex, 0);
    vec2 p = st / u_texsize * vec2(size) - 0.5;
    ivec2 i0 = ivec2(floor(p));
    vec2 f = p - floor(p);
    vec4 acc = vec4(0.0);
    float wsum = 0.0;
    ivec4 key = ivec4(0);
    for (int k = 0; k < 4; k++) {
        ivec2 o = ivec2(k & 1, k >> 1);
        ivec2 c = i0 + o;
        c.x = (u_texmode & (1u << 6)) != 0u ? clamp(c.x, 0, size.x - 1) : ((c.x % size.x) + size.x) % size.x;
        c.y = (u_texmode & (1u << 7)) != 0u ? clamp(c.y, 0, size.y - 1) : ((c.y % size.y) + size.y) % size.y;
        ivec4 comb = combine_texture(u_texmode, ivec4(round(texelFetch(u_tex, c, 0).argb * 255.0)), ivec4(0));
        float w = (o.x == 1 ? f.x : 1.0 - f.x) * (o.y == 1 ? f.y : 1.0 - f.y);
        if (keyed(comb)) { key = comb; continue; }
        acc += vec4(comb) * w;
        wsum += w;
    }
    return wsum > 0.0 ? ivec4(round(acc / wsum)) : key;
}

ivec4 texel_color() {
    vec3 stw = v_stw;
    vec2 st;
    if ((u_texmode & 1u) != 0u) st = stw.xy / stw.z;
    else st = stw.xy;
    if ((u_texmode & (1u << 3)) != 0u && stw.z < 0.0) st = vec2(0.0);
    // At the internal resolution, pixels at a triangle's edges sample closer to the edge than the
    // hardware's pixel centres do, and filtering would reach texels outside the triangle's part of
    // the texture (the next image in an atlas, or the far side through wrapping): keep the
    // coordinates within the texels the native pixels sample, half a native pixel inside the edges.
    vec2 dx = dFdx(st), dy = dFdy(st);
    if (v_strect.x <= v_strect.z) {
        vec2 inset = 0.5 * u_scale * (abs(dx) + abs(dy));
        vec2 lo = v_strect.xy + inset, hi = v_strect.zw - inset, mid = 0.5 * (v_strect.xy + v_strect.zw);
        st = mix(clamp(st, lo, hi), mid, vec2(greaterThan(lo, hi)));
    }
    // Chroma key on the texture colour, bilinear magnification: key before filtering.
    vec2 texels = vec2(textureSize(u_tex, 0)) / u_texsize; // GL level 0 texels per LOD 0 texel
    float rho = max(length(dx * texels), length(dy * texels));
    if ((u_fbzmode & (1u << 1)) != 0u && bf(u_fbzcp, 0, 2) == 1u && (u_texmode & (1u << 2)) != 0u && rho <= 1.0)
        return keyed_bilinear(st);
    vec4 t = textureGrad(u_tex, st / u_texsize, dx / u_texsize, dy / u_texsize);
    ivec4 local = ivec4(round(t.argb * 255.0)); // (a, r, g, b)
    return combine_texture(u_texmode, local, ivec4(0));
}

void main() {
    // stipple
    ivec2 px = ivec2(gl_FragCoord.xy / u_scale);
    if ((u_fbzmode & (1u << 2)) != 0u && (u_fbzmode & (1u << 12)) != 0u) {
        int scry = int(u_size_rows) - 1 - px.y;
        if (((u_stipple >> uint(((scry & 3) << 3) | (~px.x & 7))) & 1u) == 0u) discard;
    }
    int iterz = int(floor(v_zw.x));
    int wf = wfloat(v_zw.y);
    int depthval = ((u_fbzmode & (1u << 3)) != 0u) ? wf : clamped_z(iterz);
    if ((u_fbzmode & (1u << 16)) != 0u) depthval = clamp(depthval + int(int(u_zacolor << 16) >> 16), 0, 0xffff);

    ivec4 color;
    ivec4 itercolor = clamped_argb(v_rgba.wxyz);
    if (u_lfb != 0) {
        color = ivec4(round(v_rgba.wxyz));
        if ((u_fbzmode & (1u << 1)) != 0u && (uint(color.y << 16 | color.z << 8 | color.w) ^ (u_chromakey & 0xffffffu)) == 0u) discard;
        if ((u_fbzmode & (1u << 13)) != 0u && (color.x & 1) == 0) discard;
        depthval = int(v_zw.x);
    } else {
        ivec4 texel = (u_textured != 0) ? texel_color() : ivec4(0);
        ivec4 c0 = argb(u_color0), c1 = argb(u_color1);
        ivec4 other;
        uint rs = bf(u_fbzcp, 0, 2);
        other = rs == 0u ? itercolor : rs == 1u ? texel : rs == 2u ? c1 : ivec4(0);
        if ((u_fbzmode & (1u << 1)) != 0u && (uint(other.y << 16 | other.z << 8 | other.w) ^ (u_chromakey & 0xffffffu)) == 0u) discard;
        uint as = bf(u_fbzcp, 2, 2);
        other.x = as == 0u ? itercolor.x : as == 1u ? texel.x : as == 2u ? c1.x : 0;
        if ((u_fbzmode & (1u << 13)) != 0u && (other.x & 1) == 0) discard;
        bool use_c0 = ((u_fbzcp & (1u << 7)) != 0u) ? (texel.x & 0x80) != 0 : (u_fbzcp & (1u << 4)) != 0u;
        ivec4 local = use_c0 ? c0 : itercolor;
        uint ls = bf(u_fbzcp, 5, 2);
        local.x = ls == 0u ? itercolor.x : ls == 1u ? c0.x : ls == 2u ? (clamped_z(iterz) >> 8) & 0xff : 0;
        ivec4 blend = ((u_fbzcp & (1u << 8)) != 0u) ? ivec4(0) : other;
        blend.x = ((u_fbzcp & (1u << 17)) != 0u) ? 0 : other.x;
        if ((u_fbzcp & (1u << 9)) != 0u) blend.yzw -= local.yzw;
        if ((u_fbzcp & (1u << 18)) != 0u) blend.x -= local.x;
        ivec4 f = ivec4(0);
        uint ms = bf(u_fbzcp, 10, 3);
        if (ms == 1u) f.yzw = local.yzw;
        else if (ms == 2u) f.yzw = ivec3(other.x);
        else if (ms == 3u) f.yzw = ivec3(local.x);
        else if (ms == 4u) f.yzw = ivec3(texel.x);
        else if (ms == 5u) f.yzw = texel.yzw;
        uint ma = bf(u_fbzcp, 19, 3);
        if (ma == 1u || ma == 3u) f.x = local.x;
        else if (ma == 2u) f.x = other.x;
        else if (ma == 4u) f.x = texel.x;
        if ((u_fbzcp & (1u << 13)) == 0u) f.yzw ^= ivec3(0xff);
        if ((u_fbzcp & (1u << 22)) == 0u) f.x ^= 0xff;
        ivec4 add = ivec4(0);
        uint aa = bf(u_fbzcp, 14, 2);
        if (aa == 1u) add.yzw = local.yzw;
        else if (aa == 2u) add.yzw = ivec3(local.x);
        if (bf(u_fbzcp, 23, 2) != 0u) add.x = local.x;
        f += 1;
        color = scale_add_clamp(blend, f, add);
        if ((u_fbzcp & (1u << 25)) != 0u) color.x ^= 0xff;
        if ((u_fbzcp & (1u << 16)) != 0u) color.yzw ^= ivec3(0xff);
    }

    // alpha test
    if ((u_alphamode & 1u) != 0u) {
        int ref = int(bf(u_alphamode, 24, 8));
        uint fn = bf(u_alphamode, 1, 3);
        int a = color.x;
        bool pass = fn == 0u ? false : fn == 1u ? a < ref : fn == 2u ? a == ref : fn == 3u ? a <= ref :
                    fn == 4u ? a > ref : fn == 5u ? a != ref : fn == 6u ? a >= ref : true;
        if (!pass) discard;
    }

    ivec4 prefog = color;
    if ((u_fogmode & 1u) != 0u) {
        ivec4 fc = argb(u_fogcolor);
        if ((u_fogmode & (1u << 5)) != 0u) {
            if ((u_fogmode & (1u << 2)) == 0u) fc.yzw = clamp(fc.yzw + color.yzw, 0, 255);
        } else {
            int blendf = 0;
            if ((u_fogmode & (1u << 1)) != 0u) fc = ivec4(0);
            if ((u_fogmode & (1u << 2)) == 0u) fc.yzw -= color.yzw;
            uint zs = bf(u_fogmode, 3, 2);
            if (zs == 0u) {
                int d = wf;
                if ((u_fbzmode & (1u << 16)) != 0u) d = clamp(d + int(int(u_zacolor << 16) >> 16), 0, 0xffff);
                int delta = u_fogdelta[d >> 10];
                int dv = ((delta & 0xff) * ((d >> 2) & 0xff)) >> 10;
                blendf = u_fogblend[d >> 10] + dv;
            } else if (zs == 1u) {
                blendf = itercolor.x;
            } else if (zs == 2u) {
                blendf = clamped_z(iterz) >> 8;
            }
            blendf++;
            fc.yzw = clamp((fc.yzw * blendf) >> 8, 0, 255);
            if ((u_fogmode & (1u << 2)) == 0u) fc.yzw = clamp(fc.yzw + color.yzw, 0, 255);
        }
        color.yzw = fc.yzw;
    }

    o_color = vec4(color.yzw, color.x) / 255.0;
    o_prefog = vec4(prefog.yzw, prefog.x) / 255.0;
    gl_FragDepth = float(depthval) / 65535.0;
}
)";

// Draws a texture over the whole viewport (LFB composite and presentation).
const char *BLIT_VS = R"(#version 330 core
out vec2 uv;
uniform vec4 u_rect; // destination x, y, w, h in clip space
uniform vec4 u_src;  // source u, v, w, h
void main() {
    vec2 p = vec2(gl_VertexID & 1, gl_VertexID >> 1);
    uv = u_src.xy + p * u_src.zw;
    gl_Position = vec4(u_rect.xy + p * u_rect.zw, 0.0, 1.0);
}
)";
const char *BLIT_FS = R"(#version 330 core
in vec2 uv;
out vec4 color;
uniform sampler2D u_image;
uniform sampler2D u_lut;   // gamma (CLUT), 256 x 1
uniform int u_mode;        // 0 present through the CLUT, 1 LFB overlay (discard unwritten)
void main() {
    vec4 c = texture(u_image, uv);
    if (u_mode == 1) {
        if (c.a == 0.0) discard;
        color = vec4(c.rgb, 1.0);
        return;
    }
    color = vec4(texture(u_lut, vec2(c.r, 0.5)).r, texture(u_lut, vec2(c.g, 0.5)).g, texture(u_lut, vec2(c.b, 0.5)).b, 1.0);
}
)";

GLuint compile_program(const char *vs, const char *fs) {
    auto compile = [](GLenum type, const char *src) {
        GLuint s = glCreateShader(type);
        glShaderSource(s, 1, &src, nullptr);
        glCompileShader(s);
        GLint ok;
        glGetShaderiv(s, GL_COMPILE_STATUS, &ok);
        if (!ok) {
            char buf[8192];
            glGetShaderInfoLog(s, sizeof(buf), nullptr, buf);
            fatal("shader compile: %s", buf);
        }
        return s;
    };
    GLuint p = glCreateProgram();
    glAttachShader(p, compile(GL_VERTEX_SHADER, vs));
    glAttachShader(p, compile(GL_FRAGMENT_SHADER, fs));
    glLinkProgram(p);
    GLint ok;
    glGetProgramiv(p, GL_LINK_STATUS, &ok);
    if (!ok) {
        char buf[8192];
        glGetProgramInfoLog(p, sizeof(buf), nullptr, buf);
        fatal("shader link: %s", buf);
    }
    return p;
}

struct Vertex {
    float x, y;
    float r, g, b, a;
    float z, w;
    float s, t, tw;
    float smin, tmin, smax, tmax; // texture range of the triangle (smin > smax: none)
};

// Per-draw state that the shader or GL state depends on.
struct DrawState {
    uint32_t fbzcp, fbzmode, alphamode, fogmode, texmode, color0, color1, chromakey, zacolor, fogcolor, stipple;
    uint32_t clip_lr, clip_y, buffer;
    uint32_t texture; // GL texture name (0: none)
    float tex_w, tex_h;
    uint64_t fog_generation;
    int lfb;
    int squeeze; // a 2D overlay squeezed to the middle in widescreen: so is its clipping window
    bool operator==(const DrawState &o) const { return memcmp(this, &o, sizeof(*this)) == 0; }
};

struct ColorBuffer {
    GLuint tex = 0, fbo = 0;
    std::vector<uint32_t> lfb;  // pending raw LFB writes (RGBA8, alpha 0 = not written)
    int lfb_y0 = 1 << 30, lfb_y1 = -1;
    // Widescreen: how much of the frame the squeezed flat (2D) triangles cover.
    float flat_minx = 1e9f, flat_maxx = -1e9f, flat_area = 0;
    int depth_tris = 0; // non-flat (3D) triangles in the frame
    bool pillarbox = false; // the last frame shown from this buffer was a full-screen 2D picture
    void reset_frame_stats() {
        flat_minx = 1e9f, flat_maxx = -1e9f, flat_area = 0;
        depth_tris = 0;
    }
};

struct CachedTexture {
    GLuint tex;
    uint64_t key_hash;
    uint32_t page_lo, page_hi;
    uint64_t generation;  // sum of page generations when decoded
    uint64_t last_used;
    uint32_t params[4];   // sampler state last applied
    int lod_first;        // GL level 0 is this Voodoo LOD
};

struct GpuRenderer : Renderer {
    int scale;
    int width = 512, height = 384; // buffer size in pixels (from rowpixels / yorigin)
    GLuint program, blit_program, vao, vbo, blit_vao, lfb_tex, lut_tex;
    GLuint depth_tex = 0;
    std::unordered_map<uint32_t, ColorBuffer> buffers; // by frame buffer RAM offset
    std::vector<Vertex> batch;
    DrawState state{};
    bool have_state = false;
    GLenum batch_mode = GL_TRIANGLES;
    uint32_t front_offset = 0;
    uint64_t fog_generation = 1;
    int32_t fog_cache[64], fog_cache_delta[64];
    uint32_t fog_regs[32] = {};
    std::vector<uint64_t> page_gen = std::vector<uint64_t>(1024, 1);
    std::unordered_map<uint64_t, CachedTexture> textures;
    uint64_t frame = 0;
    bool lut_dirty = true;
    uint32_t clut[33];
    std::unordered_map<std::string, GLint> uniforms;

    GpuRenderer(int s) : scale(s) {
        program = compile_program(VERTEX_SHADER, FRAGMENT_SHADER);
        blit_program = compile_program(BLIT_VS, BLIT_FS);
        glGenVertexArrays(1, &vao);
        glGenBuffers(1, &vbo);
        glBindVertexArray(vao);
        glBindBuffer(GL_ARRAY_BUFFER, vbo);
        glEnableVertexAttribArray(0);
        glVertexAttribPointer(0, 2, GL_FLOAT, GL_FALSE, sizeof(Vertex), (void *)offsetof(Vertex, x));
        glEnableVertexAttribArray(1);
        glVertexAttribPointer(1, 4, GL_FLOAT, GL_FALSE, sizeof(Vertex), (void *)offsetof(Vertex, r));
        glEnableVertexAttribArray(2);
        glVertexAttribPointer(2, 2, GL_FLOAT, GL_FALSE, sizeof(Vertex), (void *)offsetof(Vertex, z));
        glEnableVertexAttribArray(3);
        glVertexAttribPointer(3, 3, GL_FLOAT, GL_FALSE, sizeof(Vertex), (void *)offsetof(Vertex, s));
        glEnableVertexAttribArray(4);
        glVertexAttribPointer(4, 4, GL_FLOAT, GL_FALSE, sizeof(Vertex), (void *)offsetof(Vertex, smin));
        glGenVertexArrays(1, &blit_vao);
        glGenTextures(1, &lfb_tex);
        glGenTextures(1, &lut_tex);
        for (int i = 0; i < 33; i++) clut[i] = (uint32_t)(i * 255 / 32) * 0x010101u;
        glEnable(GL_PROGRAM_POINT_SIZE);
    }

    GLint uni(const char *name) {
        auto it = uniforms.find(name);
        if (it != uniforms.end()) return it->second;
        GLint l = glGetUniformLocation(program, name);
        uniforms[name] = l;
        return l;
    }

    // ---- buffers ----

    void ensure_size(const VoodooState &st) {
        // The displayed size once the video registers are set up; 640x480 before that (start-up probes).
        int w = 640, h = 480;
        uint32_t dims = st.fbi[vVideoDimensions];
        if ((dims & 0x3FF) && ((dims >> 16) & 0x3FF)) {
            w = (int)(dims & 0x3FF) + 1; // the width field holds width - 1
            h = (int)((dims >> 16) & 0x3FF);
        }
        if (w == width && h == height && depth_tex) return;
        flush();
        width = w;
        height = h;
        for (auto &[off, b] : buffers) {
            glDeleteFramebuffers(1, &b.fbo);
            glDeleteTextures(1, &b.tex);
        }
        buffers.clear();
        if (depth_tex) glDeleteTextures(1, &depth_tex);
        glGenTextures(1, &depth_tex);
        glBindTexture(GL_TEXTURE_2D, depth_tex);
        glTexImage2D(GL_TEXTURE_2D, 0, GL_DEPTH_COMPONENT32F, width * scale, height * scale, 0, GL_DEPTH_COMPONENT, GL_FLOAT, nullptr);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
        log("gpu: %dx%d buffers at %dx", width, height, scale);
    }

    ColorBuffer &buffer(uint32_t offset) {
        ColorBuffer &b = buffers[offset];
        if (!b.tex) {
            glGenTextures(1, &b.tex);
            glBindTexture(GL_TEXTURE_2D, b.tex);
            glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA8, width * scale, height * scale, 0, GL_RGBA, GL_UNSIGNED_BYTE, nullptr);
            glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
            glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
            glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
            glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
            glGenFramebuffers(1, &b.fbo);
            glBindFramebuffer(GL_FRAMEBUFFER, b.fbo);
            glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, b.tex, 0);
            glFramebufferTexture2D(GL_FRAMEBUFFER, GL_DEPTH_ATTACHMENT, GL_TEXTURE_2D, depth_tex, 0);
            glDisable(GL_SCISSOR_TEST);
            glClearColor(0, 0, 0, 1);
            glClearDepth(1.0);
            glDepthMask(GL_TRUE);
            glColorMask(GL_TRUE, GL_TRUE, GL_TRUE, GL_TRUE);
            glClear(GL_COLOR_BUFFER_BIT | GL_DEPTH_BUFFER_BIT);
            have_state = false;
            b.lfb.assign((size_t)width * height, 0);
        }
        return b;
    }

    void bind_target(uint32_t offset) {
        ColorBuffer &b = buffer(offset);
        glBindFramebuffer(GL_FRAMEBUFFER, b.fbo);
        glViewport(0, 0, width * scale, height * scale);
    }

    // ---- LFB ----

    void lfb_write(int offset, int x, int y, uint32_t argb, bool rgb, bool, uint16_t, uint32_t) override {
        // Raw writes (no pixel pipeline): composited into the buffer before its next use.
        if (!rgb || x < 0 || y < 0 || x >= width || y >= height || offset < 0) return;
        ColorBuffer &b = buffer((uint32_t)offset);
        uint32_t r = (argb >> 16) & 0xFF, g = (argb >> 8) & 0xFF, bl = argb & 0xFF;
        b.lfb[(size_t)y * width + x] = 0xFF000000u | (bl << 16) | (g << 8) | r;
        b.lfb_y0 = std::min(b.lfb_y0, y);
        b.lfb_y1 = std::max(b.lfb_y1, y);
    }

    void flush_lfb(uint32_t offset) {
        auto it = buffers.find(offset);
        if (it == buffers.end()) return;
        ColorBuffer &b = it->second;
        if (b.lfb_y1 < b.lfb_y0) return;
        flush();
        int y0 = b.lfb_y0, rows = b.lfb_y1 - b.lfb_y0 + 1;
        glBindTexture(GL_TEXTURE_2D, lfb_tex);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
        glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA8, width, rows, 0, GL_RGBA, GL_UNSIGNED_BYTE, &b.lfb[(size_t)y0 * width]);
        bind_target(offset);
        glDisable(GL_DEPTH_TEST);
        glDisable(GL_BLEND);
        glDisable(GL_SCISSOR_TEST);
        glColorMask(GL_TRUE, GL_TRUE, GL_TRUE, GL_FALSE);
        glUseProgram(blit_program);
        glUniform1i(glGetUniformLocation(blit_program, "u_mode"), 1);
        glUniform1i(glGetUniformLocation(blit_program, "u_image"), 0);
        float top = 1.0f - 2.0f * y0 / height, h = 2.0f * rows / height;
        glUniform4f(glGetUniformLocation(blit_program, "u_rect"), -overlay_scale(), top, 2.0f * overlay_scale(), -h);
        glUniform4f(glGetUniformLocation(blit_program, "u_src"), 0, 0, 1, 1);
        glActiveTexture(GL_TEXTURE0);
        glBindTexture(GL_TEXTURE_2D, lfb_tex);
        glBindVertexArray(blit_vao);
        glDrawArrays(GL_TRIANGLE_STRIP, 0, 4);
        std::fill(b.lfb.begin() + (size_t)y0 * width, b.lfb.begin() + (size_t)(y0 + rows) * width, 0u);
        b.lfb_y0 = 1 << 30;
        b.lfb_y1 = -1;
        have_state = false;
    }

    // Widescreen: the 3D view is drawn 4/3 wider and squeezed (see rt_widescreen_view) and the
    // picture is shown at 16:9; the 2D overlays (flat triangles, LFB pixels) are squeezed to the
    // middle here so that they keep their shape, as on a 4:3 screen.
    float overlay_scale() const { return platform_widescreen() ? 0.75f : 1.0f; }
    float overlay_x(float x) const { return width * 0.5f + (x - width * 0.5f) * overlay_scale(); }

    // Raw LFB writes are kept aside and composited in one go; drawing on top of them needs them first.
    void composite_pending_lfb(uint32_t offset) {
        auto it = buffers.find(offset);
        if (it != buffers.end() && it->second.lfb_y1 >= it->second.lfb_y0) flush_lfb(offset);
    }

    void flush_all_lfb() {
        for (auto &[off, b] : buffers) flush_lfb(off);
    }

    void lfb_pipeline_write(const VoodooState &st, int which, int x, int y, uint32_t argb, uint16_t z) override {
        ensure_size(st);
        uint32_t off = which == 0 ? st.fb.front : st.fb.back;
        if (off == ~0u) return;
        int scry = (st.fbi[vFbzMode] & (1 << 17)) ? st.yorigin - y : y;
        DrawState ds = capture(st, 0, 1, off);
        composite_pending_lfb(off);
        use_state(ds, GL_POINTS);
        Vertex v{};
        v.smin = 1; // no texture range
        v.x = overlay_x(x + 0.5f);
        v.y = scry + 0.5f;
        v.r = (float)((argb >> 16) & 0xFF);
        v.g = (float)((argb >> 8) & 0xFF);
        v.b = (float)(argb & 0xFF);
        v.a = (float)(argb >> 24);
        v.z = z;
        batch.push_back(v);
    }

    // ---- textures ----

    struct TexInfo {
        uint32_t format, wmask, hmask, lodmask;
        uint32_t lodoffset[9];
        float lodmin, lodmax, lodbias;
    };

    TexInfo tex_info(const VoodooState &st) {
        const uint32_t *tmu = st.tmu;
        TexInfo ti{};
        uint32_t texmode = tmu[vTextureMode], tlod = tmu[vTLOD];
        ti.format = bits(texmode, 8, 4);
        ti.lodmin = bits(tlod, 0, 6) / 4.0f;
        ti.lodmax = bits(tlod, 6, 6) / 4.0f;
        ti.lodbias = (int8_t)(bits(tlod, 12, 6) << 2) / 16.0f;
        ti.lodmask = 0x1FF;
        if (tlod & (1u << 19)) ti.lodmask = (tlod & (1u << 18)) ? 0x0AA : 0x155;
        ti.wmask = ti.hmask = 0xFF;
        if (tlod & (1u << 20)) ti.hmask >>= bits(tlod, 21, 2);
        else ti.wmask >>= bits(tlod, 21, 2);
        int bppscale = ti.format >> 3;
        uint32_t base = tmu[vTexBaseAddr] & 0x7FFFF;
        uint32_t lodstart[9], offset = 0;
        for (int lod = 0; lod <= 8; lod++) {
            lodstart[lod] = offset;
            if (ti.lodmask & (1u << lod)) {
                uint32_t size = ((ti.wmask >> lod) + 1) * ((ti.hmask >> lod) + 1);
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
        for (int lod = 0; lod <= 8; lod++) ti.lodoffset[lod] = ((lodbase[lod] << 3) + lodstart[lod]) & 0x3FFFFF;
        return ti;
    }

    static uint64_t hash64(const void *p, size_t n, uint64_t h = 1469598103934665603ull) {
        const uint8_t *b = (const uint8_t *)p;
        for (size_t i = 0; i < n; i++) h = (h ^ b[i]) * 1099511628211ull;
        return h;
    }

    void build_lookup(const VoodooState &st, uint32_t format, uint32_t lut[256]) {
        auto ex = [](uint32_t v, int n) {
            uint32_t r = v << (8 - n);
            for (int s = n; s < 8; s += n) r |= r >> s;
            return r & 0xFF;
        };
        const uint32_t *tmu = st.tmu;
        switch (format) {
        case 0: case 8:
            for (uint32_t v = 0; v < 256; v++) lut[v] = 0xFF000000u | (ex(v >> 5, 3) << 16) | (ex((v >> 2) & 7, 3) << 8) | ex(v & 3, 2);
            break;
        case 2:
            for (uint32_t v = 0; v < 256; v++) lut[v] = v * 0x01010101u;
            break;
        case 3: case 13:
            for (uint32_t v = 0; v < 256; v++) lut[v] = 0xFF000000u | v * 0x010101u;
            break;
        case 4:
            for (uint32_t v = 0; v < 256; v++) {
                uint32_t a = ex(v >> 4, 4), i = ex(v & 15, 4);
                lut[v] = (a << 24) | i * 0x010101u;
            }
            break;
        case 1: case 9: {
            const uint32_t *regs = tmu + vNccTable0 + 12 * bits(tmu[vTextureMode], 5, 1);
            const uint8_t *y = (const uint8_t *)regs;
            for (int index = 0; index < 256; index++) {
                int32_t yy = y[bits(index, 4, 4)];
                int32_t i = (int32_t)regs[4 + bits(index, 2, 2)], q = (int32_t)regs[8 + bits(index, 0, 2)];
                auto s9 = [](int32_t v) { return (v << 23) >> 23; };
                int32_t r = std::clamp(yy + s9(i >> 18) + s9(q >> 18), 0, 255);
                int32_t g = std::clamp(yy + s9(i >> 9) + s9(q >> 9), 0, 255);
                int32_t b = std::clamp(yy + s9(i) + s9(q), 0, 255);
                lut[index] = 0xFF000000u | (r << 16) | (g << 8) | b;
            }
            break;
        }
        default:
            memcpy(lut, st.palette, 256 * 4);
            break;
        }
    }

    GLuint texture(const VoodooState &st) {
        TexInfo ti = tex_info(st);
        uint32_t lut[256] = {};
        bool needs_lut = ti.format < 8 || ti.format == 8 || ti.format == 9 || ti.format >= 13;
        if (needs_lut) build_lookup(st, ti.format, lut);
        // Only the LODs the texture can use (lodmin..lodmax) are decoded and watched for writes:
        // LOD 0 is always 256 texels wide, and textures that start at a smaller LOD share its
        // address range with others.
        int first = std::clamp((int)ti.lodmin, 0, 8), last = std::clamp((int)std::ceil(ti.lodmax), first, 8);
        uint64_t key = hash64(&ti.format, sizeof(uint32_t) * 4);
        key = hash64(ti.lodoffset, sizeof(ti.lodoffset), key);
        int range[2] = {first, last};
        key = hash64(range, sizeof(range), key);
        if (ti.format == 1 || ti.format == 5 || ti.format == 9 || ti.format == 14) key = hash64(lut, sizeof(lut), key); // NCC / palette
        uint32_t bpt = ti.format < 8 ? 1 : 2;
        auto source_lod = [&](int lod) { return (!(ti.lodmask & (1u << lod)) && lod < 8) ? lod + 1 : lod; }; // split LODs (one TMU)
        uint32_t lo = ~0u, hi = 0;
        for (int lod = first; lod <= last; lod++) {
            int src = source_lod(lod);
            uint32_t texels = ((ti.wmask >> src) + 1) * ((ti.hmask >> src) + 1);
            lo = std::min(lo, ti.lodoffset[src]);
            hi = std::max(hi, ti.lodoffset[src] + texels * bpt);
        }
        uint32_t page_lo = lo >> 12, page_hi = std::min<uint32_t>((hi + 4095) >> 12, 1024);
        uint64_t gen = 0;
        for (uint32_t p = page_lo; p < page_hi; p++) gen += page_gen[p];
        auto it = textures.find(key);
        if (it != textures.end() && it->second.generation == gen) {
            it->second.last_used = frame;
            apply_tex_params(it->second, st, ti);
            return it->second.tex;
        }
        // Decoding binds and uploads: draw what is pending first.
        flush();
        have_state = false;
        GLuint tex;
        if (it != textures.end()) tex = it->second.tex;
        else glGenTextures(1, &tex);
        glActiveTexture(GL_TEXTURE0);
        glBindTexture(GL_TEXTURE_2D, tex);
        std::vector<uint32_t> pixels;
        int levels = 0;
        for (int lod = first; lod <= last; lod++) {
            uint32_t w = (ti.wmask >> lod) + 1, h = (ti.hmask >> lod) + 1;
            pixels.resize((size_t)w * h);
            int src = source_lod(lod);
            uint32_t base = ti.lodoffset[src];
            for (uint32_t t = 0; t < h; t++)
                for (uint32_t s = 0; s < w; s++) {
                    uint32_t c;
                    uint32_t ss = src != lod ? s >> 1 : s, tt = src != lod ? t >> 1 : t;
                    uint32_t sw = (ti.wmask >> src) + 1;
                    uint32_t idx = tt * sw + ss;
                    if (bpt == 1) {
                        c = lut[st.tmu_mem[(base + idx) & 0x3FFFFF]];
                    } else {
                        uint16_t v;
                        memcpy(&v, &st.tmu_mem[(base + 2 * idx) & 0x3FFFFE], 2);
                        c = decode16(ti.format, v, lut);
                    }
                    // ARGB -> RGBA bytes
                    pixels[(size_t)t * w + s] = (c & 0xFF00FF00u) | ((c >> 16) & 0xFF) | ((c & 0xFF) << 16);
                }
            levels = lod - first;
            glTexImage2D(GL_TEXTURE_2D, levels, GL_RGBA8, w, h, 0, GL_RGBA, GL_UNSIGNED_BYTE, pixels.data());
            if (w == 1 && h == 1) break;
        }
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAX_LEVEL, levels);
        CachedTexture &ct = textures[key];
        ct = CachedTexture{tex, key, page_lo, page_hi, gen, frame, {~0u, ~0u, ~0u, ~0u}, first};
        apply_tex_params(ct, st, ti);
        return tex;
    }

    static uint32_t decode16(uint32_t format, uint16_t v, const uint32_t *lut) {
        auto ex = [](uint32_t x, int n) {
            uint32_t r = x << (8 - n);
            for (int s = n; s < 8; s += n) r |= r >> s;
            return r & 0xFF;
        };
        switch (format) {
        case 10: return 0xFF000000u | (ex(v >> 11, 5) << 16) | (ex((v >> 5) & 63, 6) << 8) | ex(v & 31, 5);
        case 11: return ((v & 0x8000) ? 0xFF000000u : 0) | (ex((v >> 10) & 31, 5) << 16) | (ex((v >> 5) & 31, 5) << 8) | ex(v & 31, 5);
        case 12: return (ex(v >> 12, 4) << 24) | (ex((v >> 8) & 15, 4) << 16) | (ex((v >> 4) & 15, 4) << 8) | ex(v & 15, 4);
        default: return (lut[v & 0xFF] & 0xFFFFFF) | ((uint32_t)(v & 0xFF00) << 16);
        }
    }

    void apply_tex_params(CachedTexture &ct, const VoodooState &st, const TexInfo &ti) {
        uint32_t texmode = st.tmu[vTextureMode];
        tex_size_w = (float)(ti.wmask + 1);
        tex_size_h = (float)(ti.hmask + 1);
        uint32_t p[4] = {texmode & 0xC6, st.tmu[vTLOD] & 0x3FFFF, 0, 0};
        if (!memcmp(p, ct.params, sizeof(p))) return;
        flush();
        have_state = false;
        memcpy(ct.params, p, sizeof(p));
        glActiveTexture(GL_TEXTURE0);
        glBindTexture(GL_TEXTURE_2D, ct.tex);
        bool minf = texmode & (1 << 1), magf = texmode & (1 << 2);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, minf ? GL_LINEAR_MIPMAP_NEAREST : GL_NEAREST_MIPMAP_NEAREST);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, magf ? GL_LINEAR : GL_NEAREST);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, (texmode & (1 << 6)) ? GL_CLAMP_TO_EDGE : GL_REPEAT);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, (texmode & (1 << 7)) ? GL_CLAMP_TO_EDGE : GL_REPEAT);
        glTexParameterf(GL_TEXTURE_2D, GL_TEXTURE_MIN_LOD, ti.lodmin - ct.lod_first);
        glTexParameterf(GL_TEXTURE_2D, GL_TEXTURE_MAX_LOD, ti.lodmax - ct.lod_first);
        glTexParameterf(GL_TEXTURE_2D, GL_TEXTURE_LOD_BIAS, ti.lodbias);
    }
    float tex_size_w = 256, tex_size_h = 256;

    void texture_written(uint32_t offset, uint32_t) override { page_gen[(offset >> 12) & 1023]++; }

    // ---- state ----

    DrawState capture(const VoodooState &st, uint32_t tex, int lfb, uint32_t target) {
        const uint32_t *fbi = st.fbi;
        DrawState d{};
        d.fbzcp = fbi[vFbzColorPath];
        d.fbzmode = fbi[vFbzMode];
        d.alphamode = fbi[vAlphaMode];
        d.fogmode = fbi[vFogMode];
        d.texmode = tex ? st.tmu[vTextureMode] : 0;
        d.color0 = fbi[vColor0];
        d.color1 = fbi[vColor1];
        d.chromakey = fbi[vChromaKey];
        d.zacolor = fbi[vZaColor];
        d.fogcolor = fbi[vFogColor];
        d.stipple = fbi[vStipple];
        d.clip_lr = fbi[vClipLeftRight];
        d.clip_y = fbi[vClipLowYHighY];
        d.buffer = target;
        d.texture = tex;
        d.tex_w = tex ? tex_size_w : 0;
        d.tex_h = tex ? tex_size_h : 0;
        if (memcmp(fog_regs, fbi + vFogTable, sizeof(fog_regs))) {
            memcpy(fog_regs, fbi + vFogTable, sizeof(fog_regs));
            fog_generation++;
        }
        d.fog_generation = fog_generation;
        d.lfb = lfb;
        return d;
    }

    static GLenum blend_factor(uint32_t f, bool src) {
        switch (f) {
        case 0: return GL_ZERO;
        case 1: return GL_SRC_ALPHA;
        case 2: return src ? GL_DST_COLOR : GL_SRC_COLOR;
        case 3: return GL_DST_ALPHA;
        case 4: return GL_ONE;
        case 5: return GL_ONE_MINUS_SRC_ALPHA;
        case 6: return src ? GL_ONE_MINUS_DST_COLOR : GL_ONE_MINUS_SRC_COLOR;
        case 7: return GL_ONE_MINUS_DST_ALPHA;
        case 15: return src ? GL_SRC_ALPHA_SATURATE : GL_SRC1_COLOR;
        default: return GL_ZERO;
        }
    }

    void use_state(const DrawState &d, GLenum mode) {
        if (have_state && d == state && mode == batch_mode) return;
        flush();
        if (!have_state || d.buffer != state.buffer) {
            flush_lfb(d.buffer);
            bind_target(d.buffer);
        }
        state = d;
        batch_mode = mode;
        have_state = true;
        glUseProgram(program);
        glUniform1ui(uni("u_fbzcp"), d.fbzcp);
        glUniform1ui(uni("u_fbzmode"), d.fbzmode);
        glUniform1ui(uni("u_alphamode"), d.alphamode);
        glUniform1ui(uni("u_fogmode"), d.fogmode);
        glUniform1ui(uni("u_texmode"), d.texmode);
        glUniform1ui(uni("u_color0"), d.color0);
        glUniform1ui(uni("u_color1"), d.color1);
        glUniform1ui(uni("u_chromakey"), d.chromakey);
        glUniform1ui(uni("u_zacolor"), d.zacolor);
        glUniform1ui(uni("u_fogcolor"), d.fogcolor);
        glUniform1ui(uni("u_stipple"), d.stipple);
        glUniform1i(uni("u_textured"), d.texture ? 1 : 0);
        glUniform1i(uni("u_lfb"), d.lfb);
        glUniform3f(uni("u_size"), (float)width, (float)height, (float)scale);
        glUniform1f(uni("u_scale"), (float)scale);
        glUniform1f(uni("u_size_rows"), (float)height);
        glUniform2f(uni("u_texsize"), d.tex_w, d.tex_h);
        int blend[64], delta[64];
        for (int i = 0; i < 32; i++) {
            delta[2 * i] = fog_regs[i] & 0xFF;
            blend[2 * i] = (fog_regs[i] >> 8) & 0xFF;
            delta[2 * i + 1] = (fog_regs[i] >> 16) & 0xFF;
            blend[2 * i + 1] = fog_regs[i] >> 24;
        }
        glUniform1iv(uni("u_fogblend"), 64, blend);
        glUniform1iv(uni("u_fogdelta"), 64, delta);
        glActiveTexture(GL_TEXTURE0);
        glBindTexture(GL_TEXTURE_2D, d.texture);
        glUniform1i(uni("u_tex"), 0);

        // depth
        uint32_t fz = d.fbzmode;
        bool alpha_planes = fz & (1 << 18);
        bool depth_test = (fz & (1 << 4)) && !alpha_planes;
        bool depth_write = (fz & (1 << 10)) && !alpha_planes;
        static const GLenum funcs[8] = {GL_NEVER, GL_LESS, GL_EQUAL, GL_LEQUAL, GL_GREATER, GL_NOTEQUAL, GL_GEQUAL, GL_ALWAYS};
        if (depth_test || depth_write) {
            glEnable(GL_DEPTH_TEST);
            glDepthFunc(depth_test ? funcs[bits(fz, 5, 3)] : GL_ALWAYS);
        } else {
            glDisable(GL_DEPTH_TEST);
        }
        glDepthMask(depth_write ? GL_TRUE : GL_FALSE);
        bool rgb = fz & (1 << 9);
        glColorMask(rgb, rgb, rgb, alpha_planes && (fz & (1 << 10)));
        // blending
        uint32_t am = d.alphamode;
        if (am & (1 << 4)) {
            glEnable(GL_BLEND);
            glBlendFuncSeparate(blend_factor(bits(am, 8, 4), true), blend_factor(bits(am, 12, 4), false),
                                bits(am, 16, 4) == 4 ? GL_ONE : GL_ZERO, bits(am, 20, 4) == 4 ? GL_ONE : GL_ZERO);
        } else {
            glDisable(GL_BLEND);
        }
        // clipping
        if (d.squeeze) {
            // A squeezed overlay is clipped where it would be on the 4:3 screen, the frame's edges
            // included: what the game draws past them (a sprite sliding in from the side) stays hidden.
            int cl = 0, cr = width, ct = 0, cb = height;
            if (fz & 1) {
                cl = (int)bits(d.clip_lr, 16, 10), cr = std::min((int)bits(d.clip_lr, 0, 10), width);
                ct = (int)bits(d.clip_y, 16, 10), cb = std::min((int)bits(d.clip_y, 0, 10), height);
            }
            int x0 = (int)std::lround(overlay_x((float)cl) * scale), x1 = (int)std::lround(overlay_x((float)cr) * scale);
            glEnable(GL_SCISSOR_TEST);
            glScissor(x0, (height - cb) * scale, std::max(0, x1 - x0), std::max(0, cb - ct) * scale);
        } else if (fz & 1) {
            int cl = (int)bits(d.clip_lr, 16, 10), cr = (int)bits(d.clip_lr, 0, 10);
            int ct = (int)bits(d.clip_y, 16, 10), cb = (int)bits(d.clip_y, 0, 10);
            cr = std::min(cr, width);
            cb = std::min(cb, height);
            glEnable(GL_SCISSOR_TEST);
            glScissor(cl * scale, (height - cb) * scale, std::max(0, cr - cl) * scale, std::max(0, cb - ct) * scale);
        } else {
            glDisable(GL_SCISSOR_TEST);
        }
    }

    void flush() {
        if (batch.empty()) return;
        glBindVertexArray(vao);
        glBindBuffer(GL_ARRAY_BUFFER, vbo);
        glBufferData(GL_ARRAY_BUFFER, batch.size() * sizeof(Vertex), batch.data(), GL_STREAM_DRAW);
        glDrawArrays(batch_mode, 0, (GLsizei)batch.size());
        batch.clear();
    }

    // ---- drawing ----

    void triangle(const VoodooTriangle &t, const VoodooState &st) override {
        ensure_size(st);
        uint32_t fz = st.fbi[vFbzMode];
        int buf = bits(fz, 14, 2);
        uint32_t off = buf == 0 ? st.fb.front : buf == 1 ? st.fb.back : ~0u;
        if (off == ~0u) return;
        bool textured = st.fbi[vFbzColorPath] & (1u << 27);
        GLuint tex = textured ? texture(st) : 0;
        if (textured) {
            TexInfo ti = tex_info(st);
            tex_size_w = (float)(ti.wmask + 1);
            tex_size_h = (float)(ti.hmask + 1);
        }
        // Values at a point: start + (p - 0.5 - floor(A)) * d/dx ... (the start values belong to the
        // centre of vertex A's pixel, as the hardware iterates them).
        float ax = (float)(t.ax >> 4), ay = (float)(t.ay >> 4);
        bool flip = fz & (1 << 17);
        // Flat (constant W) triangles are the 2D overlays: their vertices are snapped to the native
        // pixel grid as the hardware's coverage rule rounds them, so that pieces placed side by side
        // cover whole pixels as they do at the native resolution (no gaps, no half-covered columns).
        bool flat = t.dwdx == 0 && t.dwdy == 0 && t.dw0dx == 0 && t.dw0dy == 0;
        auto snap = [&](int32_t v16) {
            float v = v16 / 16.0f;
            if (!flat) return v;
            float ip = std::floor(v);
            return ip + ((v - ip) > 0.5f ? 1.0f : 0.0f);
        };
        // A flat triangle across the whole width drawn over a 3D scene is a full-screen effect (the
        // motion blur redraws the frame through such strips): in widescreen it spans the whole width.
        // Without 3D in the frame it is part of a 2D picture, squeezed like the other overlays.
        bool fullscreen = false;
        if (flat && buffer(off).depth_tris > 0) {
            float x0 = std::min({snap(t.ax), snap(t.bx), snap(t.cx)}), x1 = std::max({snap(t.ax), snap(t.bx), snap(t.cx)});
            fullscreen = x0 <= 1 && x1 >= width - 1;
        }
        if (!flat) buffer(off).depth_tris++;
        DrawState ds = capture(st, tex, 0, off);
        ds.squeeze = flat && !fullscreen && platform_widescreen();
        composite_pending_lfb(off);
        use_state(ds, GL_TRIANGLES);
        auto vertex = [&](int32_t vx16, int32_t vy16) {
            float x = snap(vx16), y = snap(vy16);
            double dx = x - 0.5 - ax, dy = y - 0.5 - ay;
            Vertex v;
            v.x = flat && !fullscreen ? overlay_x(x) : x;
            v.y = flip ? (st.yorigin + 1) - y : y;
            v.r = (float)((t.startr + dx * t.drdx + dy * t.drdy) / 4096.0);
            v.g = (float)((t.startg + dx * t.dgdx + dy * t.dgdy) / 4096.0);
            v.b = (float)((t.startb + dx * t.dbdx + dy * t.dbdy) / 4096.0);
            v.a = (float)((t.starta + dx * t.dadx + dy * t.dady) / 4096.0);
            v.z = (float)((t.startz + dx * t.dzdx + dy * t.dzdy) / 4096.0);
            v.w = (float)(((double)t.startw + dx * (double)t.dwdx + dy * (double)t.dwdy) / 4294967296.0);
            v.s = (float)(((double)t.starts + dx * (double)t.dsdx + dy * (double)t.dsdy) / 4294967296.0);
            v.t = (float)(((double)t.startt + dx * (double)t.dtdx + dy * (double)t.dtdy) / 4294967296.0);
            v.tw = (float)(((double)t.startw0 + dx * (double)t.dw0dx + dy * (double)t.dw0dy) / 4294967296.0);
            v.smin = 1;
            v.smax = 0;
            batch.push_back(v);
        };
        vertex(t.ax, t.ay);
        vertex(t.bx, t.by);
        vertex(t.cx, t.cy);
        if (flat && !fullscreen) {
            ColorBuffer &cb = buffer(off);
            float x0 = snap(t.ax), y0 = snap(t.ay), x1 = snap(t.bx), y1 = snap(t.by), x2 = snap(t.cx), y2 = snap(t.cy);
            cb.flat_minx = std::min({cb.flat_minx, x0, x1, x2});
            cb.flat_maxx = std::max({cb.flat_maxx, x0, x1, x2});
            cb.flat_area += std::abs((x1 - x0) * (y2 - y0) - (x2 - x0) * (y1 - y0)) * 0.5f;
        }
        if (textured) {
            // The texels the triangle covers; used to keep filtering inside them (see texel_color)
            // when they lie within the texture, i.e. the texture is not tiled across the triangle.
            Vertex *v = &batch[batch.size() - 3];
            bool persp = st.tmu[vTextureMode] & 1;
            float smin = 1e30f, tmin = 1e30f, smax = -1e30f, tmax = -1e30f;
            bool ok = true;
            for (int i = 0; i < 3; i++) {
                if (persp && v[i].tw <= 0) ok = false;
                float s = persp && v[i].tw > 0 ? v[i].s / v[i].tw : v[i].s, tt = persp && v[i].tw > 0 ? v[i].t / v[i].tw : v[i].t;
                smin = std::min(smin, s), smax = std::max(smax, s), tmin = std::min(tmin, tt), tmax = std::max(tmax, tt);
            }
            const float e = 0.01f;
            if (ok && smin >= -e && tmin >= -e && smax <= tex_size_w + e && tmax <= tex_size_h + e)
                for (int i = 0; i < 3; i++) v[i].smin = smin, v[i].tmin = tmin, v[i].smax = smax, v[i].tmax = tmax;
        }
    }

    void fastfill(const VoodooState &st) override {
        ensure_size(st);
        const uint32_t *fbi = st.fbi;
        uint32_t fz = fbi[vFbzMode];
        bool rgb = fz & (1 << 9), aux = fz & (1 << 10);
        if (!rgb && !aux) return;
        int buf = bits(fz, 14, 2);
        uint32_t off = buf == 0 ? st.fb.front : buf == 1 ? st.fb.back : ~0u;
        if (off == ~0u) return;
        flush();
        flush_lfb(off);
        bind_target(off);
        have_state = false;
        int cl = (int)bits(fbi[vClipLeftRight], 16, 10), cr = std::min((int)bits(fbi[vClipLeftRight], 0, 10), width);
        int ct = (int)bits(fbi[vClipLowYHighY], 16, 10), cb = std::min((int)bits(fbi[vClipLowYHighY], 0, 10), height);
        if (fz & (1 << 17)) { // y origin at the bottom: rows are flipped
            int t2 = st.yorigin + 1 - cb, b2 = st.yorigin + 1 - ct;
            ct = std::max(t2, 0);
            cb = std::min(b2, height);
        }
        glEnable(GL_SCISSOR_TEST);
        glScissor(cl * scale, (height - cb) * scale, std::max(0, cr - cl) * scale, std::max(0, cb - ct) * scale);
        uint32_t c1 = fbi[vColor1];
        glClearColor(((c1 >> 16) & 0xFF) / 255.0f, ((c1 >> 8) & 0xFF) / 255.0f, (c1 & 0xFF) / 255.0f, 1.0f);
        glClearDepth((fbi[vZaColor] & 0xFFFF) / 65535.0);
        glColorMask(rgb, rgb, rgb, GL_FALSE);
        glDepthMask(aux ? GL_TRUE : GL_FALSE);
        glClear((rgb ? GL_COLOR_BUFFER_BIT : 0) | (aux ? GL_DEPTH_BUFFER_BIT : 0));

    }

    void swap(const VoodooState &st) override {
        flush();
        front_offset = st.fb.front;
        // The frame just finished is in the new front buffer: decide on side bars, start counting anew.
        if (st.fb.front != ~0u) {
            ColorBuffer &f = buffer(st.fb.front);
            f.pillarbox = f.flat_minx <= 1 && f.flat_maxx >= width - 1 && f.flat_area >= 0.75f * width * height;
            f.reset_frame_stats();
        }
        if (st.fb.back != ~0u) buffer(st.fb.back).reset_frame_stats();
    }

    void clut_changed(const uint32_t *c) override {
        memcpy(clut, c, sizeof(clut));
        lut_dirty = true;
    }

    void vblank(const VoodooState &st) override {
        frame++;
        ensure_size(st);
        flush();
        uint32_t front = st.fb.front;
        flush_lfb(front);
        dump_full(front);
        present(front);
        // Drop textures not used for a while.
        if (frame % 600 == 0)
            for (auto it = textures.begin(); it != textures.end();)
                if (frame - it->second.last_used > 1200) {
                    glDeleteTextures(1, &it->second.tex);
                    it = textures.erase(it);
                } else {
                    ++it;
                }
    }

    // ARC_DUMP_GPU=dir: the front buffer at full internal resolution (ARC_DUMP_EVERY, ARC_DUMP_FROM).
    void dump_full(uint32_t front) {
        static const char *dir = getenv("ARC_DUMP_GPU");
        static int every = getenv("ARC_DUMP_EVERY") ? std::max(1, atoi(getenv("ARC_DUMP_EVERY"))) : 60;
        static int from = getenv("ARC_DUMP_FROM") ? atoi(getenv("ARC_DUMP_FROM")) : 0;
        if (!dir || frame % every || (int)frame < from) return;
        auto it = buffers.find(front);
        if (it == buffers.end()) return;
        int w = width * scale, h = height * scale;
        std::vector<uint32_t> px((size_t)w * h);
        glBindFramebuffer(GL_FRAMEBUFFER, it->second.fbo);
        glReadPixels(0, 0, w, h, GL_RGBA, GL_UNSIGNED_BYTE, px.data());
        std::vector<uint8_t> rgb((size_t)w * h * 3);
        for (int y = 0; y < h; y++)
            for (int x = 0; x < w; x++) {
                uint32_t c = px[(size_t)(h - 1 - y) * w + x];
                uint8_t *d = &rgb[((size_t)y * w + x) * 3];
                d[0] = c & 0xFF;
                d[1] = (c >> 8) & 0xFF;
                d[2] = (c >> 16) & 0xFF;
            }
        char name[64];
        snprintf(name, sizeof(name), "/full%05d.png", (int)frame);
        write_png(dir + std::string(name), w, h, rgb.data());
        have_state = false;
    }

    void present(uint32_t front) {
        if (lut_dirty) {
            // As MAME: each 8-bit value interpolates between the 33 CLUT entries.
            uint8_t lut[256 * 4];
            for (int v = 0; v < 256; v++)
                for (int ch = 0; ch < 3; ch++) {
                    int sh = 16 - 8 * ch;
                    int i = v >> 3, f = v & 7;
                    int a = (clut[i] >> sh) & 0xFF, b = (clut[std::min(i + 1, 32)] >> sh) & 0xFF;
                    lut[v * 4 + ch] = (uint8_t)((a * (8 - f) + b * f) >> 3);
                    lut[v * 4 + 3] = 255;
                }
            bool identity_like = true;
            for (int i = 0; i < 33; i++) identity_like &= (clut[i] & 0xFFFFFF) == 0;
            if (identity_like)
                for (int v = 0; v < 256; v++) lut[v * 4] = lut[v * 4 + 1] = lut[v * 4 + 2] = (uint8_t)v;
            glBindTexture(GL_TEXTURE_2D, lut_tex);
            glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA8, 256, 1, 0, GL_RGBA, GL_UNSIGNED_BYTE, lut);
            glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
            glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
            glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
            lut_dirty = false;
        }
        auto it = buffers.find(front);
        if (it == buffers.end()) return;
        int ww, wh;
        platform_window_size(ww, wh);
        glBindFramebuffer(GL_FRAMEBUFFER, 0);
        glViewport(0, 0, ww, wh);
        glDisable(GL_SCISSOR_TEST);
        glDisable(GL_DEPTH_TEST);
        glDisable(GL_BLEND);
        glColorMask(GL_TRUE, GL_TRUE, GL_TRUE, GL_TRUE);
        glClearColor(0, 0, 0, 1);
        glClear(GL_COLOR_BUFFER_BIT);
        float aspect = (float)ww / wh, w = 2, h = 2, target = platform_display_aspect();
        if (aspect > target) w = 2 * target / aspect;
        else h = 2 * aspect / target;
        glUseProgram(blit_program);
        glUniform1i(glGetUniformLocation(blit_program, "u_mode"), 0);
        glUniform1i(glGetUniformLocation(blit_program, "u_image"), 0);
        glUniform1i(glGetUniformLocation(blit_program, "u_lut"), 1);
        glUniform4f(glGetUniformLocation(blit_program, "u_rect"), -w / 2, h / 2, w, -h);
        glUniform4f(glGetUniformLocation(blit_program, "u_src"), 0, 1, 1, -1);
        glActiveTexture(GL_TEXTURE1);
        glBindTexture(GL_TEXTURE_2D, lut_tex);
        glActiveTexture(GL_TEXTURE0);
        glBindTexture(GL_TEXTURE_2D, it->second.tex);
        glBindVertexArray(blit_vao);
        glDrawArrays(GL_TRIANGLE_STRIP, 0, 4);
        // Widescreen: a full-screen 2D picture (menus made of pieces) is squeezed to the middle and does not hide
        // what is behind it any more (the game does not clear under it): black bars at the sides.
        const ColorBuffer &cb = it->second;
        if (platform_widescreen() && cb.pillarbox) {
            int pw = (int)std::lround(w / 2 * ww), px0 = (ww - pw) / 2, bar = (int)std::lround(pw * (1 - overlay_scale()) / 2);
            glEnable(GL_SCISSOR_TEST);
            glClearColor(0, 0, 0, 1);
            glScissor(px0, 0, bar, wh);
            glClear(GL_COLOR_BUFFER_BIT);
            glScissor(px0 + pw - bar, 0, bar, wh);
            glClear(GL_COLOR_BUFFER_BIT);
            glDisable(GL_SCISSOR_TEST);
        }
        platform_swap();
        have_state = false;
    }

    // Uploads what the frame buffer RAM holds (when taking over from the software renderer).
    void load_from_memory(const VoodooState &st) {
        ensure_size(st);
        std::vector<uint32_t> px((size_t)width * height);
        for (uint32_t off : {st.fb.front, st.fb.back}) {
            if (off == ~0u) continue;
            ColorBuffer &b = buffer(off);
            for (int y = 0; y < height; y++) {
                size_t o = off + (size_t)y * st.fb.rowpixels * 2;
                if (o + (size_t)width * 2 > (2u << 20)) break;
                const uint16_t *src = (const uint16_t *)(st.fb.ram + o);
                for (int x = 0; x < width; x++) {
                    uint16_t p = src[x];
                    uint32_t r = (p >> 11) * 255 / 31, g = ((p >> 5) & 63) * 255 / 63, bl = (p & 31) * 255 / 31;
                    b.lfb[(size_t)y * width + x] = 0xFF000000u | (bl << 16) | (g << 8) | r;
                }
            }
            b.lfb_y0 = 0;
            b.lfb_y1 = height - 1;
            flush_lfb(off);
        }
    }

    // ---- reading back ----

    void sync_to_memory(const FrameBufferMemory &fb) override {
        flush();
        flush_all_lfb();
        std::vector<uint32_t> px((size_t)width * height);
        for (uint32_t off : {fb.front, fb.back}) {
            auto it = buffers.find(off);
            if (off == ~0u || it == buffers.end()) continue;
            // Downscale to the native size, then read.
            GLuint tmp_tex, tmp_fbo;
            glGenTextures(1, &tmp_tex);
            glBindTexture(GL_TEXTURE_2D, tmp_tex);
            glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA8, width, height, 0, GL_RGBA, GL_UNSIGNED_BYTE, nullptr);
            glGenFramebuffers(1, &tmp_fbo);
            glBindFramebuffer(GL_DRAW_FRAMEBUFFER, tmp_fbo);
            glFramebufferTexture2D(GL_DRAW_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, tmp_tex, 0);
            glBindFramebuffer(GL_READ_FRAMEBUFFER, it->second.fbo);
            glDisable(GL_SCISSOR_TEST);
            glBlitFramebuffer(0, 0, width * scale, height * scale, 0, 0, width, height, GL_COLOR_BUFFER_BIT, GL_NEAREST);
            glBindFramebuffer(GL_READ_FRAMEBUFFER, tmp_fbo);
            glReadPixels(0, 0, width, height, GL_RGBA, GL_UNSIGNED_BYTE, px.data());
            glDeleteFramebuffers(1, &tmp_fbo);
            glDeleteTextures(1, &tmp_tex);
            for (int y = 0; y < height; y++) {
                // GL rows are bottom-up; buffer row y is GL row height-1-y.
                const uint32_t *src = &px[(size_t)(height - 1 - y) * width];
                size_t o = off + (size_t)y * fb.rowpixels * 2;
                if (o + (size_t)width * 2 > (2u << 20)) break;
                uint16_t *dst = (uint16_t *)(fb.ram + o);
                for (int x = 0; x < width; x++) {
                    uint32_t c = src[x];
                    dst[x] = (uint16_t)(((c & 0xF8) << 8) | ((c >> 5) & 0x7E0) | ((c >> 19) & 0x1F));
                }
            }
        }
        glBindFramebuffer(GL_FRAMEBUFFER, 0);
        have_state = false;
    }
};

} // namespace

// Starts with the software rasterizer (Glide's start-up probes draw and read back exact pixels),
// and moves to the GPU once the game has set up its video mode and swaps buffers.
struct StartupSwitch : Renderer {
    Renderer *sw, *gpu;
    bool on_gpu = false;
    StartupSwitch(Renderer *s, Renderer *g) : sw(s), gpu(g) {}
    Renderer *r() { return on_gpu ? gpu : sw; }
    void triangle(const VoodooTriangle &t, const VoodooState &st) override { r()->triangle(t, st); }
    void fastfill(const VoodooState &st) override { r()->fastfill(st); }
    void swap(const VoodooState &st) override {
        r()->swap(st);
        uint32_t dims = st.fbi[vVideoDimensions];
        if (!on_gpu && (dims & 0x3FF) && ((dims >> 16) & 0x3FF)) {
            on_gpu = true;
            static_cast<GpuRenderer *>(gpu)->load_from_memory(st);
            log("gpu: taking over from the software renderer");
        }
    }
    void lfb_write(int b, int x, int y, uint32_t argb, bool rgb, bool depth, uint16_t z, uint32_t mode) override {
        r()->lfb_write(b, x, y, argb, rgb, depth, z, mode);
    }
    void lfb_pipeline_write(const VoodooState &st, int b, int x, int y, uint32_t argb, uint16_t z) override {
        r()->lfb_pipeline_write(st, b, x, y, argb, z);
    }
    void sync_to_memory(const FrameBufferMemory &fb) override { r()->sync_to_memory(fb); }
    void texture_written(uint32_t o, uint32_t n) override {
        sw->texture_written(o, n);
        gpu->texture_written(o, n);
    }
    void clut_changed(const uint32_t *c) override {
        sw->clut_changed(c);
        gpu->clut_changed(c);
    }
    void vblank(const VoodooState &st) override { r()->vblank(st); }
};

// Debugging (ARC_RENDERER=both): every command goes to both renderers; the software one owns the frame
// buffer RAM. ARC_DUMP_FRAMES then also writes the GPU's frame (gpuNNNNN.png) next to the software one.
struct Both : Renderer {
    Renderer *sw;
    GpuRenderer *gpu;
    std::string dir;
    int frame = 0, every = 60, from = 0;
    Both(Renderer *s, GpuRenderer *g) : sw(s), gpu(g) {
        if (const char *d = getenv("ARC_DUMP_FRAMES")) dir = d;
        if (const char *d = getenv("ARC_DUMP_EVERY")) every = std::max(1, atoi(d));
        if (const char *d = getenv("ARC_DUMP_FROM")) from = atoi(d);
    }
    void triangle(const VoodooTriangle &t, const VoodooState &st) override {
        sw->triangle(t, st);
        gpu->triangle(t, st);
    }
    void fastfill(const VoodooState &st) override {
        sw->fastfill(st);
        gpu->fastfill(st);
    }
    void swap(const VoodooState &st) override {
        sw->swap(st);
        gpu->swap(st);
    }
    void lfb_write(int b, int x, int y, uint32_t argb, bool rgb, bool depth, uint16_t z, uint32_t mode) override {
        sw->lfb_write(b, x, y, argb, rgb, depth, z, mode);
        gpu->lfb_write(b, x, y, argb, rgb, depth, z, mode);
    }
    void lfb_pipeline_write(const VoodooState &st, int b, int x, int y, uint32_t argb, uint16_t z) override {
        sw->lfb_pipeline_write(st, b, x, y, argb, z);
        gpu->lfb_pipeline_write(st, b, x, y, argb, z);
    }
    void texture_written(uint32_t o, uint32_t n) override { gpu->texture_written(o, n); }
    void clut_changed(const uint32_t *c) override { gpu->clut_changed(c); }
    void vblank(const VoodooState &st) override {
        // The software renderer presents through GL: the GPU renderer's batch goes first, and its
        // state is set again afterwards.
        gpu->flush();
        sw->vblank(st);
        gpu->have_state = false;
        frame++;
        if (dir.empty() || frame % every || frame < from) return;
        gpu->flush();
        gpu->flush_all_lfb();
        gpu->frame = frame;
        gpu->dump_full(st.fb.front);
        auto it = gpu->buffers.find(st.fb.front);
        if (it == gpu->buffers.end()) return;
        int w = gpu->width, h = gpu->height, sc = gpu->scale;
        std::vector<uint32_t> px((size_t)w * h * sc * sc);
        glBindFramebuffer(GL_FRAMEBUFFER, it->second.fbo);
        glReadPixels(0, 0, w * sc, h * sc, GL_RGBA, GL_UNSIGNED_BYTE, px.data());
        std::vector<uint8_t> rgb((size_t)w * h * 3);
        for (int y = 0; y < h; y++)
            for (int x = 0; x < w; x++) {
                uint32_t c = px[(size_t)((h - 1 - y) * sc + sc / 2) * w * sc + x * sc + sc / 2];
                uint8_t *d = &rgb[((size_t)y * w + x) * 3];
                d[0] = c & 0xFF;
                d[1] = (c >> 8) & 0xFF;
                d[2] = (c >> 16) & 0xFF;
            }
        char name[64];
        snprintf(name, sizeof(name), "/gpu%05d.png", frame);
        write_png(dir + name, w, h, rgb.data());
        gpu->have_state = false;
    }
};

Renderer *gpu_renderer_create(int scale, Renderer *software) {
    if (const char *r = getenv("ARC_RENDERER"); r && !strcmp(r, "both")) {
        GpuRenderer *g = new GpuRenderer(scale);
        return new Both(software, g);
    }
    return new StartupSwitch(software, new GpuRenderer(scale));
}

} // namespace rt
