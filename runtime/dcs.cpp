// DCS2 sound board (RAM-based, Seattle): ADSP-2115 at 16 MHz, SDRC ASIC, 2 MB of sound DRAM, stereo
// output at 31250 Hz. The DSP boots from the board's ROM (sound102.u95), then the main CPU downloads the
// sound system and data through the I/O ASIC.
//
// The board logic follows MAME's src/mame/shared/dcs.cpp (BSD-3-Clause, Aaron Giles), DCS2/2115 parts
// only, without its HLE transfer shortcut. The DSP runs in step with real time from update().
#include "dcs.h"
#include "adsp/adsp21xx.h"

#include <algorithm>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <vector>

namespace rt {
namespace {

using adsp::adsp21xx_device;

const uint64_t DSP_HZ = 16000000;
const size_t FIFO_SIZE = 512; // the I/O ASIC's sound FIFO, in words (as MAME)
const uint16_t LCTRL_OUTPUT_EMPTY = 0x400, LCTRL_INPUT_EMPTY = 0x800;

enum {
    IDMA_CONTROL_REG = 0, BDMA_INT_ADDR_REG, BDMA_EXT_ADDR_REG, BDMA_CONTROL_REG, BDMA_WORD_COUNT_REG,
    PROG_FLAG_DATA_REG, PROG_FLAG_CONTROL_REG,
    S1_AUTOBUF_REG = 15, S1_RFSDIV_REG, S1_SCLKDIV_REG, S1_CONTROL_REG, S0_AUTOBUF_REG, S0_RFSDIV_REG,
    S0_SCLKDIV_REG, S0_CONTROL_REG, S0_MCTXLO_REG, S0_MCTXHI_REG, S0_MCRXLO_REG, S0_MCRXHI_REG,
    TIMER_SCALE_REG, TIMER_COUNT_REG, TIMER_PERIOD_REG, WAITSTATES_REG, SYSCONTROL_REG
};

struct Dcs : adsp::Bus {
    adsp21xx_device cpu{this};
    std::vector<uint16_t> bootrom;          // 32k words, low byte of each
    std::vector<uint16_t> sounddata;        // 2 MB DRAM
    std::vector<uint16_t> sram = std::vector<uint16_t>(0x8000 * 2);
    uint32_t internal_pm[0x400] = {};
    uint16_t iram[0x200] = {};
    uint16_t control_regs[32] = {};
    uint16_t sdrc[4] = {};
    uint8_t sdrc_seed = 0;
    bool in_reset = true;
    uint64_t cycles = 0;                    // DSP cycles run so far
    uint64_t last_ns = 0;

    // host interface
    uint16_t latch_control = LCTRL_INPUT_EMPTY | LCTRL_OUTPUT_EMPTY;
    uint16_t input_data = 0, output_data = 0, output_control = 0;
    uint16_t progflags = 0;

    // timer
    bool timer_enable = false, timer_ignore = false;
    uint32_t timer_scale = 1, timer_period = 0, timer_start_count = 0;
    uint64_t timer_start_cycles = 0, timers_fired = 0;
    uint64_t timer_next = ~0ull;

    // autobuffered SPORT1 output
    int ireg = 0, size = 0, incs = 0, ireg_base = 0;
    uint64_t sound_period_cycles = 0, sound_next = ~0ull;
    double sample_rate = 31250;
    uint64_t sport0_next = ~0ull;

    std::vector<int16_t> audio; // interleaved stereo, consumed by the audio output
    FILE *wav = nullptr;        // ARC_DUMP_AUDIO: raw output as a WAV file
    uint32_t wav_bytes = 0;
    uint64_t samples_out = 0;   // stereo frames produced (for log timestamps)
    FILE *dsp_trace = getenv("ARC_DSP_TRACE") ? fopen(getenv("ARC_DSP_TRACE"), "w") : nullptr;
    int dsp_trace_state = 0;

