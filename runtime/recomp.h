// Interface between the recompiled code (generated/, C) and the runtime.
#pragma once
#include <math.h>
#include <stdint.h>
#include <string.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef union {
    uint64_t u;
    int64_t l;
    double d;
    uint32_t w;
    int32_t i;
    float f;
} Fpr;

typedef struct Ctx {
    uint64_t r[32];
    uint64_t hi, lo;
    Fpr f[32];             // Status.FR = 1: 32 64-bit registers
    uint32_t fcr31;
    uint32_t cop0[32];
    int64_t budget;        // instructions-ish until the next rt_service()
} Ctx;

typedef void (*GuestFunc)(Ctx *c);
typedef struct {
    uint32_t addr;
    GuestFunc func;
    const char *name;
} FuncEntry;
extern const FuncEntry rt_func_table[];
extern const int rt_func_count;

#define RAM_SIZE 0x800000u
extern uint8_t *rt_ram;

#if defined(__clang__) || defined(__GNUC__)
#define LIKELY(x) __builtin_expect(!!(x), 1)
#define UNLIKELY(x) __builtin_expect(!!(x), 0)
#else
#define LIKELY(x) (x)
#define UNLIKELY(x) (x)
#endif

#define S32(x) ((uint64_t)(int64_t)(int32_t)(uint32_t)(x))
#define S16(x) ((uint64_t)(int64_t)(int16_t)(uint16_t)(x))
#define S8(x) ((uint64_t)(int64_t)(int8_t)(uint8_t)(x))

// Slow paths: uncached RAM, boot ROM and hardware registers.
uint32_t rt_rd_slow(uint32_t a, int size);
uint64_t rt_rd64_slow(uint32_t a);
void rt_wr_slow(uint32_t a, uint32_t v, int size);
void rt_wr64_slow(uint32_t a, uint64_t v);

// KSEG0 (cached) RAM is the fast path.
#define RAM_OFF(a) ((uint32_t)(a) - 0x80000000u)
static inline uint32_t rd8(uint32_t a) {
    uint32_t o = RAM_OFF(a);
    return LIKELY(o < RAM_SIZE) ? rt_ram[o] : rt_rd_slow(a, 1);
}
static inline uint32_t rd16(uint32_t a) {
    uint32_t o = RAM_OFF(a), v;
    if (LIKELY(o < RAM_SIZE)) { memcpy(&v, rt_ram + o, 2); return v & 0xFFFF; }
    return rt_rd_slow(a, 2);
}
static inline uint32_t rd32(uint32_t a) {
    uint32_t o = RAM_OFF(a), v;
    if (LIKELY(o < RAM_SIZE)) { memcpy(&v, rt_ram + o, 4); return v; }
    return rt_rd_slow(a, 4);
}
static inline uint64_t rd64(uint32_t a) {
    uint32_t o = RAM_OFF(a);
    uint64_t v;
    if (LIKELY(o < RAM_SIZE)) { memcpy(&v, rt_ram + o, 8); return v; }
    return rt_rd64_slow(a);
}
static inline void wr8(uint32_t a, uint8_t v) {
    uint32_t o = RAM_OFF(a);
    if (LIKELY(o < RAM_SIZE)) rt_ram[o] = v; else rt_wr_slow(a, v, 1);
}
static inline void wr16(uint32_t a, uint16_t v) {
    uint32_t o = RAM_OFF(a);
    if (LIKELY(o < RAM_SIZE)) memcpy(rt_ram + o, &v, 2); else rt_wr_slow(a, v, 2);
}
static inline void wr32(uint32_t a, uint32_t v) {
    uint32_t o = RAM_OFF(a);
    if (LIKELY(o < RAM_SIZE)) memcpy(rt_ram + o, &v, 4); else rt_wr_slow(a, v, 4);
}
static inline void wr64(uint32_t a, uint64_t v) {
    uint32_t o = RAM_OFF(a);
    if (LIKELY(o < RAM_SIZE)) memcpy(rt_ram + o, &v, 8); else rt_wr64_slow(a, v);
}

// Unaligned loads and stores, little-endian semantics.
static inline uint64_t rt_lwl(uint32_t a, uint64_t rt) {
    uint32_t b = a & 3, w = rd32(a & ~3u), sh = 24 - 8 * b;
    uint32_t keep = sh ? ((1u << sh) - 1) : 0;
    return S32((w << sh) | ((uint32_t)rt & keep));
}
static inline uint64_t rt_lwr(uint32_t a, uint64_t rt) {
    uint32_t b = a & 3, w = rd32(a & ~3u), sh = 8 * b;
    uint32_t keep = b ? ~(0xFFFFFFFFu >> sh) : 0;
    return S32((w >> sh) | ((uint32_t)rt & keep));
}
static inline uint64_t rt_ldl(uint32_t a, uint64_t rt) {
    uint32_t b = a & 7, sh = 56 - 8 * b;
    uint64_t d = rd64(a & ~7u), keep = sh ? ((1ull << sh) - 1) : 0;
    return (d << sh) | (rt & keep);
}
static inline uint64_t rt_ldr(uint32_t a, uint64_t rt) {
    uint32_t b = a & 7, sh = 8 * b;
    uint64_t d = rd64(a & ~7u), keep = b ? ~(~0ull >> sh) : 0;
    return (d >> sh) | (rt & keep);
}
static inline void rt_swl(uint32_t a, uint64_t rt) {
    uint32_t b = a & 3, sh = 24 - 8 * b, w = rd32(a & ~3u);
    uint32_t m = 0xFFFFFFFFu >> sh;
    wr32(a & ~3u, (w & ~m) | ((uint32_t)rt >> sh));
}
static inline void rt_swr(uint32_t a, uint64_t rt) {
    uint32_t b = a & 3, sh = 8 * b, w = rd32(a & ~3u);
    uint32_t m = 0xFFFFFFFFu << sh;
    wr32(a & ~3u, (w & ~m) | ((uint32_t)rt << sh));
}
static inline void rt_sdl(uint32_t a, uint64_t rt) {
    uint32_t b = a & 7, sh = 56 - 8 * b;
    uint64_t d = rd64(a & ~7u), m = ~0ull >> sh;
    wr64(a & ~7u, (d & ~m) | (rt >> sh));
}
static inline void rt_sdr(uint32_t a, uint64_t rt) {
    uint32_t b = a & 7, sh = 8 * b;
    uint64_t d = rd64(a & ~7u), m = ~0ull << sh;
    wr64(a & ~7u, (d & ~m) | (rt << sh));
}

