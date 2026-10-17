// Seattle board registers on the Galileo's chip select 3 (physical 0x16000000-0x17FFFFFF), following
// MAME's seattle.cpp (California Speed: "seattle150_widget" with a DCS2 sound board):
//   0x16000000  I/O ASIC (16 registers, one per word)
//   0x16100000  CMOS (battery-backed RAM, one byte per word)
//   0x16C00000  widget board: analog inputs (ADC0848), outputs, ethernet
//   0x17000000  CMOS write protect     0x17100000  watchdog
//   0x17300000  interrupt enable       0x17400000  interrupt configuration
//   0x17500000  interrupt state        0x17600000  interrupt state 2
//   0x17700000  VBLANK clear           0x17900000  status LEDs     0x17F00000  ASIC reset
#include "board.h"
#include "dcs.h"

#include <cstdio>
#include <cstring>
#include <string>

namespace {

enum {
    IO_PORT0, IO_PORT1, IO_PORT2, IO_PORT3, IO_UARTCONTROL, IO_UARTOUT, IO_UARTIN, IO_COIN, IO_SOUNDCTL,
    IO_SOUNDOUT, IO_SOUNDSTAT, IO_SOUNDIN, IO_PICOUT, IO_PICIN, IO_INTSTAT, IO_INTCTL
};
// California Speed's register shuffle once the I/O ASIC is unlocked (MAME SHUFFLE_CALSPEED).
const uint8_t SHUFFLE[16] = {0x8, 0x9, 0xa, 0xb, 0x0, 0x1, 0x2, 0x3, 0xf, 0xe, 0xc, 0xd, 0x4, 0x5, 0x6, 0x7};

const int IOASIC_IRQ_LINE = 1; // IP3
const int VBLANK_SHIFT = 7, WIDGET_SHIFT = 1;

struct Board : rt::Device {
    uint16_t io[16] = {};
    uint32_t cmos[0x20000 / 4];
    bool cmos_write = false;
    uint32_t int_enable = 0, int_config = 0, leds = 0, asic_reset = 0;
    int vblank_line = -1, widget_line = -1;
    bool vblank_latch = false, vblank_state = false;
    uint32_t analog_value = 0;
    std::string cmos_path;
    bool cmos_dirty = false;

    Board() : Device("board", 0x16000000, 0x02000000) { memset(cmos, 0xFF, sizeof(cmos)); }

    void load_cmos(const std::string &path) {
        cmos_path = path;
        if (FILE *f = fopen(path.c_str(), "rb")) {
            fread(cmos, 1, sizeof(cmos), f);
            fclose(f);
        }
    }
    void save_cmos() {
        if (!cmos_dirty || cmos_path.empty()) return;
        if (FILE *f = fopen(cmos_path.c_str(), "wb")) {
            fwrite(cmos, 1, sizeof(cmos), f);
            fclose(f);
        }
        cmos_dirty = false;
    }

    // ---- I/O ASIC ----

    void update_ioasic_irq() {
        uint16_t bits = 0x2000;              // UART transmitter empty
        if (rt::dcs_input_empty()) bits |= 0x0080; // sound board can take data
        if (rt::dcs_output_full()) bits |= 0x0040; // sound board has data for us
        bits |= io[IO_UARTIN] & 0x3F00;
        if (rt::dcs_fifo_status() & 8) bits |= 0x0008; // sound FIFO empty
        bits |= 1;
        io[IO_INTSTAT] = bits;
        bool on = (io[IO_INTCTL] & 1) && (io[IO_INTSTAT] & io[IO_INTCTL] & 0x3FFE);
        rt::set_irq(IOASIC_IRQ_LINE, on);
    }

    uint32_t ioasic_read(int r) {
        r = SHUFFLE[r & 15];
        switch (r) {
        case IO_PORT0: return rt::input_dips();
        case IO_PORT1: return rt::input_system();
        case IO_PORT2: return rt::input_in1();
        case IO_PORT3: return 0xFFFF;
        case IO_UARTIN: {
            uint32_t v = io[r];
            io[r] &= ~0x1000;
            update_ioasic_irq();
            return v;
        }
        case IO_SOUNDSTAT: return rt::sound_status();
        case IO_SOUNDIN: return rt::sound_data_read();
        case IO_PICIN: return rt::pic_read();
        }
        return io[r];
    }

