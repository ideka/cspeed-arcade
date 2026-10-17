// 3dfx Voodoo Graphics (SST-1): FBI + one TMU with 4 MB, 2 MB frame buffer, at PCI slot 8 mapped at
// 0x08000000. Register behaviour follows MAME's voodoo.cpp; pixels are produced by a Renderer.
//
// Address space (byte offsets in the 16 MB BAR):
//   0x000000-0x3FFFFF  registers: bits 2-9 register, bits 10-13 chip mask (FBI, TMU0, TMU1, TMU2),
//                      bit 21 alternate (remapped) triangle register order
//   0x400000-0x7FFFFF  linear frame buffer (1024-pixel rows)
//   0x800000-0xFFFFFF  texture memory downloads: bits 2-8 s, 9-16 t, 17-20 LOD
#include "voodoo.h"
#include "board.h"
#include "galileo.h"

#include <cstring>
#include <deque>

namespace rt { volatile uint64_t swap_count; } // buffer swaps, for the frame rate

namespace {

using namespace rt;

const uint8_t ALIAS[0x40] = {
    0x00, 0x01, 0x02, 0x03, 0x04, 0x05, 0x06, 0x07,
    0x08, 0x10, 0x18, 0x09, 0x11, 0x19, 0x0A, 0x12,
    0x1A, 0x0B, 0x13, 0x1B, 0x0C, 0x14, 0x1C, 0x0D,
    0x15, 0x1D, 0x0E, 0x16, 0x1E, 0x0F, 0x17, 0x1F,
    0x20, 0x21, 0x22, 0x23, 0x24, 0x25, 0x26, 0x27,
    0x28, 0x30, 0x38, 0x29, 0x31, 0x39, 0x2A, 0x32,
    0x3A, 0x2B, 0x33, 0x3B, 0x2C, 0x34, 0x3C, 0x2D,
    0x35, 0x3D, 0x2E, 0x36, 0x3E, 0x2F, 0x37, 0x3F,
};

const uint64_t FRAME_NS = 1000000000ull / 57;
const uint64_t VBLANK_LEN_NS = FRAME_NS * 45 / 525; // ~45 of 525 lines

float as_float(uint32_t v) { float f; memcpy(&f, &v, 4); return f; }

struct Voodoo : Device {
    Renderer *renderer;
    uint32_t fbi[256] = {};
    uint32_t tmu[256] = {};
    std::vector<uint8_t> tmu_mem = std::vector<uint8_t>(4 << 20);
    uint32_t init_enable = 0;
    uint8_t dac_reg[8] = {};
    uint32_t dac_read = 0;
    uint32_t clut[33] = {};
    VoodooTriangle tri{};
    int frontbuf = 0, backbuf = 1;
    int swaps_pending = 0;
    int swap_interval = 0, vblank_count = 0;
    uint64_t frame_start = 0;
    bool in_vblank = false;

    Voodoo(Renderer *r) : Device("voodoo", 0x08000000, 0x01000000), renderer(r) {}

    // ---- triangle parameters ----

    // idx: integer-register index 0x02..0x1F (start values 0x08-0x0F, d/dX 0x10-0x17, d/dY 0x18-0x1F,
    // each in the order R G B Z A S T W). v32 for the FBI's 32-bit values, v64 for S/T/W.
    void set_param(int idx, int32_t v32, int64_t v64, unsigned chipmask) {
        VoodooTriangle &t = tri;
        switch (idx) {
        case vVertexAx: t.ax = v32; return;
        case vVertexAy: t.ay = v32; return;
        case vVertexBx: t.bx = v32; return;
        case vVertexBy: t.by = v32; return;
        case vVertexCx: t.cx = v32; return;
        case vVertexCy: t.cy = v32; return;
        }
        int set = (idx - 8) >> 3; // 0 start, 1 d/dX, 2 d/dY
        int32_t *rgbza[3][5] = {{&t.startr, &t.startg, &t.startb, &t.startz, &t.starta},
                                {&t.drdx, &t.dgdx, &t.dbdx, &t.dzdx, &t.dadx},
                                {&t.drdy, &t.dgdy, &t.dbdy, &t.dzdy, &t.dady}};
        int64_t *st[3][2] = {{&t.starts, &t.startt}, {&t.dsdx, &t.dtdx}, {&t.dsdy, &t.dtdy}};
        int64_t *w[3] = {&t.startw, &t.dwdx, &t.dwdy}, *w0[3] = {&t.startw0, &t.dw0dx, &t.dw0dy};
        int p = idx & 7;
        if (p <= 4) {
            if (chipmask & 1) *rgbza[set][p] = v32;
        } else if (p <= 6) {
            if (chipmask & 2) *st[set][p - 5] = v64;
        } else {
            if (chipmask & 1) *w[set] = v64;
            if (chipmask & 2) *w0[set] = v64;
        }
    }