    void wav_header() {
        if (!wav) return;
        uint32_t rate = 31250, byterate = rate * 4, riff = 36 + wav_bytes;
        uint16_t fmt[] = {1, 2};
        uint16_t align = 4, bits = 16;
        fseek(wav, 0, SEEK_SET);
        fwrite("RIFF", 1, 4, wav); fwrite(&riff, 4, 1, wav); fwrite("WAVEfmt ", 1, 8, wav);
        uint32_t sixteen = 16;
        fwrite(&sixteen, 4, 1, wav); fwrite(fmt, 2, 2, wav); fwrite(&rate, 4, 1, wav); fwrite(&byterate, 4, 1, wav);
        fwrite(&align, 2, 1, wav); fwrite(&bits, 2, 1, wav); fwrite("data", 1, 4, wav); fwrite(&wav_bytes, 4, 1, wav);
        fseek(wav, 0, SEEK_END);
        fflush(wav);
    }

    int sdrc_rom_st() const { return sdrc[0] & 3; }
    int sdrc_rom_sz() const { return (sdrc[0] >> 4) & 1; }
    int sdrc_rom_ms() const { return (sdrc[0] >> 5) & 1; }
    int sdrc_rom_pg() const { return (sdrc[0] >> 7) & 7; }
    int sdrc_sm_en() const { return (sdrc[0] >> 11) & 1; }
    int sdrc_sm_bk() const { return (sdrc[0] >> 12) & 1; }
    int sdrc_smode() const { return (sdrc[0] >> 13) & 7; }
    int sdrc_dm_st() const { return sdrc[1] & 3; }
    int sdrc_dm_pg() const { return sdrc[2] & 0x7FF; }

    // ---- memory ----

    uint32_t program_read(uint32_t a) override {
        if (a < 0x400) return internal_pm[a];
        if (a >= 0x800 && sdrc_sm_en()) {
            // 24-bit words as two 16-bit SRAM halves from sram[0x4800] (MAME's install_ram on the 32-bit space)
            uint32_t i = 0x4800 + (a - 0x800) * 2;
            return ((uint32_t)sram[i + 1] << 16 | sram[i]) & 0xFFFFFF;
        }
        return 0xFFFFFF;
    }
    void program_write(uint32_t a, uint32_t d) override {
        if (a < 0x400) internal_pm[a] = d & 0xFFFFFF;
        else if (a >= 0x800 && sdrc_sm_en()) {
            uint32_t i = 0x4800 + (a - 0x800) * 2;
            sram[i] = (uint16_t)d;
            sram[i + 1] = (uint16_t)(d >> 16);
        }
    }

    // ROM page and DRAM page windows, which take precedence over the SRAM.
    bool rom_window(uint32_t a, uint32_t &off) const {
        if (!(sdrc_rom_ms() == 1 && sdrc_rom_st() != 3 && sdrc_sm_en())) return false;
        uint32_t base = sdrc_rom_st() == 0 ? 0 : sdrc_rom_st() == 1 ? 0x3000 : 0x3400;
        uint32_t pagesize = (sdrc_rom_sz() == 0 && sdrc_rom_st() != 0) ? 4096 : 1024;
        if (a < base || a >= base + pagesize) return false;
        off = (sdrc_rom_pg() * 4096 + (a - base)) % bootrom.size();
        return true;
    }
    bool dram_window(uint32_t a, uint32_t &off) const {
        if (sdrc_dm_st() == 0 || !sdrc_sm_en()) return false;
        uint32_t base = sdrc_dm_st() == 1 ? 0 : sdrc_dm_st() == 2 ? 0x3000 : 0x3400;
        if (a < base || a >= base + 0x400) return false;
        off = (sdrc_dm_pg() * 1024 + (a - base)) % sounddata.size();
        return true;
    }
    uint16_t *sram_data(uint32_t a) {
        if (!sdrc_sm_en() || a < 0x800 || a >= 0x3800) return nullptr;
        if (sdrc_sm_bk() == 0) {
            if (a < 0x1800) return &sram[a - 0x800];
            if (a < 0x2800) return &sram[0x1000 + a - 0x1800];
            return &sram[0x2000 + a - 0x2800];
        }
        if (a < 0x1800) return nullptr;
        if (a < 0x2800) return &sram[0x3000 + a - 0x1800];
        return &sram[0x2000 + a - 0x2800];
    }

