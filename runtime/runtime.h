// Runtime internals shared by the native side (C++).
#pragma once
#include "recomp.h"

#include <cstdint>
#include <vector>

enum {
    COP0_INDEX = 0, COP0_COUNT = 9, COP0_COMPARE = 11, COP0_STATUS = 12, COP0_CAUSE = 13, COP0_EPC = 14,
    COP0_PRID = 15, COP0_CONFIG = 16,
};

namespace rt {

// A block of hardware registers at a physical address.
struct Device {
    const char *name;
    uint32_t base, size;
    Device(const char *n, uint32_t b, uint32_t s) : name(n), base(b), size(s) {}
    virtual ~Device() = default;
    virtual uint32_t read(uint32_t off, int size) = 0;
    virtual void write(uint32_t off, uint32_t v, int size) = 0;
    virtual void update(uint64_t) {} // called from rt_service with the current time
};

void add_device(Device *d);
void update_devices();
// Guest physical memory access for DMA (RAM or devices).
uint32_t phys_read32(uint32_t phys);
void phys_write32(uint32_t phys, uint32_t v);
void set_boot_rom(std::vector<uint8_t> data);

void log(const char *fmt, ...);
[[noreturn]] void fatal(const char *fmt, ...);
uint64_t now_ns();
const char *func_name(uint32_t addr);
void print_native_stack();
void dump_trace(int n);
void call(Ctx *c, uint32_t addr);

// Interrupts (interrupts.cpp). Lines are the CPU's IP2..IP7 inputs (0..5).
void set_irq(int line, bool on);
uint32_t pending_ip();           // Cause.IP bits for the external and timer lines
void compare_written(Ctx *c);
void deliver_interrupts(Ctx *c);

extern bool trace_mmio;

} // namespace rt