    void write_param_int(int idx, uint32_t v, unsigned chipmask) {
        if (idx <= vVertexCy) return set_param(idx, (int16_t)v, 0, chipmask);
        switch (idx & 7) {
        case 0: case 1: case 2: case 4: return set_param(idx, (int32_t)(v << 8) >> 8, 0, chipmask); // 12.12, 24 bits
        case 3: return set_param(idx, (int32_t)v, 0, chipmask);                                      // Z 20.12
        case 5: case 6: return set_param(idx, 0, (int64_t)(int32_t)v << 14, chipmask);               // 14.18
        default: return set_param(idx, 0, (int64_t)(int32_t)v << 2, chipmask);                       // W 2.30
        }
    }

    void write_param_float(int idx, uint32_t v, unsigned chipmask) {
        double f = as_float(v);
        auto i32 = [](double x) { return (int32_t)(int64_t)x; };
        if (idx <= vVertexCy) return set_param(idx, i32(f * 16.0), 0, chipmask);
        if ((idx & 7) <= 4) return set_param(idx, i32(f * 4096.0), 0, chipmask);
        return set_param(idx, 0, (int64_t)(f * 4294967296.0), chipmask);
    }

    // Subpixel correction: move the start values from vertex A to the centre of its pixel.
    void subpixel_adjust() {
        VoodooTriangle &t = tri;
        int32_t dx = 8 - (t.ax & 15), dy = 8 - (t.ay & 15);
        t.startr += (dy * t.drdy + dx * t.drdx) >> 4;
        t.startg += (dy * t.dgdy + dx * t.dgdx) >> 4;
        t.startb += (dy * t.dbdy + dx * t.dbdx) >> 4;
        t.starta += (dy * t.dady + dx * t.dadx) >> 4;
        t.startz += (dy * t.dzdy + dx * t.dzdx) >> 4;
        t.startw += (dy * t.dwdy + dx * t.dwdx) >> 4;
        t.startw0 += (dy * t.dw0dy + dx * t.dw0dx) >> 4;
        t.starts += (dy * t.dsdy + dx * t.dsdx) >> 4;
        t.startt += (dy * t.dtdy + dx * t.dtdx) >> 4;
    }

    VoodooState state() {
        return VoodooState{fbi, tmu, tmu_mem.data(), palette, this_fb(), yorigin(), generation};
    }

    // ---- video timing ----

    void update(uint64_t now) override {
        if (!frame_start) frame_start = now;
        while (now >= frame_start + FRAME_NS) {
            frame_start += FRAME_NS;
            if (now - frame_start > 4 * FRAME_NS) frame_start = now; // fell far behind: resync
            in_vblank = false;
            vblank_begin();
        }
        bool v = now < frame_start + VBLANK_LEN_NS;
        if (in_vblank && !v) board_vblank(false);
        in_vblank = v;
    }

    void vblank_begin() {
        in_vblank = true;
        board_vblank(true);
        // As MAME: a swap synced to the retrace happens at the first vblank at least `interval`
        // vblanks after the previous swap. What the FIFO held meanwhile runs after the new front
        // buffer is presented, so that the host's present does not wait for that drawing too.
        vblank_count = std::min(vblank_count + 1, 250);
        bool swapped = retrace_wait && vblank_count >= swap_interval;
        if (swapped) do_swap();
        renderer->vblank(state());
        if (swapped) drain_fifo();
    }

    void do_swap() {
        swap_count++;
        std::swap(frontbuf, backbuf);
        generation++;
        if (swaps_pending) swaps_pending--;
        vblank_count = 0;
        retrace_wait = false;
        renderer->swap(state());
    }

    // ---- command FIFO ----
    // A swap waiting for the retrace stalls the hardware's FIFO: register, LFB and texture writes
    // after it are held until the swap has happened, while the CPU keeps running. Executing them
    // at once would draw the next frame's clear and first triangles into the frame about to be shown.
    struct FifoEntry {
        uint32_t off, v;
        int size;
    };
    std::deque<FifoEntry> fifo;
    bool retrace_wait = false;

