// 3dfx Voodoo Graphics (SST-1) with one TMU, emulated at the register level (voodoo.cpp). Drawing is
// handed to a Renderer, which reproduces it on the host GPU.
#pragma once
#include "runtime.h"

namespace rt {

// FBI register indices (byte offset / 4).
enum VoodooReg {
    vStatus = 0x000 / 4, vIntrCtrl = 0x004 / 4, vVertexAx = 0x008 / 4, vVertexAy, vVertexBx, vVertexBy, vVertexCx,
    vVertexCy, vStartR = 0x020 / 4, vStartG, vStartB, vStartZ, vStartA, vStartS, vStartT, vStartW,
    vDRdX = 0x040 / 4, vDGdX, vDBdX, vDZdX, vDAdX, vDSdX, vDTdX, vDWdX,
    vDRdY = 0x060 / 4, vDGdY, vDBdY, vDZdY, vDAdY, vDSdY, vDTdY, vDWdY,
    vTriangleCMD = 0x080 / 4, vFtriangleCMD = 0x100 / 4, vFbzColorPath = 0x104 / 4, vFogMode = 0x108 / 4,
    vAlphaMode = 0x10C / 4, vFbzMode = 0x110 / 4, vLfbMode = 0x114 / 4, vClipLeftRight = 0x118 / 4,
    vClipLowYHighY = 0x11C / 4, vNopCMD = 0x120 / 4, vFastfillCMD = 0x124 / 4, vSwapbufferCMD = 0x128 / 4,
    vFogColor = 0x12C / 4, vZaColor = 0x130 / 4, vChromaKey = 0x134 / 4, vStipple = 0x140 / 4,
    vColor0 = 0x144 / 4, vColor1 = 0x148 / 4, vFogTable = 0x160 / 4, vFbiInit4 = 0x200 / 4,
    vVRetrace = 0x204 / 4, vBackPorch = 0x208 / 4, vVideoDimensions = 0x20C / 4, vFbiInit0 = 0x210 / 4,
    vFbiInit1 = 0x214 / 4, vFbiInit2 = 0x218 / 4, vFbiInit3 = 0x21C / 4, vHSync = 0x220 / 4, vVSync = 0x224 / 4,
    vClutData = 0x228 / 4, vDacData = 0x22C / 4,
    // TMU registers
    vTextureMode = 0x300 / 4, vTLOD = 0x304 / 4, vTDetail = 0x308 / 4, vTexBaseAddr = 0x30C / 4,
    vTexBaseAddr1 = 0x310 / 4, vTexBaseAddr2 = 0x314 / 4, vTexBaseAddr38 = 0x318 / 4, vTrexInit0 = 0x31C / 4,
    vTrexInit1 = 0x320 / 4, vNccTable0 = 0x324 / 4, vNccTable1 = 0x354 / 4,
};

// Triangle setup in the hardware's fixed-point formats (as MAME keeps them): vertices 12.4, colours
// 12.12, Z 20.12, W/S/T with 32 fraction bits. The parameters are the values at vertex A's pixel.
struct VoodooTriangle {
    int32_t ax, ay, bx, by, cx, cy;
    int32_t startr, startg, startb, starta, startz;
    int32_t drdx, dgdx, dbdx, dadx, dzdx;
    int32_t drdy, dgdy, dbdy, dady, dzdy;
    int64_t startw, dwdx, dwdy;                 // FBI W
    int64_t starts, startt, startw0;            // TMU 0
    int64_t dsdx, dtdx, dw0dx, dsdy, dtdy, dw0dy;
};

// The frame buffer RAM as the CPU sees it (byte offsets of each buffer; ~0u if absent).
struct FrameBufferMemory {
    uint8_t *ram;
    uint32_t front, back, aux, rowpixels;
};

// Everything a renderer needs to draw: register files, texture memory and palette, frame buffer.
struct VoodooState {
    const uint32_t *fbi, *tmu;
    const uint8_t *tmu_mem;
    const uint32_t *palette;   // 256 entries, 0xFFrrggbb (palette written through the NCC registers)
    FrameBufferMemory fb;
    int yorigin;
    uint64_t generation; // changes whenever a register, the palette or the buffers change
};

struct Renderer {
    virtual ~Renderer() = default;
    virtual void triangle(const VoodooTriangle &, const VoodooState &) {}
    virtual void fastfill(const VoodooState &) {}
    virtual void swap(const VoodooState &) {} // st.fb.front is now the displayed buffer
    virtual void lfb_write(int buffer, int x, int y, uint32_t argb, bool rgb, bool depth, uint16_t z, uint32_t lfb_mode) {}
    // An LFB write with lfbMode's pixel pipeline enabled: run it through the pipeline at (x, y) into
    // buffer (0 front, 1 back, as lfbMode selects).
    virtual void lfb_pipeline_write(const VoodooState &, int buffer, int x, int y, uint32_t argb, uint16_t z) {}
    // Before an LFB read: copy what was drawn on the GPU into the frame buffer RAM.
    virtual void sync_to_memory(const FrameBufferMemory &) {}
    virtual void texture_written(uint32_t offset, uint32_t bytes) {}
    virtual void clut_changed(const uint32_t *clut) {}
    virtual void vblank(const VoodooState &) {} // start of each vertical blank
};

void voodoo_init(Renderer *r);
bool voodoo_vblank_active();

} // namespace rt
