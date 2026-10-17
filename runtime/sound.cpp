// DCS2 sound board. Placeholder: reports an idle board with empty buffers.
#include "board.h"

namespace rt {

uint16_t sound_status() { return 0x0048; }
uint16_t sound_data_read() { return 0; }
void sound_data_write(uint16_t) {}
void sound_ack() {}
void sound_control(uint16_t, uint16_t) {}

} // namespace rt