    void drain_fifo() {
        while (!retrace_wait && !fifo.empty()) {
            FifoEntry e = fifo.front();
            fifo.pop_front();
            execute(e.off, e.v, e.size);
        }
    }

    // ---- register access ----

    uint32_t status() {
        uint32_t r = 0x3F;                       // PCI FIFO free
        if (in_vblank) r |= 1 << 6;
        r |= frontbuf << 10;
        r |= 0xFFFFu << 12;                      // memory FIFO free
        r |= (uint32_t)std::min(swaps_pending, 7) << 28;
        if (swaps_pending) r |= 7 << 7;          // busy while a swap is queued
        return r;
    }

    uint32_t reg_read(int reg) {
        switch (reg) {
        case vStatus: return status();
        case vFbiInit2: return (init_enable & 4) ? dac_read : fbi[reg];
        case vVRetrace: {
            if (in_vblank) return 0;
            uint64_t into = now_ns() - frame_start;
            return (uint32_t)(into * 525 / FRAME_NS) & 0xFFF;
        }
        }
        return fbi[reg];
    }

    uint64_t generation = 1;

    void reg_write(int reg, uint32_t v, unsigned chipmask) {
        generation++;
        // Triangle setup parameters, integer (0x02-0x1F) and float (0x22-0x3F).
        if (reg >= 0x02 && reg < 0x20) {
            write_param_int(reg, v, chipmask);
            fbi[reg] = v;
            return;
        }
        if (reg >= 0x22 && reg < 0x40) {
            write_param_float(reg - 0x20, v, chipmask);
            return;
        }
        if (reg >= 0xC0 && reg < 0xE1) {         // TMU registers 0x300-0x380
            if (chipmask & 2) {
                // NCC table 0 I/Q entries with bit 31 set load the palette instead.
                int ncc = reg - vNccTable0;
                if ((v & 0x80000000u) && ncc >= 4 && ncc < 12)
                    palette[((v >> 23) & 0xFE) | (ncc & 1)] = 0xFF000000u | (v & 0xFFFFFF);
                else
                    tmu[reg] = v;
            }
            return;
        }
        switch (reg) {
        case vTriangleCMD:
        case vFtriangleCMD:
            if (fbi[vFbzColorPath] & (1u << 26)) subpixel_adjust();
            renderer->triangle(tri, state());
            return;
        case vNopCMD:
            return;
        case vFastfillCMD:
            renderer->fastfill(state());
            return;
        case vSwapbufferCMD:
            // (counted in swaps_pending when written) Synced to the retrace, or at once.
            swap_interval = (v >> 1) & 0xFF;
            if (v & 1) retrace_wait = true;
            else do_swap();
            return;
        case vFbiInit0: case vFbiInit1: case vFbiInit2: case vFbiInit3: case vFbiInit4:
            if ((chipmask & 1) && (init_enable & 1)) {
                fbi[reg] = v;
                if (reg == vFbiInit1 || reg == vFbiInit2) recompute_video_memory();
            }
            return;
        case vClutData:
            if (chipmask & 1) {
                int i = (v >> 24) & 0x3F;
                if (i <= 32) {
                    clut[i] = v & 0xFFFFFF;
                    renderer->clut_changed(clut);
                }
            }
            return;
        case vDacData:
            if (chipmask & 1) {
                int r = (v >> 8) & 7;
                if (!(v & 0x800)) dac_reg[r] = v & 0xFF;
                else {
                    dac_read = dac_reg[r];
                    switch (dac_reg[7]) {
                    case 0x01: dac_read = 0x55; break;
                    case 0x07: dac_read = 0x71; break;
                    case 0x0B: dac_read = 0x79; break;
                    }
                }
            }
            return;
        }
        if (chipmask & 1) fbi[reg] = v;
        if (reg >= vFogTable && reg < vFogTable + 32) fbi[reg] = v;
    }

    uint32_t palette[256] = {};

    // ---- frame buffer memory ----
    // The 2 MB of frame buffer RAM, laid out as the hardware does (16-bit pixels, rows of rowpixels):
    // LFB reads and writes and Glide's memory probing use it. The renderer draws on the host GPU and
    // also receives every LFB write.

    std::vector<uint8_t> fbram = std::vector<uint8_t>(2 << 20);
    uint32_t rgboffs[3] = {0, 0, ~0u}, auxoffs = ~0u, rowpixels = 0;

