// Core runtime: guest memory, the hardware register dispatcher, COP0/COP1 helpers and indirect calls.
#include "runtime.h"

#include <algorithm>
#include <chrono>
#include <cstdarg>
#include <cstdio>
#include <cstdlib>
#include <intrin.h>
#include <map>
#include <vector>

uint8_t *rt_ram;

extern "C" uint32_t rt_rd_slow(uint32_t a, int size);
extern "C" void rt_wr_slow(uint32_t a, uint32_t v, int size);

namespace rt {

static std::vector<uint8_t> boot_rom;
static std::vector<Device *> devices;
static auto start_time = std::chrono::steady_clock::now();
bool trace_mmio = getenv("ARC_TRACE_MMIO") != nullptr;

void log(const char *fmt, ...) {
    va_list ap;
    va_start(ap, fmt);
    vfprintf(stderr, fmt, ap);
    va_end(ap);
    fputc('\n', stderr);
    fflush(stderr);
}

[[noreturn]] void fatal(const char *fmt, ...) {
    va_list ap;
    va_start(ap, fmt);
    fprintf(stderr, "FATAL: ");
    vfprintf(stderr, fmt, ap);
    va_end(ap);
    fputc('\n', stderr);
    fflush(stderr);
    exit(1);
}

uint64_t now_ns() {
    return (uint64_t)std::chrono::duration_cast<std::chrono::nanoseconds>(std::chrono::steady_clock::now() - start_time).count();
}

void add_device(Device *d) { devices.push_back(d); }

void update_devices() {
    uint64_t now = now_ns();
    for (Device *d : devices) d->update(now);
}

uint32_t phys_read32(uint32_t phys) { return rt_rd_slow(phys | 0xA0000000u, 4); }
void phys_write32(uint32_t phys, uint32_t v) { rt_wr_slow(phys | 0xA0000000u, v, 4); }

void set_boot_rom(std::vector<uint8_t> data) { boot_rom = std::move(data); }

const char *func_name(uint32_t addr) {
    const FuncEntry *e = std::upper_bound(rt_func_table, rt_func_table + rt_func_count, addr,
                                          [](uint32_t a, const FuncEntry &f) { return a < f.addr; });
    if (e == rt_func_table) return "?";
    return e[-1].name[0] ? e[-1].name : "?";
}

static Device *find_device(uint32_t phys) {
    for (Device *d : devices)
        if (phys >= d->base && phys < d->base + d->size) return d;
    return nullptr;
}

// Unclaimed addresses: log each one the first few times.
void dump_trace(int n);
static std::map<uint32_t, int> unknown_hits;
static void unknown(const char *what, uint32_t a, uint64_t v, int size) {
    int &n = unknown_hits[a];
    if (n++ < 4) log("unmapped %s%d %08x = %llx", what, size * 8, a, (unsigned long long)v);
    static int stacks;
    if (n == 1 && stacks++ < 6) print_native_stack();
    static bool traced;
    if (!traced && what[0] == 'w') {
        traced = true;
        dump_trace(300);
    }
}

// ---- TLB (R5000: 48 entries, each mapping an even/odd page pair) ----

struct TlbEntry {
    uint32_t mask, hi, lo0, lo1;
};
static TlbEntry tlb[48];

static bool tlb_translate(uint32_t a, uint32_t &phys) {
    for (const TlbEntry &e : tlb) {
        uint32_t pagemask = e.mask | 0x1FFF;          // covers the page pair
        if (((a ^ e.hi) & ~pagemask) != 0) continue;  // VPN2 (ASIDs not compared: the game uses global entries)
        uint32_t page = (pagemask >> 1) + 1;           // bytes per page
        uint32_t lo = (a & page) ? e.lo1 : e.lo0;
        if (!(lo & 2)) continue;                       // not valid
        phys = ((lo >> 6) << 12 & ~(page - 1)) | (a & (page - 1));
        return true;
    }
    return false;
}

static uint32_t to_phys(uint32_t a) {
    if (a >= 0x80000000u && a < 0xC0000000u) return a & 0x1FFFFFFFu;
    uint32_t p;
    if (tlb_translate(a, p)) return p;
    return 0xFFFFFFFFu; // unmapped
}

} // namespace rt