    uint16_t data_read(uint32_t a) override {
        uint32_t off;
        if (dram_window(a, off)) return sounddata[off];
        if (rom_window(a, off)) return bootrom[off];
        if (a >= 0x400 && a < 0x408) {
            switch (a) {
            case 0x400: return input_data; // input latch
            case 0x402: return output_control;
            case 0x403: return latch_status();
            case 0x404: case 0x405: case 0x406: case 0x407: return fifo_read();
            }
            return 0xFFFF;
        }
        if (a >= 0x480 && a < 0x484) return sdrc_read(a - 0x480);
        if (uint16_t *p = sram_data(a)) return *p;
        if (a >= 0x3800 && a < 0x3A00) return iram[a - 0x3800];
        if (a >= 0x3FE0) return control_read(a - 0x3FE0);
        return 0xFFFF;
    }

    void data_write(uint32_t a, uint16_t d) override {
        uint32_t off;
        if (dram_window(a, off)) {
            sounddata[off] = d;
            return;
        }
        // The ROM window is read-only: writes reach what is mapped underneath (as MAME's read bank).
        if (a >= 0x400 && a < 0x408) {
            switch (a) {
            case 0x400: input_latch_ack(); break;
            case 0x401: output_latch(d); break;
            case 0x402: output_control = d; break;
            }
            return;
        }
        if (a >= 0x480 && a < 0x484) return sdrc_write(a - 0x480, d);
        if (uint16_t *p = sram_data(a)) {
            *p = d;
            return;
        }
        if (a >= 0x3800 && a < 0x3A00) {
            iram[a - 0x3800] = d;
            return;
        }
        if (a >= 0x3FE0) control_write(a - 0x3FE0, d);
    }

    // ---- SDRC ----

    uint16_t sdrc_read(int r) {
        if (r != 3) return sdrc[r];
        switch (sdrc_smode()) {
        default: return 0x5A81;
        case 1: return 0x5AA4;
        case 2: return 0x5A00 | ((sdrc_seed & 0x3F) << 1);
        case 3: return 0x5AB9;
        case 4: return 0x5A03;
        case 5: return 0x5A69;
        case 6: return 0x5A20;
        case 7: return 0x5AFF;
        }
    }
    void sdrc_write(int r, uint16_t d) {
        if (r < 3) {
            sdrc[r] = d;
            return;
        }
        switch (sdrc_smode()) {
        case 1: sdrc_seed = d & 0xFF; break;
        case 3: sdrc_seed = (uint8_t)((sdrc_seed << 1) | 1); break;
        case 4: sdrc_seed = (uint8_t)(sdrc_seed + (sdrc_seed >> 1)); break;
        case 5: sdrc_seed ^= (uint8_t)((sdrc_seed << 1) | 1); break;
        case 6: sdrc_seed = (uint8_t)((((sdrc_seed << 7) ^ (sdrc_seed << 5) ^ (sdrc_seed << 4) ^ (sdrc_seed << 3)) & 0x80) | (sdrc_seed >> 1)); break;
        case 7: sdrc_seed = (uint8_t)~sdrc_seed; break;
        }
    }

    // ---- host latches ----

    uint16_t latch_status() {
        uint16_t r = 0;
        if (!(latch_control & LCTRL_INPUT_EMPTY)) r |= 0x80;
        if (latch_control & LCTRL_OUTPUT_EMPTY) r |= 0x40;
        if (transfer.state == 0) r |= fifo_status() & 0x38;
        else r |= 0x08; // a shortcut transfer is in progress: the DSP sees an empty FIFO
        return r;
    }
    void input_latch_ack() {
        latch_control |= LCTRL_INPUT_EMPTY;
        cpu.set_input_line(adsp::ADSP2115_IRQ2, adsp::CLEAR_LINE);
        sound_irq_changed();
    }
    void output_latch(uint16_t d) {
        latch_control &= ~LCTRL_OUTPUT_EMPTY;
        output_data = d;
        sound_irq_changed();
    }

    // ---- control registers ----

    uint16_t control_read(int r) {
        switch (r) {
        case PROG_FLAG_DATA_REG: {
            uint16_t v = (control_regs[PROG_FLAG_CONTROL_REG] & control_regs[PROG_FLAG_DATA_REG]) |
                         (progflags & ~control_regs[PROG_FLAG_CONTROL_REG]);
            progflags ^= 0x6;
            return v;
        }
        case TIMER_COUNT_REG:
            update_timer_count();
            return control_regs[r];
        }
        return control_regs[r];
    }