    void recompute_video_memory() {
        uint32_t config = (fbi[vFbiInit2] >> 4) & 1;
        rowpixels = ((fbi[vFbiInit1] >> 4) & 15) * 64;
        uint32_t pages = (fbi[vFbiInit2] >> 11) & 0x1FF;
        rgboffs[0] = 0;
        rgboffs[1] = pages * 0x1000;
        if (config == 0) {
            rgboffs[2] = ~0u;
            auxoffs = 2 * pages * 0x1000;
        } else {
            rgboffs[2] = 2 * pages * 0x1000;
            auxoffs = ~0u;
        }
        for (uint32_t &o : rgboffs)
            if (o != ~0u && o > 0x1FFFFF) o = 0x1FFFFF;
        if (auxoffs != ~0u && auxoffs > 0x1FFFFF) auxoffs = 0x1FFFFF;
    }

    uint16_t *pixel_ptr(uint32_t bufoff, int x, int y) {
        if (bufoff == ~0u || x < 0 || y < 0) return nullptr;
        uint32_t o = bufoff + ((uint32_t)y * rowpixels + (uint32_t)x) * 2;
        if (o + 2 > fbram.size()) return nullptr;
        return (uint16_t *)&fbram[o];
    }
    uint32_t color_buffer(int which) { return which == 0 ? rgboffs[frontbuf] : which == 1 ? rgboffs[backbuf] : ~0u; }

    int yorigin() { return (fbi[vFbiInit3] >> 22) & 0x3FF; }

    FrameBufferMemory this_fb() {
        return FrameBufferMemory{fbram.data(), rgboffs[frontbuf], rgboffs[backbuf], auxoffs, rowpixels};
    }

    // Stores one pixel written through the LFB (without the pixel pipeline).
    void lfb_store(int buffer, int x, int y, uint32_t argb, bool rgb, bool alpha, bool depth, uint16_t z) {
        uint32_t mode = fbi[vLfbMode];
        if (mode & (1 << 8)) {
            if (rgb && buffer <= 1) renderer->lfb_pipeline_write(state(), buffer, x, y, argb, z);
            return;
        }
        int scry = (mode & (1 << 13)) ? yorigin() - y : y;
        if (rgb) {
            if (uint16_t *p = pixel_ptr(color_buffer(buffer), x, scry)) {
                uint32_t r = (argb >> 16) & 0xFF, g = (argb >> 8) & 0xFF, b = argb & 0xFF;
                *p = (uint16_t)(((r >> 3) << 11) | ((g >> 2) << 5) | (b >> 3));
            }
        }
        if (uint16_t *p = pixel_ptr(auxoffs, x, scry)) {
            if (fbi[vFbzMode] & (1 << 18)) {
                if (alpha) *p = (uint16_t)(argb >> 24);
            } else if (depth) {
                *p = z;
            }
        }
        if (buffer <= 1 && color_buffer(buffer) != ~0u)
            renderer->lfb_write((int)color_buffer(buffer), x, scry, argb, rgb, depth, z, mode);
    }

    static uint32_t expand(uint32_t v, int bits) { return bits == 5 ? (v << 3 | v >> 2) : bits == 6 ? (v << 2 | v >> 4) : v; }