using namespace rt;

extern "C" uint32_t rt_rd_slow(uint32_t a, int size) {
    uint32_t p = to_phys(a), v = 0;
    if (p < RAM_SIZE && p + size <= RAM_SIZE) {
        memcpy(&v, rt_ram + p, size);
        return v;
    }
    if (p >= 0x1FC00000u && p - 0x1FC00000u + size <= boot_rom.size()) {
        memcpy(&v, boot_rom.data() + (p - 0x1FC00000u), size);
        return v;
    }
    if (Device *d = find_device(p)) {
        v = d->read(p - d->base, size);
        if (trace_mmio) log("%s rd%d %08x -> %08x", d->name, size * 8, p, v);
        return v;
    }
    unknown("read", a, 0, size);
    return 0;
}

extern "C" uint64_t rt_rd64_slow(uint32_t a) {
    return rt_rd_slow(a, 4) | ((uint64_t)rt_rd_slow(a + 4, 4) << 32);
}

extern "C" void rt_wr_slow(uint32_t a, uint32_t v, int size) {
    uint32_t p = to_phys(a);
    if (p < RAM_SIZE && p + size <= RAM_SIZE) {
        memcpy(rt_ram + p, &v, size);
        return;
    }
    if (Device *d = find_device(p)) {
        if (trace_mmio) log("%s wr%d %08x <- %08x", d->name, size * 8, p, v);
        d->write(p - d->base, v, size);
        return;
    }
    unknown("write", a, v, size);
}

extern "C" void rt_wr64_slow(uint32_t a, uint64_t v) {
    rt_wr_slow(a, (uint32_t)v, 4);
    rt_wr_slow(a + 4, (uint32_t)(v >> 32), 4);
}

// ---- 64-bit multiply / divide ----

extern "C" void rt_dmult(Ctx *c, uint64_t a, uint64_t b) {
    int64_t hi;
    c->lo = (uint64_t)_mul128((int64_t)a, (int64_t)b, &hi);
    c->hi = (uint64_t)hi;
}
extern "C" void rt_dmultu(Ctx *c, uint64_t a, uint64_t b) {
    uint64_t hi;
    c->lo = _umul128(a, b, &hi);
    c->hi = hi;
}
extern "C" void rt_ddiv(Ctx *c, uint64_t a, uint64_t b) {
    int64_t x = (int64_t)a, y = (int64_t)b;
    if (y == 0) { c->lo = x >= 0 ? ~0ull : 1; c->hi = a; return; }
    if (x == INT64_MIN && y == -1) { c->lo = a; c->hi = 0; return; }
    c->lo = (uint64_t)(x / y);
    c->hi = (uint64_t)(x % y);
}
extern "C" void rt_ddivu(Ctx *c, uint64_t a, uint64_t b) {
    if (b == 0) { c->lo = ~0ull; c->hi = a; return; }
    c->lo = a / b;
    c->hi = a % b;
}

// ---- FPU ----

static double rnd(Ctx *c, double v) {
    switch (c->fcr31 & 3) {
    case 0: return nearbyint(v);
    case 1: return trunc(v);
    case 2: return ceil(v);
    default: return floor(v);
    }
}
static int32_t to_w(double v) {
    if (!(v >= -2147483648.0 && v <= 2147483647.0)) return INT32_MAX;
    return (int32_t)v;
}
static int64_t to_l(double v) {
    if (!(v >= -9.2233720368547758e18 && v < 9.2233720368547758e18)) return INT64_MAX;
    return (int64_t)v;
}
extern "C" int32_t rt_rint_w(Ctx *c, double v) { return to_w(rnd(c, v)); }
extern "C" int64_t rt_rint_l(Ctx *c, double v) { return to_l(rnd(c, v)); }
extern "C" int32_t rt_round_w(Ctx *, double v) { return to_w(nearbyint(v)); }
extern "C" int64_t rt_round_l(Ctx *, double v) { return to_l(nearbyint(v)); }
extern "C" int32_t rt_trunc_w(Ctx *, double v) { return to_w(trunc(v)); }
extern "C" int64_t rt_trunc_l(Ctx *, double v) { return to_l(trunc(v)); }
extern "C" int32_t rt_ceil_w(Ctx *, double v) { return to_w(ceil(v)); }
extern "C" int64_t rt_ceil_l(Ctx *, double v) { return to_l(ceil(v)); }
extern "C" int32_t rt_floor_w(Ctx *, double v) { return to_w(floor(v)); }
extern "C" int64_t rt_floor_l(Ctx *, double v) { return to_l(floor(v)); }

