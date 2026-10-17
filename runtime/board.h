// Seattle board: I/O ASIC, CMOS, widget board and interrupt control (board.cpp), and the pieces it
// connects to (inputs, sound board, security PIC).
#pragma once
#include "runtime.h"

#include <string>

namespace rt {

void board_init(const std::string &cmos_path);
void board_shutdown();

// Inputs (input.cpp). Ports are active low.
uint16_t input_dips();   // DIP switches (1 = off)
uint16_t input_system();
uint16_t input_in1();
uint8_t analog_input(int channel); // 0 steering, 1 accelerator, 2 brake
void widget_output(uint32_t v);    // wheel motor, lamps
void vblank();
void board_vblank(bool state); // from the Voodoo's video timing

// DCS2 sound board as seen through the I/O ASIC (sound.cpp).
uint16_t sound_status();
uint16_t sound_data_read();
void sound_data_write(uint16_t v);
void sound_ack();
void sound_control(uint16_t old, uint16_t v);

// Serial security PIC (pic.cpp).
uint32_t pic_read();
void pic_write(uint8_t v);

} // namespace rt