    void control_write(int r, uint16_t d) {
        control_regs[r] = d;
        switch (r) {
        case SYSCONTROL_REG:
            if (d & 0x0200) { // reboot
                cpu.reset();
                boot();
                control_regs[SYSCONTROL_REG] = 0;
            }
            if (!(d & 0x0800)) sound_next = ~0ull;                    // SPORT1 off
            sport0_next = (d & 0x1000) ? cycles + DSP_HZ / 100000 : ~0ull; // SPORT0 as a 1 kHz timer
            break;
        case S1_AUTOBUF_REG:
            if (!(d & 2)) sound_next = ~0ull;
            break;
        case TIMER_SCALE_REG: {
            uint32_t v = (d & 0xFF) + 1;
            if (v != timer_scale) {
                update_timer_count();
                timer_scale = v;
                reset_timer();
            }
            break;
        }
        case TIMER_COUNT_REG:
            timer_start_count = d;
            reset_timer();
            break;
        case TIMER_PERIOD_REG:
            if (d != timer_period) {
                update_timer_count();
                timer_period = d;
                reset_timer();
            }
            break;
        }
    }

    // ---- timer ----

    void update_timer_count() {
        if (!timer_enable) return;
        uint64_t elapsed = (cycles - timer_start_cycles) / timer_scale;
        if (elapsed < timer_start_count + 1ull) {
            timer_start_count -= (uint32_t)elapsed;
        } else {
            elapsed -= timer_start_count + 1ull;
            elapsed %= timer_period + 1ull;
            timer_start_count = timer_period - (uint32_t)elapsed;
        }
        control_regs[TIMER_COUNT_REG] = (uint16_t)timer_start_count;
        timer_start_cycles = cycles;
        timers_fired = 0;
        if (timer_enable) schedule_timer();
    }

    void schedule_timer() {
        timer_next = timer_start_cycles + timer_scale * (timer_start_count + 1ull + timers_fired * (timer_period + 1ull));
    }

    void reset_timer() {
        if (!timer_enable) return;
        timer_start_cycles = cycles;
        timers_fired = 0;
        if (!timer_ignore) {
            // A timer whose handler only refreshes the DRAM page register does not need to run.
            if (program_read(0x18) == 0x0C0030 && program_read(0x19) == 0x804828 && program_read(0x1A) == 0x904828 &&
                program_read(0x1B) == 0x0C0020 && program_read(0x1C) == 0x0A001F)
                timer_ignore = true;
        }
        if (timer_ignore) timer_next = ~0ull;
        else schedule_timer();
    }

    void timer_fired(int state) override {
        if (state) {
            timer_enable = true;
            reset_timer();
        } else {
            update_timer_count();
            timer_enable = false;
            timer_next = ~0ull;
        }
    }

    // ---- sound output ----

    void sport_tx(int port, uint32_t) override {
        if (port != 1) return;
        if ((control_regs[SYSCONTROL_REG] & 0x0800) && (control_regs[S1_AUTOBUF_REG] & 2)) {
            ireg = (control_regs[S1_AUTOBUF_REG] >> 9) & 7;
            int mreg = ((control_regs[S1_AUTOBUF_REG] >> 7) & 3) | (ireg & 4);
            uint16_t source = (uint16_t)cpu.i_reg(ireg);
            incs = cpu.m_reg(mreg);
            size = (int)cpu.l_reg(ireg);
            source &= ~0xF;
            cpu.set_i_reg(ireg, source);
            ireg_base = source;
            // Sample clock: internal (from SCLKDIV) or the external 31.25 kHz.
            if (control_regs[S1_CONTROL_REG] & 0x4000)
                sample_rate = (double)DSP_HZ / (2.0 * (control_regs[S1_SCLKDIV_REG] + 1)) / (16.0 * 2);
            else
                sample_rate = 31250;
            if (incs && size) {
                // fire every half buffer
                double samples = (double)size / (2.0 * 2 * incs);
                sound_period_cycles = (uint64_t)(samples * DSP_HZ / sample_rate);
                if (!sound_period_cycles) sound_period_cycles = 1;
                sound_next = cycles + sound_period_cycles;
            }
            return;
        }
        sound_next = ~0ull;
    }

