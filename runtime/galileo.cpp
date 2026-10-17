// Galileo GT64010 system controller: timers, DMA, interrupt cause/mask and PCI configuration space.
// Register behaviour follows MAME's gt64xxx.cpp. Its interrupt output is the CPU's IP2 (line 0).
#include "galileo.h"

#include <cstring>

namespace {

enum {
    DMA0_COUNT = 0x800 / 4, DMA0_SOURCE = 0x810 / 4, DMA0_DEST = 0x820 / 4, DMA0_NEXT = 0x830 / 4,
    DMA0_CONTROL = 0x840 / 4, TIMER0_COUNT = 0x850 / 4, TIMER_CONTROL = 0x864 / 4, PCI_COMMAND = 0xC00 / 4,
    INTR_CAUSE = 0xC18 / 4, CPU_MASK = 0xC1C / 4, CONFIG_ADDRESS = 0xCF8 / 4, CONFIG_DATA = 0xCFC / 4,
};
const uint64_t CLOCK = 50000000;
const int IRQ_LINE = 0;

struct Timer {
    bool active = false;
    uint32_t count = 0;   // counts left when started
    uint64_t start_ns = 0;
    uint64_t fire_ns = 0;
};

struct Galileo : rt::Device {
    uint32_t reg[0x1000 / 4] = {};
    Timer timers[4];
    std::vector<rt::PciDevice *> pci;

    Galileo() : Device("galileo", 0x0C000000, 0x1000) {}

    static uint64_t ticks_ns(uint64_t ticks) { return ticks * 1000000000ull / CLOCK; }

    void update_irq() {
        uint32_t cause = reg[INTR_CAUSE] & ~1u & ~(1u << 30);
        if (cause) cause |= 1;
        if (cause & reg[CPU_MASK] & 0x3C1FFFFE) cause |= 1u << 30;
        reg[INTR_CAUSE] = cause;
        rt::set_irq(IRQ_LINE, (cause & reg[CPU_MASK]) != 0);
    }

    uint32_t timer_value(int which, uint64_t now) {
        Timer &t = timers[which];
        if (!t.active) return t.count;
        uint64_t elapsed = (now - t.start_ns) * CLOCK / 1000000000ull;
        return t.count > elapsed ? (uint32_t)(t.count - elapsed) : 0;
    }

    void start_timer(int which, uint64_t now) {
        Timer &t = timers[which];
        t.active = true;
        if (t.count == 0) t.count = reg[TIMER0_COUNT + which] & (which ? 0xFFFFFF : 0xFFFFFFFF);
        t.start_ns = now;
        t.fire_ns = now + ticks_ns(t.count ? t.count : 0xFFFFFF);
    }

    void update(uint64_t now) override {
        for (int which = 0; which < 4; which++) {
            Timer &t = timers[which];
            if (!t.active || now < t.fire_ns) continue;
            reg[INTR_CAUSE] |= 1u << (8 + which);
            t.count = reg[TIMER0_COUNT + which] & (which ? 0xFFFFFF : 0xFFFFFFFF);
            if (reg[TIMER_CONTROL] & (2u << (2 * which))) {
                uint64_t period = ticks_ns(t.count ? t.count : 0xFFFFFF);
                // Keep the period; if the guest fell far behind, do not replay every missed tick.
                t.fire_ns += period;
                if (t.fire_ns <= now) t.fire_ns = now + period;
                t.start_ns = t.fire_ns - period;
            } else {
                t.active = false;
                t.count = 0;
            }
            update_irq();
        }
    }

    // DMA runs to completion as soon as it is enabled.
    void fetch_next(int which) {
        uint32_t a = (reg[DMA0_CONTROL + which] & 0x200) ? 0 : reg[DMA0_NEXT + which];
        if (!a) {
            reg[DMA0_CONTROL + which] &= ~0x5000u;
            return;
        }
        reg[DMA0_COUNT + which] = rt::phys_read32(a);
        reg[DMA0_SOURCE + which] = rt::phys_read32(a + 4);
        reg[DMA0_DEST + which] = rt::phys_read32(a + 8);
        reg[DMA0_NEXT + which] = rt::phys_read32(a + 12);
    }

