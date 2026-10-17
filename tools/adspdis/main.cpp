// adspdis <pm dump> [start] [count]: disassembles an ADSP-21xx program memory dump (24-bit words as u32).
#include "emu.h"
#include "2100dasm.h"
#include <cstdlib>
#include <sstream>
#include <vector>
int main(int argc, char **argv) {
    FILE *f = fopen(argv[1], "rb");
    std::vector<u32> pm(0x4000);
    fread(pm.data(), 4, 0x4000, f);
    u32 start = argc > 2 ? strtoul(argv[2], 0, 16) : 0, count = argc > 3 ? strtoul(argv[3], 0, 16) : 0x4000;
    adsp21xx_disassembler d;
    util::disasm_interface::data_buffer b{pm.data()};
    for (u32 pc = start; pc < start + count && pc < 0x4000; pc++) {
        std::ostringstream s;
        d.disassemble(s, pc, b, b);
        printf("%04x: %06x  %s\n", pc, pm[pc], s.str().c_str());
    }
}