    void sound_irq() {
        int reg = (int)cpu.i_reg(ireg);
        int count = size / (2 * (incs ? incs : 1));
        samples_out += count / 2;
        for (int i = 0; i < count; i++) {
            audio.push_back((int16_t)data_read((uint32_t)reg & 0x3FFF));
            reg += incs;
        }
        if (wav) {
            fwrite(&audio[audio.size() - count], 2, count, wav);
            wav_bytes += count * 2;
        }
        if (audio.size() > 31250 * 2 * 2) audio.erase(audio.begin(), audio.begin() + (audio.size() - 31250 * 2)); // keep ~1 s
        ireg_base = (int)cpu.get_ibase(ireg);
        if (reg >= ireg_base + size) {
            reg = ireg_base;
            cpu.pulse_input_line(adsp::ADSP2115_IRQ1);
        }
        cpu.set_i_reg(ireg, (uint32_t)reg);
    }

    // ---- boot / reset ----

    void boot() {
        uint8_t buffer[0x1000];
        size_t base = (size_t)sdrc_rom_pg() * 0x1000; // RAM-based board: boot page from SDRC_ROM_PG
        for (int i = 0; i < 0x1000; i++) buffer[i] = (uint8_t)bootrom[(base + i) % bootrom.size()];
        cpu.load_boot_data(buffer, internal_pm);
    }

    void reset() {
        memset(sdrc, 0, sizeof(sdrc));
        memset(control_regs, 0, sizeof(control_regs));
        size = incs = ireg = 0;
        cpu.set_input_line(adsp::ADSP2115_IRQ0, adsp::CLEAR_LINE);
        cpu.set_input_line(adsp::ADSP2115_IRQ1, adsp::CLEAR_LINE);
        cpu.set_input_line(adsp::ADSP2115_IRQ2, adsp::CLEAR_LINE);
        latch_control = LCTRL_INPUT_EMPTY | LCTRL_OUTPUT_EMPTY;
        sound_irq_changed();
        boot();
        timer_ignore = false;
        timer_enable = false;
        timer_scale = 1;
        timer_next = sound_next = sport0_next = ~0ull;
        transfer = Transfer();
        replies.clear();
        watchdog_writes = -1;
    }

    // ---- running ----

    // ARC_DSP_DUMP=<seconds>: dumps program memory to dsp_pm.bin at that time, then profiles the PC
    // for a second (dsp_profile.txt).
    double dump_at = getenv("ARC_DSP_DUMP") ? atof(getenv("ARC_DSP_DUMP")) : -1;
    std::vector<uint32_t> pc_hist = std::vector<uint32_t>(0x4000);
    int profiling = 0; // 0 no, 1 running, 2 done
    uint64_t profile_end = 0;

    void debug_hooks() {
        if (dump_at < 0 || profiling == 2) return;
        double t = now_ns() / 1e9;
        if (!profiling && t >= dump_at) {
            FILE *f = fopen("dsp_pm.bin", "wb");
            for (uint32_t a = 0; a < 0x4000; a++) {
                uint32_t w = program_read(a);
                fwrite(&w, 4, 1, f);
            }
            fclose(f);
            f = fopen("dsp_dm.bin", "wb"); // SRAM data bank 0 (0800-37ff), then internal RAM (3800-39ff)
            fwrite(sram.data(), 2, 0x3000, f);
            fwrite(iram, 2, 0x200, f);
            fclose(f);
            profiling = 1;
            profile_end = cycles + DSP_HZ;
            log("dcs: program memory dumped, profiling");
        } else if (profiling == 1 && cycles >= profile_end) {
            FILE *f = fopen("dsp_profile.txt", "w");
            std::vector<std::pair<uint32_t, uint32_t>> v;
            for (uint32_t a = 0; a < 0x4000; a++) if (pc_hist[a]) v.push_back({pc_hist[a], a});
            std::sort(v.rbegin(), v.rend());
            for (auto &[n, a] : v) fprintf(f, "%04x %u\n", a, n);
            fclose(f);
            profiling = 2;
            log("dcs: profile written");
        }
    }