    void run_dma(int which) {
        for (;;) {
            uint32_t ctl = reg[DMA0_CONTROL + which];
            uint32_t src = reg[DMA0_SOURCE + which], dst = reg[DMA0_DEST + which];
            uint32_t left = reg[DMA0_COUNT + which] & 0xFFFF;
            int sinc = ((ctl >> 2) & 3) == 0 ? 4 : ((ctl >> 2) & 3) == 1 ? -4 : 0;
            int dinc = ((ctl >> 4) & 3) == 0 ? 4 : ((ctl >> 4) & 3) == 1 ? -4 : 0;
            while (left >= 4) {
                rt::phys_write32(dst, rt::phys_read32(src));
                src += sinc;
                dst += dinc;
                left -= 4;
            }
            while (left) {
                uint32_t b = rt_rd_slow(src | 0xA0000000u, 1);
                rt_wr_slow(dst | 0xA0000000u, b, 1);
                src += sinc / 4;
                dst += dinc / 4;
                left--;
            }
            reg[DMA0_SOURCE + which] = src;
            reg[DMA0_DEST + which] = dst;
            reg[DMA0_COUNT + which] &= ~0xFFFFu;
            if (!(ctl & (1u << 10))) reg[INTR_CAUSE] |= 1u << (4 + which);
            if (ctl & 0x200) break;
            uint32_t next = reg[DMA0_NEXT + which];
            if (!next) {
                if (ctl & (1u << 10)) reg[INTR_CAUSE] |= 1u << (4 + which);
                break;
            }
            fetch_next(which);
        }
        reg[DMA0_CONTROL + which] &= ~0x5000u;
        update_irq();
    }

    rt::PciDevice *pci_target(uint32_t addr) {
        if (!(addr & 0x80000000u) || (addr & 0xFF0000)) return nullptr; // bus 0 only
        int dev = (addr >> 11) & 31, fn = (addr >> 8) & 7;
        if (fn) return nullptr;
        for (rt::PciDevice *d : pci)
            if (d->slot == dev) return d;
        return nullptr;
    }

    uint32_t read(uint32_t off, int) override {
        uint32_t i = off / 4;
        switch (i) {
        case TIMER0_COUNT: case TIMER0_COUNT + 1: case TIMER0_COUNT + 2: case TIMER0_COUNT + 3:
            return timer_value(i - TIMER0_COUNT, rt::now_ns());
        case PCI_COMMAND:
            return (reg[i] & ~1u) | 1; // little-endian
        case CONFIG_DATA: {
            uint32_t a = reg[CONFIG_ADDRESS];
            int r = (a >> 2) & 63;
            if (r == 0 && (a & 0x80000000u) && ((a >> 11) & 31) == 0) return 0x014611AB; // GT64010 itself
            rt::PciDevice *d = pci_target(a);
            return d ? d->config_read(r) : 0xFFFFFFFF;
        }
        }
        return reg[i];
    }

    void write(uint32_t off, uint32_t v, int size) override {
        uint32_t i = off / 4;
        if (size != 4) {
            // Partial writes only reach the plain registers.
            int sh = (off & 3) * 8;
            uint32_t m = (size == 1 ? 0xFFu : 0xFFFFu) << sh;
            v = (reg[i] & ~m) | ((v << sh) & m);
        }
        uint32_t old = reg[i];
        reg[i] = v;
        uint64_t now = rt::now_ns();
        switch (i) {
        case DMA0_CONTROL: case DMA0_CONTROL + 1: case DMA0_CONTROL + 2: case DMA0_CONTROL + 3: {
            int which = i - DMA0_CONTROL;
            reg[i] = (reg[i] & ~0x4000u) | (old & 0x4000u);
            if (v & 0x2000) fetch_next(which);
            reg[i] &= ~0x2000u;
            if (!(old & 0x1000) && (v & 0x1000)) run_dma(which);
            break;
        }
        case TIMER0_COUNT: case TIMER0_COUNT + 1: case TIMER0_COUNT + 2: case TIMER0_COUNT + 3: {
            int which = i - TIMER0_COUNT;
            if (which) v &= 0xFFFFFF;
            if (!timers[which].active) timers[which].count = v;
            break;
        }
        case TIMER_CONTROL:
            for (int which = 0; which < 4; which++) {
                bool en = v & (1u << (2 * which));
                Timer &t = timers[which];
                if (!t.active && en) start_timer(which, now);
                else if (t.active && !en) {
                    t.count = timer_value(which, now);
                    t.active = false;
                }
            }
            break;
        case INTR_CAUSE:
            reg[i] = old & v;
            update_irq();
            break;
        case CPU_MASK:
            reg[i] &= 0x3C1FFFFE;
            update_irq();
            break;
        case CONFIG_DATA: {
            uint32_t a = reg[CONFIG_ADDRESS];
            if (rt::PciDevice *d = pci_target(a)) d->config_write((a >> 2) & 63, v);
            break;
        }
        }
    }
};

Galileo *galileo;

} // namespace

namespace rt {

uint32_t PciDevice::config_read(int r) {
    switch (r) {
    case 0: return id;
    case 2: return class_rev;
    }
    if (r >= 4 && r < 10) {
        int bar = r - 4;
        if (bar_size[bar] == 0) return 0;
        return config[r] & ~(bar_size[bar] - 1);
    }
    return config[r];
}

void PciDevice::config_write(int r, uint32_t v) {
    config[r] = v;
    config_written(r, v);
    if (r >= 4 && r < 10 && bar_size[r - 4]) bar_written(r - 4, v & ~(bar_size[r - 4] - 1));
}

void galileo_init() {
    galileo = new Galileo();
    add_device(galileo);
}

void pci_add(PciDevice *d) { galileo->pci.push_back(d); }

} // namespace rt
