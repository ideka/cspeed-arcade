// license:BSD-3-Clause
// copyright-holders:Aaron Giles
//
// ADSP-21xx core from MAME (src/devices/cpu/adsp2100), reduced to a standalone class for the
// ADSP-2115 of the DCS2 sound board: no device framework, memory through a Bus interface.
// adsp21xx.cpp and 2100ops.hxx are MAME's code with the framework calls replaced.
#pragma once

#include <cstdint>
#include <cstring>
#include <type_traits>

namespace adsp {

constexpr int CLEAR_LINE = 0, ASSERT_LINE = 1;

// ADSP-2115 interrupt lines
constexpr int ADSP2115_IRQ0 = 0, ADSP2115_SPORT1_RX = 0, ADSP2115_IRQ1 = 1, ADSP2115_SPORT1_TX = 1,
              ADSP2115_IRQ2 = 2, ADSP2115_SPORT0_RX = 3, ADSP2115_SPORT0_TX = 4, ADSP2115_TIMER = 5;
constexpr int ADSP2101_IRQ0 = 0, ADSP2101_SPORT1_RX = 0, ADSP2101_IRQ1 = 1, ADSP2101_SPORT1_TX = 1,
              ADSP2101_IRQ2 = 2, ADSP2101_SPORT0_RX = 3, ADSP2101_SPORT0_TX = 4, ADSP2101_TIMER = 5;

constexpr int ADSP2181_IRQ0 = 0, ADSP2181_SPORT1_RX = 0, ADSP2181_IRQ1 = 1, ADSP2181_SPORT1_TX = 1,
              ADSP2181_IRQ2 = 2, ADSP2181_SPORT0_RX = 3, ADSP2181_SPORT0_TX = 4, ADSP2181_TIMER = 5,
              ADSP2181_IRQE = 6, ADSP2181_BDMA = 7, ADSP2181_IRQL1 = 8, ADSP2181_IRQL0 = 9;

using u8 = uint8_t;
using u16 = uint16_t;
using u32 = uint32_t;
using u64 = uint64_t;
using s8 = int8_t;
using s16 = int16_t;
using s32 = int32_t;
using s64 = int64_t;

namespace util {
template <typename T> constexpr T sext(T value, unsigned bits) {
    using S = std::make_signed_t<T>;
    return T(S(value << (sizeof(T) * 8 - bits)) >> (sizeof(T) * 8 - bits));
}
} // namespace util

// Memory as the DSP sees it: 24-bit program words, 16-bit data words, 14-bit addresses.
struct Bus {
    virtual ~Bus() = default;
    virtual uint32_t program_read(uint32_t addr) = 0;
    virtual void program_write(uint32_t addr, uint32_t data) = 0;
    virtual uint16_t data_read(uint32_t addr) = 0;
    virtual void data_write(uint32_t addr, uint16_t data) = 0;
    // Serial port transmit (port 0/1), receive, timer enable changes, DMOVLAY (2181 only).
    virtual void sport_tx(int port, uint32_t data) {}
    virtual uint32_t sport_rx(int port) { return 0; }
    virtual void timer_fired(int state) {}
};

class adsp21xx_device {
public:
    enum { CHIP_TYPE_ADSP2100, CHIP_TYPE_ADSP2101, CHIP_TYPE_ADSP2104, CHIP_TYPE_ADSP2105, CHIP_TYPE_ADSP2115,
           CHIP_TYPE_ADSP2181 };

    adsp21xx_device(Bus *bus, uint32_t chiptype = CHIP_TYPE_ADSP2115);

    void reset();
    // Runs for (at least) the given number of cycles; returns the cycles actually run.
    int run(int cycles);
    void set_input_line(int line, int state);
    void pulse_input_line(int line) {
        set_input_line(line, ASSERT_LINE);
        set_input_line(line, CLEAR_LINE);
    }
    void load_boot_data(uint8_t *srcdata, uint32_t *dstdata);
    uint32_t get_ibase(int index) { return m_base[index]; }
    uint64_t total_cycles() const { return m_total_cycles; }

    // Register access for the board (autobuffer DMA uses I/M/L registers directly).
    uint32_t i_reg(int n) const { return m_i[n]; }
    void set_i_reg(int n, uint32_t v) { m_i[n] = v & 0x3fff; }
    int32_t m_reg(int n) const { return m_m[n]; }
    uint32_t l_reg(int n) const { return m_l[n]; }
    uint32_t pc() const { return m_pc; }

protected:
    // framework replacements used by the MAME code
    struct tx_cb { Bus *&b; void operator()(int port, uint32_t data, uint32_t) { b->sport_tx(port, data); } };
    struct rx_cb { Bus *&b; uint32_t operator()(int port) { return b->sport_rx(port); } };
    struct timer_cb { Bus *&b; void operator()(int state) { b->timer_fired(state); } };
    struct ovl_cb { void operator()(uint32_t) {} };