    void run_until(uint64_t target) {
        debug_hooks();
        while (cycles < target) {
            // ARC_DSP_TRACE: registers at each instruction of the first effect frame (from 2955 to the
            // wait loop at 2c71), in the format of a MAME debugger trace.
            if (dsp_trace && dsp_trace_state < 2 && samples_out > 31250 * 6) {
                uint32_t pc = cpu.pc();
                if (dsp_trace_state == 0 && pc == 0x2955) dsp_trace_state = 1;
                if (dsp_trace_state == 1) {
                    if (pc == 0x2C71) {
                        dsp_trace_state = 2;
                        fclose(dsp_trace);
                        log("dcs: trace written");
                    } else {
                        cpu.trace_registers(dsp_trace);
                        fprintf(dsp_trace, "%04X:\n", pc);
                    }
                }
                cycles += (uint64_t)cpu.run(1);
                if (cycles >= timer_next || cycles >= sound_next || cycles >= sport0_next) goto events;
                continue;
            }
            if (profiling == 1) {
                cycles += (uint64_t)cpu.run(7);
                pc_hist[cpu.pc() & 0x3FFF]++;
                if (cycles >= timer_next || cycles >= sound_next || cycles >= sport0_next) goto events;
                continue;
            }
            uint64_t next = std::min({target, timer_next, sound_next, sport0_next});
            if (next > cycles) {
                int slice = (int)std::min<uint64_t>(next - cycles, 100000);
                cycles += (uint64_t)cpu.run(slice);
            }
        events:
            if (cycles >= timer_next) {
                timers_fired++;
                schedule_timer();
                if (timer_ignore || !(timer_period > 10 || timer_scale > 1)) timer_next = ~0ull;
                cpu.pulse_input_line(adsp::ADSP2115_TIMER);
            }
            if (cycles >= sound_next) {
                sound_next += sound_period_cycles;
                sound_irq();
            }
            if (cycles >= sport0_next) {
                sport0_next += DSP_HZ / 1000;
                cpu.pulse_input_line(adsp::ADSP2115_SPORT0_RX);
            }
        }
    }

    void update(uint64_t now) {
        if (!last_ns) last_ns = now;
        uint64_t dt = now - last_ns;
        last_ns = now;
        transfer_watchdog(now);
        send_replies();
        if (in_reset) return;
        if (dt > 50000000) dt = 50000000; // do not try to catch up more than 50 ms
        static uint64_t last_header;
        if (wav && now - last_header > 1000000000ull) {
            last_header = now;
            wav_header();
        }
        run_until(cycles + dt * DSP_HZ / 1000000000ull);
    }

    // ---- I/O ASIC FIFO (host to DSP stream data) ----

    std::vector<uint16_t> fifo;
    size_t fifo_out = 0;
    uint16_t fifo_read() {
        if (fifo_out < fifo.size()) {
            uint16_t v = fifo[fifo_out++];
            if (fifo_out == fifo.size()) {
                fifo.clear();
                fifo_out = 0;
            }
            sound_irq_changed();
            return v;
        }
        return 0;
    }
    uint16_t fifo_status() const {
        size_t n = fifo.size() - fifo_out;
        uint16_t r = 0;
        if (n == 0) r |= 0x08;
        if (n >= FIFO_SIZE / 2) r |= 0x10;
        if (n >= FIFO_SIZE) r |= 0x20;
        return r;
    }

    // ---- transfer shortcut (MAME's "HLE transfers") ----
    // The main CPU uploads the sound system (command 0x001a) and sound banks (0x55d0/0x55d1, also
    // through the FIFO) a word at a time. As MAME does, these go straight into memory and the
    // checksum replies are sent here, instead of through the DSP's own receive loop.
    struct Transfer {
        int dcs_state = 0, state = 0, start = 0, stop = 0, type = 0, temp = 0, writes_left = 0;
        uint16_t sum = 0;
    } transfer;
    // Replies waiting for the output latch to be free: (word, set output_control 0x03xx after).
    std::vector<std::pair<uint16_t, bool>> replies;
    uint64_t watchdog_ns = 0;
    int watchdog_writes = -1;

