// Galileo GT64010 system controller and the PCI devices behind it.
#pragma once
#include "runtime.h"

extern "C" uint32_t rt_rd_slow(uint32_t a, int size);
extern "C" void rt_wr_slow(uint32_t a, uint32_t v, int size);

namespace rt {

// A device's PCI configuration space (bus 0, function 0).
struct PciDevice {
    int slot;
    uint32_t id, class_rev;
    uint32_t bar_size[6] = {};
    uint32_t config[64] = {};
    PciDevice(int s, uint32_t i, uint32_t c) : slot(s), id(i), class_rev(c) {}
    virtual ~PciDevice() = default;
    uint32_t config_read(int reg);
    void config_write(int reg, uint32_t v);
    virtual void bar_written(int, uint32_t) {}
    virtual void config_written(int, uint32_t) {}
};

void galileo_init();
void pci_add(PciDevice *d);

} // namespace rt