    void create_tables();
    inline void update_mstat();
    inline uint32_t pc_stack_top();
    inline void set_pc_stack_top(uint32_t top);
    inline void pc_stack_push();
    inline void pc_stack_push_val(uint32_t val);
    inline void pc_stack_pop();
    inline uint32_t pc_stack_pop_val();
    inline uint32_t cntr_stack_top();
    inline void cntr_stack_push();
    inline void cntr_stack_pop();
    inline uint32_t loop_stack_top();
    inline void loop_stack_push(uint32_t value);
    inline void loop_stack_pop();
    inline void stat_stack_push();
    inline void stat_stack_pop();
    int slow_condition();
    inline void modify_address(uint32_t ireg, uint32_t mreg);
    inline void data_write_dag1(uint32_t op, int32_t val);
    inline uint32_t data_read_dag1(uint32_t op);
    inline void data_write_dag2(uint32_t op, int32_t val);
    inline uint32_t data_read_dag2(uint32_t op);
    inline void pgm_write_dag2(uint32_t op, int32_t val);
    inline uint32_t pgm_read_dag2(uint32_t op);
    void alu_op_ar(uint32_t op);
    void alu_op_ar_const(uint32_t op);
    void alu_op_af(uint32_t op);
    void alu_op_af_const(uint32_t op);
    void alu_op_none(uint32_t op);
    void mac_op_mr(uint32_t op);
    void mac_op_mr_xop(uint32_t op);
    void mac_op_mf(uint32_t op);
    void mac_op_mf_xop(uint32_t op);
    void shift_op(uint32_t op);
    void shift_op_imm(uint32_t op);

    inline uint16_t data_read(uint32_t addr) { return m_bus->data_read(addr & 0x3fff); }
    inline void data_write(uint32_t addr, uint16_t data) { m_bus->data_write(addr & 0x3fff, data); }
    inline uint16_t io_read(uint32_t) { return 0; }
    inline void io_write(uint32_t, uint16_t) {}
    inline uint32_t program_read(uint32_t addr) { return m_bus->program_read(addr & 0x3fff); }
    inline void program_write(uint32_t addr, uint32_t data) { m_bus->program_write(addr & 0x3fff, data & 0xffffff); }
    inline uint32_t opcode_read() { return m_bus->program_read(m_pc & 0x3fff); }

    inline void update_i(int which);
    inline void update_l(int which);
    inline void update_dmovlay();
    inline void write_reg0(int regnum, int32_t val);
    inline void write_reg1(int regnum, int32_t val);
    inline void write_reg2(int regnum, int32_t val);
    inline void write_reg3(int regnum, int32_t val);
    inline int32_t read_reg0(int regnum);
    inline int32_t read_reg1(int regnum);
    inline int32_t read_reg2(int regnum);
    inline int32_t read_reg3(int regnum);

    bool generate_irq(int which, int indx = 0);
    void check_irqs();

    static const int PC_STACK_DEPTH = 16;
    static const int CNTR_STACK_DEPTH = 4;
    static const int STAT_STACK_DEPTH = 4;
    static const int LOOP_STACK_DEPTH = 4;

    union adsp_reg16 {
        uint16_t u;
        int16_t s;
    };
    union adsp_shift {
        struct { adsp_reg16 sr0, sr1; } srx;
        uint32_t sr;
    };
    union adsp_mac {
        struct { adsp_reg16 mr0, mr1, mr2, mrzero; } mrx;
        struct { uint32_t mr0, mr1; } mry;
        uint64_t mr;
    };
    struct adsp_core {
        adsp_reg16 ax0, ax1, ay0, ay1, ar, af;
        adsp_reg16 mx0, mx1, my0, my1;
        adsp_mac mr;
        adsp_reg16 mf;
        adsp_reg16 si, se, sb;
        adsp_shift sr;
        adsp_reg16 zero;
    };

    Bus *m_bus;
    uint32_t m_chip_type;
    uint64_t m_total_cycles = 0;

    uint32_t m_pc = 0, m_ppc = 0, m_loop = 0, m_loop_condition = 0, m_cntr = 0;
    uint32_t m_astat = 0, m_sstat = 0, m_mstat = 0, m_mstat_prev = 0, m_astat_clear = 0, m_idle = 0;
    adsp_core m_core;
    uint32_t m_i[8], m_l[8], m_lmask[8], m_base[8];
    int32_t m_m[8];
    uint8_t m_px = 0;
    uint32_t m_pmovlay = 0, m_dmovlay = 0;
    uint32_t m_loop_stack[LOOP_STACK_DEPTH], m_cntr_stack[CNTR_STACK_DEPTH], m_pc_stack[PC_STACK_DEPTH];
    uint16_t m_stat_stack[STAT_STACK_DEPTH][3];
    int32_t m_pc_sp = 0, m_cntr_sp = 0, m_stat_sp = 0, m_loop_sp = 0;
    uint8_t m_flagout = 0, m_flagin = 0, m_fl0 = 0, m_fl1 = 0, m_fl2 = 0;
    uint16_t m_idma_addr = 0, m_idma_cache = 0;
    uint8_t m_idma_offs = 0;
    uint16_t m_imask = 0;
    uint8_t m_icntl = 0;
    uint16_t m_ifc = 0;
    uint8_t m_irq_state[10], m_irq_latch[10];
    int m_icount = 0;
    int m_mstat_mask, m_imask_mask;

    int16_t *m_read0_ptr[16];
    uint32_t *m_read1_ptr[16];
    uint32_t *m_read2_ptr[16];
    void *m_alu_xregs[8];
    void *m_alu_yregs[4];
    void *m_mac_xregs[8];
    void *m_mac_yregs[4];
    void *m_shift_xregs[8];
    adsp_core m_alt;

    uint8_t m_condition_table[0x1000];
    uint16_t m_mask_table[0x4000];
    uint16_t m_reverse_table[0x4000];

    tx_cb m_sport_tx_cb{m_bus};
    rx_cb m_sport_rx_cb{m_bus};
    timer_cb m_timer_fired_cb{m_bus};
    ovl_cb m_dmovlay_cb;

    static const int SSFLAG = 0x80, MVFLAG = 0x40, QFLAG = 0x20, SFLAG = 0x10, CFLAG = 0x08, VFLAG = 0x04,
                     NFLAG = 0x02, ZFLAG = 0x01;
};

} // namespace adsp