    void lfb_write(uint32_t off, uint32_t data, int size) {
        uint32_t mode = fbi[vLfbMode];
        uint32_t mask = size == 4 ? 0xFFFFFFFFu : (off & 2) ? 0xFFFF0000u : 0x0000FFFFu;
        if (size == 2 && (off & 2)) data <<= 16;
        if (size == 1) {
            static int n;
            if (n++ < 4) log("voodoo: 8-bit LFB write ignored");
            return;
        }
        if (mode & (1 << 12)) {
            data = __builtin_bswap32(data);
            mask = __builtin_bswap32(mask);
        }
        if (mode & (1 << 11)) {
            data = (data << 16) | (data >> 16);
            mask = (mask << 16) | (mask >> 16);
        }
        int format = mode & 15;
        int lanes = (mode >> 9) & 3;
        int buffer = (mode >> 4) & 3;
        uint32_t za = fbi[vZaColor];
        uint32_t alpha = za >> 24;
        uint16_t zdef = za & 0xFFFF;
        uint32_t word = (off & 0x3FFFFF) >> 2;
        bool bgr = lanes == 1 || lanes == 3;
        if (lanes >= 2) {
            static int n;
            if (n++ < 4) log("voodoo: LFB RGBA/BGRA lane order not handled (format %d)", format);
        }
        auto c565 = [&](uint32_t v) {
            uint32_t r = expand((v >> 11) & 31, 5), g = expand((v >> 5) & 63, 6), b = expand(v & 31, 5);
            if (bgr) std::swap(r, b);
            return (alpha << 24) | (r << 16) | (g << 8) | b;
        };
        auto c555 = [&](uint32_t v, bool a1) {
            uint32_t r = expand((v >> 10) & 31, 5), g = expand((v >> 5) & 31, 5), b = expand(v & 31, 5);
            if (bgr) std::swap(r, b);
            uint32_t a = a1 ? ((v & 0x8000) ? 0xFF : 0) : alpha;
            return (a << 24) | (r << 16) | (g << 8) | b;
        };
        // 16-bit formats carry two pixels per word.
        bool two = format <= 2 || format == 15;
        uint32_t pix = two ? word * 2 : word;
        int x = pix & 1023, y = (pix >> 10) & 1023;
        bool p0 = (mask & 0xFFFF) != 0, p1 = (mask & 0xFFFF0000u) != 0;
        switch (format) {
        case 0:
            if (p0) lfb_store(buffer, x, y, c565(data & 0xFFFF), true, false, false, zdef);
            if (p1) lfb_store(buffer, x + 1, y, c565(data >> 16), true, false, false, zdef);
            break;
        case 1:
        case 2:
            if (p0) lfb_store(buffer, x, y, c555(data & 0xFFFF, format == 2), true, format == 2, false, zdef);
            if (p1) lfb_store(buffer, x + 1, y, c555(data >> 16, format == 2), true, format == 2, false, zdef);
            break;
        case 4:
        case 5: {
            uint32_t r = (data >> 16) & 0xFF, g = (data >> 8) & 0xFF, b = data & 0xFF, a = format == 5 ? data >> 24 : alpha;
            if (bgr) std::swap(r, b);
            lfb_store(buffer, x, y, (a << 24) | (r << 16) | (g << 8) | b, true, format == 5, false, zdef);
            break;
        }
        case 12:
            lfb_store(buffer, x, y, c565(data & 0xFFFF), true, false, true, (uint16_t)(data >> 16));
            break;
        case 13:
        case 14:
            lfb_store(buffer, x, y, c555(data & 0xFFFF, format == 14), true, format == 14, true, (uint16_t)(data >> 16));
            break;
        case 15:
            if (p0) lfb_store(buffer, x, y, 0, false, false, true, (uint16_t)data);
            if (p1) lfb_store(buffer, x + 1, y, 0, false, false, true, (uint16_t)(data >> 16));
            break;
        default: {
            static int n;
            if (n++ < 8) log("voodoo: LFB write format %d not handled", format);
        }
        }
    }

    uint32_t lfb_read(uint32_t off) {
        uint32_t mode = fbi[vLfbMode];
        int buffer = (mode >> 6) & 3;
        uint32_t bufoff = buffer == 0 ? rgboffs[frontbuf] : buffer == 1 ? rgboffs[backbuf] : buffer == 2 ? auxoffs : ~0u;
        if (bufoff == ~0u) return 0xFFFFFFFF;
        renderer->sync_to_memory(this_fb());
        uint32_t pix = ((off & 0x3FFFFF) >> 2) * 2;
        int x = pix & 1023, y = (pix >> 10) & 1023;
        if (mode & (1 << 13)) y = yorigin() - y;
        uint16_t *p = pixel_ptr(bufoff, x, y);
        if (!p) return 0xFFFFFFFF;
        uint32_t v = p[0] | ((uint32_t)p[1] << 16);
        if (mode & (1 << 15)) v = (v << 16) | (v >> 16);
        if (mode & (1 << 16)) v = __builtin_bswap32(v);
        return v;
    }

    // ---- texture memory ----

