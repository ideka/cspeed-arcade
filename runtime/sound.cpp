// The I/O ASIC's sound port: connects the main CPU to the DCS2 board (as MAME's midway_ioasic with a
// DCS, auto-acknowledging reads as California Speed's board does).
#include "board.h"
#include "dcs.h"

namespace rt {

uint16_t sound_status() {
    uint16_t r = ((dcs_control() >> 4) ^ 0x40) & 0x00C0;
    r |= dcs_fifo_status() & 0x0038;
    r |= dcs_output_control() & 0xFF00;
    return r;
}

uint16_t sound_data_read() {
    uint16_t v = dcs_data_read();
    dcs_ack(); // auto-ack
    return v;
}

void sound_data_write(uint16_t v) { dcs_data_write(v); }
void sound_ack() { dcs_ack(); }

void sound_control(uint16_t, uint16_t v) {
    dcs_reset_line(v & 1);
    if (!(v & 4)) dcs_fifo_reset();
}

} // namespace rt
