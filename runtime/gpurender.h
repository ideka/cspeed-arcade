// GPU renderer for the emulated Voodoo (gpurender.cpp): draws on the host GPU at any resolution.
#pragma once
#include "voodoo.h"

namespace rt {

// `software` draws until the game has set up its video mode (see gpurender.cpp).
Renderer *gpu_renderer_create(int scale, Renderer *software);

} // namespace rt