    void texture_write(uint32_t off, uint32_t data) {
        uint32_t texmode = tmu[vTextureMode], tlod = tmu[vTLOD];
        if (tlod & (1u << 25)) data = __builtin_bswap32(data);    // tdata_swizzle
        if (tlod & (1u << 26)) data = (data >> 16) | (data << 16); // tdata_swap
        uint32_t format = (texmode >> 8) & 15;
        uint32_t bpt = format < 8 ? 1 : 2;
        uint32_t index = off >> 2;
        if ((index >> 19) & 3) return; // only TMU 0 is fitted: writes for the others go nowhere (as MAME)
        uint32_t lod = (index >> 15) & 15, t = (index >> 7) & 0xFF;
        bool seq8 = texmode & (1u << 31);
        uint32_t s = (index << ((seq8 && bpt == 1) ? 2 : 1)) & 0xFF;
        if (lod > 8) return;
        // Address of (lod, s, t) for the texture the registers currently describe.
        uint32_t lodoff[9];
        texture_lod_offsets(lodoff);
        uint32_t wmask, hmask;
        texture_masks(wmask, hmask);
        uint32_t o = t * ((wmask >> lod) + 1) + s;
        uint32_t addr = (lodoff[lod] + ((bpt * o) & ~3u)) & 0x3FFFFF;
        memcpy(&tmu_mem[addr], &data, 4);
        renderer->texture_written(addr, 4);
    }

public:
    void texture_masks(uint32_t &wmask, uint32_t &hmask) {
        uint32_t tlod = tmu[vTLOD];
        uint32_t aspect = (tlod >> 21) & 3;
        wmask = hmask = 0xFF;
        if (tlod & (1u << 20)) hmask >>= aspect; // S is wider
        else wmask >>= aspect;
    }

    void texture_lod_offsets(uint32_t lodoff[9]) {
        uint32_t tlod = tmu[vTLOD];
        uint32_t lodmask = 0x1FF;
        if (tlod & (1u << 19)) lodmask = (tlod & (1u << 18)) ? 0x0AA : 0x155; // tsplit (19), odd (18)
        uint32_t wmask, hmask;
        texture_masks(wmask, hmask);
        uint32_t bppscale = ((tmu[vTextureMode] >> 8) & 15) >> 3;
        uint32_t base = tmu[vTexBaseAddr] & 0x7FFFF;
        uint32_t offset = 0;
        for (int lod = 0; lod <= 8; lod++) {
            lodoff[lod] = ((base << 3) + offset) & 0x3FFFFF;
            if (lodmask & (1u << lod)) {
                uint32_t size = ((wmask >> lod) + 1) * ((hmask >> lod) + 1);
                if (lod >= 3 && size < 4) size = 4;
                offset += size << bppscale;
            }
        }
    }

    // ---- bus interface ----

    uint32_t read(uint32_t off, int size) override {
        if (off < 0x400000) {
            uint32_t v = reg_read((off >> 2) & 0xFF);
            if (size != 4) v >>= (off & 3) * 8;
            return v;
        }
        if (off < 0x800000) {
            uint32_t v = lfb_read((off - 0x400000) & ~3u);
            if (size == 2) v >>= (off & 2) * 8;
            return v;
        }
        return 0xFFFFFFFF;
    }

    void write(uint32_t off, uint32_t v, int size) override {
        if (off < 0x400000 && ((off >> 2) & 0xFF) == vSwapbufferCMD) swaps_pending++;
        if (retrace_wait || !fifo.empty()) {
            fifo.push_back({off, v, size});
            return;
        }
        execute(off, v, size);
    }

    void execute(uint32_t off, uint32_t v, int size) {
        if (off < 0x400000) {
            unsigned chipmask = (off >> 10) & 15;
            if (!chipmask) chipmask = 15;
            int reg = (off >> 2) & 0xFF;
            if ((off & (1u << 21)) && (fbi[vFbiInit3] & 1) && reg < 0x40) reg = ALIAS[reg];
            reg_write(reg, v, chipmask);
            return;
        }
        if (off < 0x800000) return lfb_write(off - 0x400000, v, size);
        texture_write(off - 0x800000, v);
    }
};

Voodoo *voodoo;

// PCI configuration register 0x40 is initEnable.
struct VoodooPci : PciDevice {
    VoodooPci() : PciDevice(8, 0x0001121A, 0x00000002) { bar_size[0] = 0x01000000; }
    void config_written(int reg, uint32_t v) override {
        if (reg == 0x40 / 4) voodoo->init_enable = v;
    }
};

VoodooPci *voodoo_pci;

} // namespace

namespace rt {

void voodoo_init(Renderer *r) {
    voodoo = new Voodoo(r);
    add_device(voodoo);
    voodoo_pci = new VoodooPci();
    pci_add(voodoo_pci);
}

bool voodoo_vblank_active() { return voodoo->in_vblank; }

} // namespace rt
