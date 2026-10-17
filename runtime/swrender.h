// Software Voodoo rasterizer (swrender.cpp) and a Renderer that uses it.
#pragma once
#include "voodoo.h"

#include <string>

namespace rt {

extern volatile int sw_counts[4];
extern volatile uint64_t sw_render_ns;
void sw_triangle(const VoodooTriangle &t, const VoodooState &st);
void sw_fastfill(const VoodooState &st);
void sw_pixel(const VoodooState &st, int buffer, int x, int y, uint32_t argb, uint16_t z);

// Draws everything into the frame buffer RAM; can dump displayed frames as PNG files.
struct SoftwareRenderer : Renderer {
    std::string dump_dir;   // empty: no dumps
    int dump_every = 60;    // vblanks (ARC_DUMP_EVERY)
    int dump_from = 0;      // first vblank dumped (ARC_DUMP_FROM)
    int frame = 0;
    void vblank(const VoodooState &st) override;
    void triangle(const VoodooTriangle &t, const VoodooState &st) override {
        uint64_t t0 = now_ns();
        sw_triangle(t, st);
        sw_counts[1]++;
        sw_render_ns += now_ns() - t0;
    }
    void fastfill(const VoodooState &st) override {
        uint64_t t0 = now_ns();
        sw_fastfill(st);
        sw_counts[3]++;
        sw_render_ns += now_ns() - t0;
    }
    void swap(const VoodooState &st) override;
    void lfb_pipeline_write(const VoodooState &st, int buffer, int x, int y, uint32_t argb, uint16_t z) override { sw_pixel(st, buffer, x, y, argb, z); }
};

void write_png(const std::string &path, int w, int h, const uint8_t *rgb);

} // namespace rt
