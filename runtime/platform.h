// Host platform (platform.cpp): window, OpenGL context, audio output, controllers and keyboard.
#pragma once
#include <cstdint>

namespace rt {

bool platform_init(bool headless);
// Called once per emulated vertical blank: handles window events, queues audio.
void platform_frame();
// Shows a 16-bit 5-6-5 image (the software renderer's front buffer).
void platform_present_565(const uint16_t *pixels, int width, int height, int stride);
bool platform_quit_requested();
void platform_window_size(int &w, int &h); // drawable size in pixels
void platform_swap();
// Widescreen (F9, in settings.ini): the game draws a 4/3 wider view, shown at 16:9 (see gpurender).
bool platform_widescreen();
float platform_display_aspect(); // 4:3 or 16:9

// Current input state, in the cabinet's terms.
struct Controls {
    float steering = 0;      // -1 (left) .. 1 (right)
    float accel = 0, brake = 0;
    bool start = false, coin = false, test = false, service = false;
    bool view1 = false, view2 = false, view3 = false, radio = false;
    int gear = 0;            // 1-4 when a gear button is held, 0 otherwise
};
const Controls &platform_controls();

} // namespace rt