// Multiply and divide.
static inline void MULT(Ctx *c, uint64_t a, uint64_t b) {
    int64_t p = (int64_t)(int32_t)a * (int64_t)(int32_t)b;
    c->lo = S32(p);
    c->hi = S32((uint64_t)p >> 32);
}
static inline void MULTU(Ctx *c, uint64_t a, uint64_t b) {
    uint64_t p = (uint64_t)(uint32_t)a * (uint64_t)(uint32_t)b;
    c->lo = S32(p);
    c->hi = S32(p >> 32);
}
static inline void DIV(Ctx *c, uint64_t a, uint64_t b) {
    int32_t x = (int32_t)a, y = (int32_t)b;
    if (y == 0) { c->lo = S32(x >= 0 ? -1 : 1); c->hi = S32(x); return; }
    if (x == INT32_MIN && y == -1) { c->lo = S32(x); c->hi = 0; return; }
    c->lo = S32(x / y);
    c->hi = S32(x % y);
}
static inline void DIVU(Ctx *c, uint64_t a, uint64_t b) {
    uint32_t x = (uint32_t)a, y = (uint32_t)b;
    if (y == 0) { c->lo = S32(0xFFFFFFFFu); c->hi = S32(x); return; }
    c->lo = S32(x / y);
    c->hi = S32(x % y);
}
void rt_dmult(Ctx *c, uint64_t a, uint64_t b);
void rt_dmultu(Ctx *c, uint64_t a, uint64_t b);
void rt_ddiv(Ctx *c, uint64_t a, uint64_t b);
void rt_ddivu(Ctx *c, uint64_t a, uint64_t b);

// FPU condition codes: cc 0 is bit 23 of FCSR, cc 1..7 are bits 25..31.
#define FCC_BIT(n) ((n) == 0 ? 23 : 24 + (n))
#define FCC(n) ((c->fcr31 >> FCC_BIT(n)) & 1)
#define SET_FCC(c, n, v) ((c)->fcr31 = ((c)->fcr31 & ~(1u << FCC_BIT(n))) | ((uint32_t)((v) ? 1 : 0) << FCC_BIT(n)))
#define UNORD(a, b) (isnan(a) || isnan(b))

int32_t rt_rint_w(Ctx *c, double v);
int64_t rt_rint_l(Ctx *c, double v);
int32_t rt_round_w(Ctx *c, double v);
int64_t rt_round_l(Ctx *c, double v);
int32_t rt_trunc_w(Ctx *c, double v);
int64_t rt_trunc_l(Ctx *c, double v);
int32_t rt_ceil_w(Ctx *c, double v);
int64_t rt_ceil_l(Ctx *c, double v);
int32_t rt_floor_w(Ctx *c, double v);
int64_t rt_floor_l(Ctx *c, double v);
uint32_t rt_cfc1(Ctx *c, int reg);
void rt_ctc1(Ctx *c, int reg, uint32_t v);

// System control.
uint32_t rt_mfc0(Ctx *c, int reg);
void rt_mtc0(Ctx *c, int reg, uint32_t v);
void rt_tlb(Ctx *c, uint32_t pc);
void rt_eret(Ctx *c);
void rt_wait(Ctx *c);
void rt_syscall(Ctx *c, uint32_t pc);
void rt_break(Ctx *c, uint32_t pc, uint32_t code);
void rt_trap(Ctx *c, uint32_t pc);
void rt_unreachable(Ctx *c, uint32_t pc);

// Call tracing (ARC_CALLTRACE=1): every function entry goes into a ring buffer.
extern int rt_trace_on;
void rt_trace(Ctx *c, uint32_t addr);
#define TRACE_ENTRY(c, addr) do { if (UNLIKELY(rt_trace_on)) rt_trace(c, addr); } while (0)

// Debugging: print a guest NUL-terminated string.
void rt_guest_message(Ctx *c, const char *what, uint32_t addr);
void rt_game_message(Ctx *c, uint32_t addr);

// Widescreen (recomp.toml hook at the end of mb_setView).
void rt_widescreen_view(Ctx *c);

// Indirect calls and jumps.
void rt_call(Ctx *c, uint32_t addr);
void rt_jump(Ctx *c, uint32_t addr, uint32_t from);

// Interrupts and timing: called at loop back-edges when the budget runs out.
// pc: the loop's address, for the watchdog.
void rt_service(Ctx *c, uint32_t pc);
#define CHECK(c, pc) do { if (UNLIKELY(--(c)->budget < 0)) rt_service(c, pc); } while (0)

#ifdef __cplusplus
}
#endif
