// Minimal stand-ins for the MAME utilities the ADSP-21xx disassembler uses.
#pragma once
#include <cstdint>
#include <cstdio>
#include <ostream>
#include <string>
using u8 = uint8_t; using u16 = uint16_t; using u32 = uint32_t; using u64 = uint64_t;
using s8 = int8_t; using s16 = int16_t; using s32 = int32_t; using offs_t = uint32_t;
constexpr u32 BIT(u32 x, int n) { return (x >> n) & 1; }
constexpr u32 BIT(u32 x, int n, int w) { return (x >> n) & ((1u << w) - 1); }
namespace util {
template <typename... A> void stream_format(std::ostream &s, const char *fmt, A... a) {
    char buf[256];
    snprintf(buf, sizeof(buf), fmt, a...);
    s << buf;
}
inline void stream_format(std::ostream &s, const char *fmt) { s << fmt; }
template <typename T> constexpr std::make_signed_t<T> sext(T v, unsigned w) {
    return std::make_signed_t<T>(v << (8 * sizeof(v) - w)) >> (8 * sizeof(v) - w);
}
struct disasm_interface {
    struct data_buffer {
        const u32 *words;
        u32 r32(offs_t pc) const { return words[pc & 0x3fff]; }
    };
    static constexpr u32 SUPPORTED = 0x80000000, STEP_OVER = 0x20000000, STEP_OUT = 0x40000000, STEP_COND = 0x10000000;
    virtual ~disasm_interface() = default;
    virtual u32 opcode_alignment() const = 0;
    virtual offs_t disassemble(std::ostream &stream, offs_t pc, const data_buffer &opcodes, const data_buffer &params) = 0;
};
}
