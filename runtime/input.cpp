// Player inputs and the widget board's outputs. Placeholder input until the window/controller layer
// exists: nothing pressed, wheel centred.
//
// The wheel board: at start-up the game drives the force-feedback motor left and right and waits for
// the steering potentiometer to follow. As in MAME (seattle.cpp), the reading drifts while force is
// applied until calibration is over.
#include "board.h"

#include <algorithm>

namespace rt {

static int wheel_force, wheel_offset;
static bool wheel_calibrated;
static uint32_t output_last, output_mode;

// DIP switch 1 on (0) skips the force-feedback wheel calibration at start-up, which needs a real
// motorised wheel.
uint16_t input_dips() { return 0xFFFE; }
uint16_t input_system() { return 0xFFFF; }
uint16_t input_in1() { return 0x0FFF; } // gearshift bits (0xF000) active high: none engaged

static uint8_t raw_analog(int channel) { return channel == 0 ? 0x80 : 0x00; }

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

void vblank() {}

} // namespace rt
