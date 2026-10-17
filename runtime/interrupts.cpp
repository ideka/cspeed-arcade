// Interrupt delivery. The recompiled code cannot be interrupted at an arbitrary instruction, so the
// guest's exception handler is called synchronously at loop back-edges (rt_service), when the guest's
// Status register allows it. All guest state is in Ctx at those points, which is what the handler saves
// and restores.
#include "runtime.h"

namespace rt {

extern int depth;
static uint32_t irq_lines; // IP2..IP6 (bit 0 = IP2)
static bool timer_irq;     // IP7: Count == Compare
static uint32_t last_count;
static const uint32_t EXCEPT_HANDLER = 0x80011180; // `except`, which the general exception vector jumps to
int depth;

void set_irq(int line, bool on) {
    if (on) irq_lines |= 1u << line;
    else irq_lines &= ~(1u << line);
}

uint32_t pending_ip() { return (irq_lines << 10) | (timer_irq ? 0x8000u : 0); }

void compare_written(Ctx *c) {
    timer_irq = false;
    last_count = rt_mfc0(c, COP0_COUNT);
}

static void check_timer(Ctx *c) {
    uint32_t now = rt_mfc0(c, COP0_COUNT);
    uint32_t cmp = c->cop0[COP0_COMPARE];
    // Did Count pass Compare since the last check?
    if (cmp - last_count <= now - last_count && cmp != last_count) timer_irq = true;
    last_count = now;
}

void deliver_interrupts(Ctx *c) {
    check_timer(c);
    uint32_t sr = c->cop0[COP0_STATUS];
    if (!(sr & 1) || (sr & 6)) return; // IE off, or already in an exception (EXL/ERL)
    uint32_t cause = c->cop0[COP0_CAUSE] | pending_ip();
    if (!(cause & sr & 0xFF00)) return;
    if (depth > 4) return;
    depth++;
    c->cop0[COP0_CAUSE] = (c->cop0[COP0_CAUSE] & ~0x7Cu); // ExcCode 0: interrupt
    c->cop0[COP0_EPC] = 0x80000400;                        // not resumable by address; the handler returns
    c->cop0[COP0_STATUS] |= 2;                             // EXL
    call(c, EXCEPT_HANDLER);
    c->cop0[COP0_STATUS] &= ~2u;
    depth--;
}

} // namespace rt

volatile uint32_t rt_last_loop_pc;

extern "C" void rt_service(Ctx *c, uint32_t pc) {
    rt_last_loop_pc = pc;
    c->budget = 2000;
    rt::update_devices();
    rt::deliver_interrupts(c);
}
