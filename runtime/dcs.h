// DCS2 sound board (dcs.cpp): the I/O ASIC's view of it.
#pragma once
#include "runtime.h"

#include <vector>

namespace rt {

void dcs_init(std::vector<uint8_t> boot_rom);
void dcs_update(uint64_t now);

uint16_t dcs_control();          // latch control bits: 0x400 output empty, 0x800 input empty
uint16_t dcs_output_control();   // the DSP's three extra status lines (bits 8-15 on the host side)
bool dcs_output_full();
bool dcs_input_empty();
void dcs_data_write(uint16_t d); // host to DSP
uint16_t dcs_data_read();        // DSP to host
void dcs_ack();                  // host acknowledges the DSP's data
void dcs_reset_line(bool high);
void dcs_fifo_write(uint16_t d);
void dcs_fifo_reset();
uint16_t dcs_fifo_status();
// Takes up to max_frames stereo frames (int16 pairs) of produced audio; returns the frames taken.
size_t dcs_take_audio(int16_t *out, size_t max_frames);

// Called by the board when the latches change (I/O ASIC sound interrupt bits).
void dcs_irq_changed();

} // namespace rt
