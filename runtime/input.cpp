// Player inputs and the widget board's outputs. Placeholder input until the window/controller layer
// exists: nothing pressed, wheel centred.
//
// The wheel board: at start-up the game drives the force-feedback motor left and right and waits for
// the steering potentiometer to follow. As in MAME (seattle.cpp), the reading drifts while force is
// applied until calibration is over.
#include "board.h"

#include "platform.h"

#include <algorithm>
#include <cmath>

namespace rt {

static int wheel_force, wheel_offset;
static bool wheel_calibrated;
static uint32_t output_last, output_mode;

// DIP switch 1 on (0) skips the force-feedback wheel calibration at start-up, which needs a real
// motorised wheel.
uint16_t input_dips() { return 0xFFFE; }
// SYSTEM (active low): coin 1 0x01, start 0x04, test 0x10, service 0x40.
uint16_t input_system() {
    const Controls &c = platform_controls();
    uint16_t v = 0xFFFF;
    if (c.coin) v &= ~0x01;
    if (c.start) v &= ~0x04;
    if (c.test) v &= ~0x10;
    if (c.service) v &= ~0x40;
    return v;
}

// IN1 (active low): radio 0x02, views 0x10/0x20/0x40; bits 12-15 the gear last selected (one-hot).
static uint16_t gear = 1;
uint16_t input_in1() {
    const Controls &c = platform_controls();
    uint16_t v = 0x0FFF;
    if (c.radio) v &= ~0x02;
    if (c.view1) v &= ~0x10;
    if (c.view2) v &= ~0x20;
    if (c.view3) v &= ~0x40;
    if (c.gear) gear = (uint16_t)(1 << (c.gear - 1));
    return v | (uint16_t)(gear << 12);
}

// ADC channels: 0 steering (0x10-0xF0, centre 0x80), 1 accelerator, 2 brake (0x00 released - 0xFF).
static uint8_t raw_analog(int channel) {
    const Controls &c = platform_controls();
    switch (channel) {
    case 0: return (uint8_t)std::lround(0x80 + c.steering * 0x70);
    case 1: return (uint8_t)std::lround(c.accel * 255);
    case 2: return (uint8_t)std::lround(c.brake * 255);
    }
    return 0x80;
}

uint8_t analog_input(int channel) {
    int v = raw_analog(channel);
    if (channel == 0 && !wheel_calibrated && (wheel_force > 20 || wheel_force < -20)) {
        // The reading moves against the motor's force value (the calibration's first push, force
        // -24, must end with a higher reading than the second one).
        if (wheel_force < 0 && wheel_offset < 128) wheel_offset++;
        else if (wheel_force > 0 && wheel_offset > -128) wheel_offset--;
        v = std::clamp(v + wheel_offset, 0x10, 0xF0);
    }
    if (!wheel_calibrated && (~input_system() & 0xFFFF)) wheel_calibrated = true;
    return (uint8_t)v;
}

void widget_output(uint32_t data) {
    uint32_t arg = data & 0xFF;
    if (!(output_last & (1 << 11)) && (data & (1 << 11))) output_mode = arg;
    output_last = data;
    if (!(data & (1 << 10)) && output_mode == 4) wheel_force = (int8_t)~arg; // wheel motor
}

// Enhancement: no time limit on the selection screens (track, mode, car, transmission: game states
// 4-10 at 0x80701D80). Their countdown (frames, at 0x80701D84) is held at 5 seconds left; each
// screen subtracts the elapsed frames itself and picks for the player below 3.
// ARC_SELECT_TIMER=1 keeps the original countdown.
void hold_select_timer() {
    static bool original = getenv("ARC_SELECT_TIMER") != nullptr;
    if (original) return;
    uint32_t state = rd32(0x80701D80u);
    if (state < 4 || state > 10) return;
    const int32_t hold = 5 * 57; // below the times the screens start taking input (track 840, car 570, transmission 390)
    int32_t timer = (int32_t)rd32(0x80701D84u);
    if (timer > 0 && timer < hold) wr32(0x80701D84u, (uint32_t)hold);
}

void vblank() {
    hold_select_timer();
    platform_frame();
}

} // namespace rt
