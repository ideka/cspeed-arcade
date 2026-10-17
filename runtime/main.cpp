// Entry point: loads the game executable into guest RAM and runs it from where its boot code hands
// over to C (BootUp).
#include "runtime.h"
#include "swrender.h"
#include "platform.h"
#include "../generated/funcs.h"

#include <cstdio>
#include <cstdlib>
#include <fstream>
#include <string>
#include <vector>

namespace rt { void watchdog_start(); extern Ctx *main_ctx; }

std::vector<uint8_t> read_data_file(const std::string &path) {
    std::ifstream f(path, std::ios::binary);
    if (!f) rt::fatal("cannot open %s", path.c_str());
    return std::vector<uint8_t>((std::istreambuf_iterator<char>(f)), std::istreambuf_iterator<char>());
}

// Copies the ELF's loadable segment into RAM and clears the rest of it (the .bss).
static void load_elf(const std::vector<uint8_t> &elf) {
    auto u32 = [&](size_t o) { uint32_t v; memcpy(&v, elf.data() + o, 4); return v; };
    uint32_t phoff = u32(0x1C);
    uint16_t phnum;
    memcpy(&phnum, elf.data() + 0x2C, 2);
    for (int i = 0; i < phnum; i++) {
        size_t ph = phoff + i * 32;
        if (u32(ph) != 1) continue;
        uint32_t off = u32(ph + 4), vaddr = u32(ph + 8), filesz = u32(ph + 16), memsz = u32(ph + 20);
        uint32_t ram = vaddr - 0x80000000u;
        if (ram + memsz > RAM_SIZE) rt::fatal("segment at %08x does not fit in RAM", vaddr);
        memcpy(rt_ram + ram, elf.data() + off, filesz);
        memset(rt_ram + ram + filesz, 0, memsz - filesz);
    }
}

void devices_init(const char *data_dir);

#include <SDL.h>

int main(int argc, char **argv) {
    const char *data_dir = argc > 1 ? argv[1] : "data";
    std::string dir = data_dir;
    rt_ram = (uint8_t *)calloc(1, RAM_SIZE);
    load_elf(read_data_file(dir + "/vmunix"));
    rt::set_boot_rom(read_data_file(dir + "/caspd1_2.u32"));
    if (!rt::platform_init(getenv("ARC_HEADLESS") != nullptr)) rt::fatal("cannot open the window");
    devices_init(data_dir);

    static Ctx c{};
    c.cop0[COP0_STATUS] = 0xF4010080; // as boot_entry leaves it: COP0-3 usable, FR, BEV off, IE off
    // R5000: 32 KB instruction and data caches with 32-byte lines, kseg0 cacheable.
    c.cop0[COP0_COMPARE] = 0xFFFFFFFF;
    c.cop0[COP0_CONFIG] = (3 << 9) | (3 << 6) | (1 << 5) | (1 << 4) | 3;
    // What boot_entry records after probing the caches (config_cache) and the CPU.
    const uint32_t icache = 0x8000, dcache = 0x8000, iline = 32, dline = 32;
    uint32_t bootinfo[6] = {0x11F8, 0x2320, icache, dcache, iline, dline};
    for (int i = 0; i < 6; i++) wr32(0x800FC890 + 4 * i, bootinfo[i]);
    wr32(0x80485950, icache);
    wr32(0x80485958, dcache);
    wr32(0x80485954, iline);
    wr32(0x8048595C, dline);
    c.fcr31 = 0x01000E00;
    c.r[29] = S32(0x807FFF00);
    c.budget = 2000;
    rt::main_ctx = &c;
    if (getenv("ARC_WATCH")) rt::watchdog_start();
    rt::log("starting at BootUp");
    f_8007729c(&c);
    rt::log("BootUp returned");
    return 0;
}