    void ioasic_write(int r, uint32_t v) {
        r = SHUFFLE[r & 15];
        uint16_t old = io[r];
        io[r] = (uint16_t)v;
        switch (r) {
        case IO_UARTOUT:
            if (io[IO_UARTCONTROL] & 0x800) io[IO_UARTIN] = (v & 0xFF) | 0x3000;
            else {
                fputc((int)(v & 0xFF), stderr);
                io[IO_UARTIN] |= 0x2000;
            }
            update_ioasic_irq();
            break;
        case IO_SOUNDCTL:
            rt::sound_control(old, (uint16_t)v);
            break;
        case IO_SOUNDOUT:
            rt::sound_data_write((uint16_t)v);
            break;
        case IO_SOUNDIN:
            rt::sound_ack();
            break;
        case IO_PICOUT:
            rt::pic_write((uint8_t)v);
            break;
        case IO_INTCTL:
            update_ioasic_irq();
            break;
        }
    }

    // ---- interrupts ----

    void update_vblank_irq() {
        if (vblank_line < 0) return;
        rt::set_irq(vblank_line, vblank_latch && (int_enable & (1u << VBLANK_SHIFT)));
    }

    void config_irqs() {
        if (vblank_line >= 0) rt::set_irq(vblank_line, false);
        int irq = (int_config >> (2 * VBLANK_SHIFT)) & 3;
        vblank_line = irq ? 2 + irq : -1;
        if (widget_line >= 0) rt::set_irq(widget_line, false);
        irq = (int_config >> (2 * WIDGET_SHIFT)) & 3;
        widget_line = irq ? 2 + irq : -1;
        update_vblank_irq();
    }

    void vblank_assert(bool state) {
        vblank_state = state;
        if (state != bool(int_enable & 0x100)) {
            vblank_latch = true;
            update_vblank_irq();
        }
        if (state) rt::vblank();
    }

    void update(uint64_t now) override {
        rt::dcs_update(now);
        static uint64_t last_save;
        if (now - last_save > 2000000000ull) {
            last_save = now;
            save_cmos();
        }
    }

    // ---- widget ----

    uint32_t widget_read(uint32_t off) {
        switch (off) {
        case 0x04: return ~0u;               // no ethernet interrupt
        case 0x10: return analog_value;
        }
        return ~0u;
    }
    void widget_write(uint32_t off, uint32_t v) {
        switch (off) {
        case 0x0C: rt::widget_output(v); break;
        case 0x10: analog_value = rt::analog_input((int)(v & 7)); break;
        }
    }

    uint32_t read(uint32_t off, int) override {
        if (off < 0x40) return ioasic_read(off / 4);
        if (off >= 0x100000 && off < 0x120000) return cmos[(off - 0x100000) / 4];
        if (off >= 0xC00000 && off < 0xC00020) return widget_read(off - 0xC00000);
        switch (off) {
        case 0x1000000: return cmos_write;
        case 0x1300000: return int_enable;
        case 0x1400000: return int_config;
        case 0x1500000: return vblank_latch << VBLANK_SHIFT;
        case 0x1600000: return (vblank_latch << VBLANK_SHIFT) | (vblank_state << 8);
        case 0x1900000: return leds | 0xFFFFFF00;
        case 0x1F00000: return asic_reset;
        }
        rt::log("board read %08x", base + off);
        return 0;
    }

    void write(uint32_t off, uint32_t v, int) override {
        if (off < 0x40) return ioasic_write(off / 4, v);
        if (off >= 0x100000 && off < 0x120000) {
            if (cmos_write) {
                cmos[(off - 0x100000) / 4] = v;
                cmos_dirty = true;
            }
            cmos_write = false;
            return;
        }
        if (off >= 0xC00000 && off < 0xC00020) return widget_write(off - 0xC00000, v);
        switch (off) {
        case 0x1000000: cmos_write = true; return;
        case 0x1100000: return; // watchdog
        case 0x1300000: {
            uint32_t old = int_enable;
            int_enable = v;
            if (old != v) update_vblank_irq();
            return;
        }
        case 0x1400000: int_config = v; config_irqs(); return;
        case 0x1700000: vblank_latch = false; update_vblank_irq(); return;
        case 0x1800000: return;
        case 0x1900000: leds = v & 0xFF; return;
        case 0x1F00000: asic_reset = v; return;
        }
        rt::log("board write %08x <- %08x", base + off, v);
    }
};

Board *board;

} // namespace

namespace rt {

void board_init(const std::string &cmos_path) {
    board = new Board();
    board->load_cmos(cmos_path);
    add_device(board);
}

void board_shutdown() { board->save_cmos(); }

void board_vblank(bool state) { board->vblank_assert(state); }

void dcs_irq_changed() {
    if (board) board->update_ioasic_irq();
}

} // namespace rt