    // Each reply goes out a little after the latch is free (MAME uses 1 us timers), so that the
    // host's acknowledge of the previous word does not discard it.
    uint64_t reply_at = 0;
    void send_replies() {
        if (replies.empty() || !(latch_control & LCTRL_OUTPUT_EMPTY)) return;
        uint64_t now = now_ns();
        if (!reply_at) reply_at = now + 2000;
        if (now < reply_at) return;
        reply_at = 0;
        {
            auto [w, control] = replies.front();
            replies.erase(replies.begin());
            output_latch(w);
            if (control) output_control = (output_control & ~0xFF00) | 0x0300;
        }
    }

    bool stage1(uint16_t data) {
        Transfer &t = transfer;
        switch (t.state) {
        case 0:
            if (data == 0x001A) {
                t.state++;
                return true;
            }
            if (data == 0x002A) {
                t.dcs_state = 1;
                log("dcs: start the uploaded program");
            }
            return false;
        case 1: t.start = data; t.state++; return true;
        case 2: t.stop = data; t.state++; return true;
        case 3:
            t.type = data;
            t.state++;
            t.writes_left = t.stop - t.start + 1;
            if (t.type == 0) t.writes_left *= 2;
            log("dcs: upload %04x-%04x type %d (SDRC %04x %04x %04x)", t.start, t.stop, t.type, sdrc[0], sdrc[1], sdrc[2]);
            t.sum = 0;
            if (t.type == 1 && sdrc_sm_bk() == 1) sdrc[0] &= ~0x1000;
            if (t.type == 2 && sdrc_sm_bk() == 0) sdrc[0] |= 0x1000;
            return true;
        case 4:
            t.sum += data;
            if (--t.writes_left == 0) t.state = 0;
            if (t.type == 0) {
                if (t.writes_left & 1) t.temp = data;
                else program_write((uint32_t)t.start++ & 0x3FFF, ((uint32_t)t.temp << 8) | (data & 0xFF));
            } else {
                data_write((uint32_t)t.start++ & 0x3FFF, data);
            }
            if (t.state == 0) {
                replies.push_back({t.sum, false});
                replies.push_back({0x000A, false});
            }
            return true;
        }
        return false;
    }

    bool stage2(uint16_t data) {
        Transfer &t = transfer;
        switch (t.state) {
        case 0:
            if (data == 0x55D0 || data == 0x55D1) {
                t.state++;
                return true;
            }
            return false;
        case 1: t.start = data << 16; t.state++; return true;
        case 2: t.start |= data; t.state++; return true;
        case 3: t.stop = data << 16; t.state++; return true;
        case 4:
            t.stop |= data;
            t.state++;
            t.writes_left = t.stop - t.start + 1;
            t.sum = 0;
            watchdog_ns = now_ns() + 1000000;
            watchdog_writes = t.writes_left;
            return true;
        case 5:
            t.sum += data;
            if (--t.writes_left == 0) t.state = 0;
            sounddata[(size_t)t.start++ % sounddata.size()] = data;
            if (t.state == 0) {
                replies.push_back({t.sum, true});
                watchdog_writes = -1;
            }
            return true;
        }
        return false;
    }

    bool preprocess(uint16_t data) {
        bool done = transfer.dcs_state == 0 ? stage1(data) : stage2(data);
        if (done) send_replies();
        return done;
    }

    // During a bank transfer, FIFO data is part of it.
    void fifo_notify() {
        if (transfer.state == 0) return;
        size_t n = fifo.size() - fifo_out;
        if (transfer.state != 5 || (int)n == transfer.writes_left || n >= 256)
            while (fifo_out < fifo.size()) preprocess(fifo_read());
    }

    void transfer_watchdog(uint64_t now) {
        if (watchdog_writes < 0 || now < watchdog_ns) return;
        watchdog_ns = now + 1000000;
        if (fifo_out < fifo.size() && watchdog_writes == transfer.writes_left)
            while (fifo_out < fifo.size()) preprocess(fifo_read());
        watchdog_writes = transfer.writes_left;
    }

