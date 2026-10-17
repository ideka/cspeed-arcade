// Serial security PIC (game ID and serial number). Placeholder.
#include "board.h"

namespace rt {

uint32_t pic_read() { return 0; }
void pic_write(uint8_t v) { log("PIC write %02x", v); }

} // namespace rt
