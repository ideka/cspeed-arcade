// Hardware of the Midway Seattle board (California Speed). Physical addresses:
//   0x08000000  3dfx Voodoo (SST-1) on PCI
//   0x0A000000  PCI I/O space: IDE controller (ide.cpp)
//   0x0C000000  Galileo GT64010 system controller (galileo.cpp)
//   0x16000000+ board registers on chip select 3 (board.cpp)
#include "board.h"
#include "dcs.h"
#include "galileo.h"
#include "gpurender.h"
#include "swrender.h"

#include <algorithm>
#include <cstring>

#include <cstdlib>

namespace {

// Placeholder: remembers writes, reads back what was written, logs the first accesses.
struct Stub : rt::Device {
    std::vector<uint32_t> regs;
    int logged = 0;
    Stub(const char *n, uint32_t b, uint32_t s) : Device(n, b, s), regs(s / 4) {}
    uint32_t read(uint32_t off, int) override {
        uint32_t v = regs[off / 4];
        if (logged++ < 64) rt::log("%s read  +%05x -> %08x", name, off, v);
        return v;
    }
    void write(uint32_t off, uint32_t v, int) override {
        if (logged++ < 64) rt::log("%s write +%05x <- %08x", name, off, v);
        regs[off / 4] = v;
    }
};

// The I/O ASIC's sound FIFO (chip select 1): stream data for the sound board.
struct SoundFifo : rt::Device {
    SoundFifo() : Device("soundfifo", 0x13000000, 0x10) {}
    uint32_t read(uint32_t, int) override { return 0; }
    void write(uint32_t, uint32_t v, int) override { rt::dcs_fifo_write((uint16_t)v); }
};

} // namespace

namespace rt { bool ide_init(const std::string &chd, const std::string &overlay); }

std::vector<uint8_t> read_data_file(const std::string &path);

void devices_init(const char *data_dir) {
    rt::dcs_init(read_data_file(std::string(data_dir) + "/sound102.u95"));
    rt::galileo_init();
    rt::board_init(std::string(data_dir) + "/cmos.bin");
    rt::pci_add(new rt::PciDevice(9, 0x0002100B, 0x01018F01)); // NS 87415 IDE
    rt::add_device(new SoundFifo());
    // ARC_RENDERER=sw: the software rasterizer (also used headless); otherwise the GPU, at ARC_SCALE.
    static rt::SoftwareRenderer sw;
    if (const char *d = getenv("ARC_DUMP_FRAMES")) sw.dump_dir = d;
    if (const char *d = getenv("ARC_DUMP_EVERY")) sw.dump_every = std::max(1, atoi(d));
    if (const char *d = getenv("ARC_DUMP_FROM")) sw.dump_from = atoi(d);
    const char *r = getenv("ARC_RENDERER");
    bool software = getenv("ARC_HEADLESS") || (r && !strcmp(r, "sw"));
    int scale = getenv("ARC_SCALE") ? atoi(getenv("ARC_SCALE")) : 3;
    rt::voodoo_init(software ? &sw : rt::gpu_renderer_create(std::clamp(scale, 1, 8), &sw));
    if (!rt::ide_init(std::string(data_dir) + "/calspeed.chd", std::string(data_dir) + "/hdd_writes.bin"))
        rt::fatal("the hard disk image (calspeed.chd) is required");
}