extern "C" uint32_t rt_cfc1(Ctx *c, int reg) {
    if (reg == 31) return c->fcr31;
    if (reg == 0) return 0x2320; // R5000 FPU implementation/revision
    return 0;
}
extern "C" void rt_ctc1(Ctx *c, int reg, uint32_t v) {
    if (reg == 31) c->fcr31 = v;
}

// ---- COP0 ----

// R5000 Count runs at half the pipeline clock (150 MHz).
static const uint64_t COUNT_HZ = 75000000;
static uint64_t count_base_ns;
static uint32_t count_base;

static uint32_t count_now() {
    return count_base + (uint32_t)((now_ns() - count_base_ns) * COUNT_HZ / 1000000000ull);
}

extern "C" uint32_t rt_mfc0(Ctx *c, int reg) {
    switch (reg) {
    case COP0_COUNT: return count_now();
    case COP0_CAUSE: return c->cop0[COP0_CAUSE] | rt::pending_ip();
    case COP0_PRID: return 0x2320; // R5000
    default: return c->cop0[reg];
    }
}

extern "C" void rt_mtc0(Ctx *c, int reg, uint32_t v) {
    switch (reg) {
    case COP0_COUNT:
        count_base_ns = now_ns();
        count_base = v;
        break;
    case COP0_COMPARE:
        c->cop0[reg] = v;
        rt::compare_written(c);
        break;
    case COP0_CAUSE:
        c->cop0[reg] = (c->cop0[reg] & ~0x300u) | (v & 0x300u); // only the software interrupt bits
        break;
    default:
        c->cop0[reg] = v;
        break;
    }
    if (reg == COP0_STATUS || reg == COP0_CAUSE) c->budget = 0; // interrupts may now be deliverable
}

extern "C" void rt_tlb(Ctx *c, uint32_t pc) {
    uint32_t op = rd32(pc) & 63; // 1 tlbr, 2 tlbwi, 6 tlbwr, 8 tlbp
    switch (op) {
    case 2:
    case 6: {
        int i = op == 2 ? (c->cop0[0] & 63) % 48 : (int)(count_now() % 48);
        tlb[i] = {c->cop0[5] & 0x01FFE000, c->cop0[10], c->cop0[2], c->cop0[3]};
        log("TLB[%d] = mask %08x hi %08x lo0 %08x lo1 %08x (%s)", i, tlb[i].mask, tlb[i].hi, tlb[i].lo0, tlb[i].lo1, func_name(pc));
        break;
    }
    case 1: {
        TlbEntry &e = tlb[(c->cop0[0] & 63) % 48];
        c->cop0[5] = e.mask;
        c->cop0[10] = e.hi;
        c->cop0[2] = e.lo0;
        c->cop0[3] = e.lo1;
        break;
    }
    case 8: {
        c->cop0[0] = 0x80000000u;
        for (int i = 0; i < 48; i++)
            if (((c->cop0[10] ^ tlb[i].hi) & ~(tlb[i].mask | 0x1FFF)) == 0) c->cop0[0] = i;
        break;
    }
    }
}
extern "C" void rt_eret(Ctx *c) { c->cop0[COP0_STATUS] &= ~2u; }
extern "C" void rt_wait(Ctx *c) { c->budget = 0; }
extern "C" void rt_syscall(Ctx *, uint32_t pc) { fatal("syscall at %08x (%s)", pc, func_name(pc)); }
extern "C" void rt_break(Ctx *c, uint32_t pc, uint32_t code) {
    log("break %x at %08x (%s), ra %08x", code, pc, func_name(pc), (uint32_t)c->r[31]);
}
extern "C" void rt_trap(Ctx *c, uint32_t pc) {
    fatal("trap at %08x (%s), ra %08x", pc, func_name(pc), (uint32_t)c->r[31]);
}
extern "C" void rt_unreachable(Ctx *, uint32_t pc) { fatal("reached unexplored code at %08x (%s)", pc, func_name(pc)); }