    void sound_irq_changed();
};

Dcs *dcs;

} // namespace

void Dcs::sound_irq_changed() { dcs_irq_changed(); }

void dcs_init(std::vector<uint8_t> boot_rom) {
    dcs = new Dcs();
    dcs->bootrom.resize(boot_rom.size());
    for (size_t i = 0; i < boot_rom.size(); i++) dcs->bootrom[i] = boot_rom[i];
    dcs->sounddata.assign((2 << 20) / 2, 0);
    dcs->reset();
    if (const char *p = getenv("ARC_DUMP_AUDIO")) {
        dcs->wav = fopen(p, "wb");
        dcs->wav_header();
    }
    // At power-on the DSP runs its boot code and waits for the main CPU.
    dcs->cpu.reset();
    dcs->in_reset = false;
}

void dcs_update(uint64_t now) { dcs->update(now); }

// Host side, as the I/O ASIC sees it.
uint16_t dcs_control() { return dcs->latch_control; }
uint16_t dcs_output_control() { return dcs->output_control; }
bool dcs_output_full() { return !(dcs->latch_control & LCTRL_OUTPUT_EMPTY); }
bool dcs_input_empty() { return dcs->latch_control & LCTRL_INPUT_EMPTY; }

void dcs_data_write(uint16_t d) {
    static int lost;
    if (!(dcs->latch_control & LCTRL_INPUT_EMPTY) && lost++ < 20)
        log("dcs: host wrote %04x while the DSP still had %04x unread", d, dcs->input_data);
    static FILE *trace = getenv("ARC_DCS_LOG") ? fopen(getenv("ARC_DCS_LOG"), "w") : nullptr;
    if (trace) fprintf(trace, "%.6f a%.3f host->dsp %04x\n", now_ns() / 1e9, dcs->samples_out / 31250.0, d);
    if (dcs->preprocess(d)) {
        dcs->sound_irq_changed(); // the input latch stays empty: the I/O ASIC's "can write" interrupt
        return;
    }
    dcs->cpu.set_input_line(adsp::ADSP2115_IRQ2, adsp::ASSERT_LINE);
    dcs->latch_control &= ~LCTRL_INPUT_EMPTY;
    dcs->input_data = d;
    dcs->sound_irq_changed();
}

uint16_t dcs_data_read() {
    uint16_t v = dcs->output_data;
    static FILE *trace = getenv("ARC_DCS_LOG") ? fopen("dcs_read.log", "w") : nullptr;
    if (trace) fprintf(trace, "%.6f dsp->host %04x %s\n", now_ns() / 1e9, v, (dcs->latch_control & LCTRL_OUTPUT_EMPTY) ? "(empty)" : "");
    return v;
}

void dcs_ack() {
    dcs->latch_control |= LCTRL_OUTPUT_EMPTY;
    dcs->send_replies();
    dcs->sound_irq_changed();
}

void dcs_reset_line(bool high) {
    if (!high) {
        dcs->reset();
        dcs->in_reset = true;
    } else if (dcs->in_reset) {
        dcs->cpu.reset();
        dcs->in_reset = false;
    }
}

void dcs_fifo_write(uint16_t d) {
    static FILE *dump = getenv("ARC_DUMP_FIFO") ? fopen(getenv("ARC_DUMP_FIFO"), "wb") : nullptr;
    if (dump) {
        fwrite(&d, 2, 1, dump);
        fflush(dump);
    }
    if (dcs->fifo.size() - dcs->fifo_out < FIFO_SIZE) dcs->fifo.push_back(d);
    dcs->fifo_notify();
    dcs->sound_irq_changed();
}
void dcs_fifo_reset() {
    dcs->fifo.clear();
    dcs->fifo_out = 0;
    dcs->sound_irq_changed();
}
uint16_t dcs_fifo_status() { return dcs->fifo_status(); }

size_t dcs_take_audio(int16_t *out, size_t max_frames) {
    size_t n = std::min(max_frames * 2, dcs->audio.size());
    n &= ~(size_t)1;
    memcpy(out, dcs->audio.data(), n * 2);
    dcs->audio.erase(dcs->audio.begin(), dcs->audio.begin() + n);
    return n / 2;
}

} // namespace rt