// ---- indirect calls ----

static GuestFunc lookup(uint32_t addr) {
    if (addr >= 0xA0000000u && addr < 0xA0800000u) addr -= 0x20000000u; // uncached alias
    const FuncEntry *e = std::lower_bound(rt_func_table, rt_func_table + rt_func_count, addr,
                                          [](const FuncEntry &f, uint32_t a) { return f.addr < a; });
    if (e != rt_func_table + rt_func_count && e->addr == addr) return e->func;
    return nullptr;
}

extern "C" void rt_call(Ctx *c, uint32_t addr) {
    GuestFunc f = lookup(addr);
    if (!f) fatal("indirect call to unknown function %08x, ra %08x (%s)", addr, (uint32_t)c->r[31], func_name((uint32_t)c->r[31]));
    f(c);
}

extern "C" void rt_jump(Ctx *c, uint32_t addr, uint32_t from) {
    GuestFunc f = lookup(addr);
    if (!f) fatal("indirect jump from %08x (%s) to unknown address %08x", from, func_name(from), addr);
    f(c);
}

void rt::call(Ctx *c, uint32_t addr) { rt_call(c, addr); }

extern "C" void rt_guest_message(Ctx *c, const char *what, uint32_t addr) {
    char buf[512];
    int n = 0;
    while (n < 511) {
        char ch = (char)rd8(addr + n);
        if (!ch) break;
        buf[n++] = ch;
    }
    buf[n] = 0;
    log("%s: %s (ra %08x %s)", what, buf, (uint32_t)c->r[31], func_name((uint32_t)c->r[31]));
    if (strstr(buf, "rror")) dump_trace(20000);
}

// ---- call tracing ----

int rt_trace_on = getenv("ARC_CALLTRACE") != nullptr;
static struct {
    uint32_t func, ra, a0, a1;
} trace_ring[65536];
static unsigned trace_pos;

namespace rt { extern int depth; }

extern "C" void rt_trace(Ctx *c, uint32_t addr) {
    trace_ring[trace_pos++ & 65535] = {addr, (uint32_t)c->r[31], (uint32_t)c->r[4], (uint32_t)c->r[5]};
}

void rt::dump_trace(int n) {
    if (!rt_trace_on) return;
    static int dumps;
    char path[64];
    snprintf(path, sizeof(path), "trace_dump%d.txt", dumps++);
    FILE *out = fopen(path, "w");
    if (!out) return;
    log("call trace written to %s", path);
    // Leave out the timer interrupt's housekeeping.
    static const char *noise[] = {"prc_set_ipl", "prc_get_count", "prc_get_cause", "prc_get_ipl", "tq_maint", "tq_ins",
                                  "tq_del", "prc_q_action", "prc_add_q", "timer_pll", "process_action_q", "pot_timer",
                                  "ReadPot", "process_ast_q", "process_fg_q", "returnToUser", "except", "normal_exception",
                                  "exception_handler", "interrupt_handler", "galileoint", "cputimerint", "prc_timer_jobs",
                                  "pot_linear", "DoFeedback", "SetForce", "new_check", "cn_irq", "prc_get_actlvl"};
    fprintf(out, "last calls (of %d):\n", n);
    for (int i = n; i > 0; i--) {
        auto &e = trace_ring[(trace_pos - i) & 65535];
        const char *name = func_name(e.func);
        bool skip = false;
        for (const char *q : noise) skip |= !strcmp(name, q);
        if (!skip) fprintf(out, "  %08x %-28s ra %08x a0 %08x a1 %08x\n", e.func, name, e.ra, e.a0, e.a1);
    }
    fclose(out);
}
