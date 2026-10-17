// Debugging aid (ARC_WATCH=1): every few seconds, report the guest loop last seen and the native stack
// of the main thread, symbolized from the PDB.
#include "runtime.h"
#include "swrender.h"

#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <dbghelp.h>

#include <chrono>
#include <cstring>
#include <thread>

extern volatile uint32_t rt_last_loop_pc;
namespace rt { Ctx *main_ctx; }

namespace rt {

static void print_stack(HANDLE process, HANDLE thread) {
    SuspendThread(thread);
    CONTEXT ctx{};
    ctx.ContextFlags = CONTEXT_FULL;
    if (GetThreadContext(thread, &ctx)) {
        STACKFRAME64 frame{};
        frame.AddrPC.Offset = ctx.Rip;
        frame.AddrPC.Mode = AddrModeFlat;
        frame.AddrFrame.Offset = ctx.Rbp;
        frame.AddrFrame.Mode = AddrModeFlat;
        frame.AddrStack.Offset = ctx.Rsp;
        frame.AddrStack.Mode = AddrModeFlat;
        for (int i = 0; i < 24; i++) {
            if (!StackWalk64(IMAGE_FILE_MACHINE_AMD64, process, thread, &frame, &ctx, nullptr, SymFunctionTableAccess64,
                             SymGetModuleBase64, nullptr))
                break;
            char buf[sizeof(SYMBOL_INFO) + 256];
            SYMBOL_INFO *sym = (SYMBOL_INFO *)buf;
            sym->SizeOfStruct = sizeof(SYMBOL_INFO);
            sym->MaxNameLen = 255;
            DWORD64 disp = 0;
            if (SymFromAddr(process, frame.AddrPC.Offset, &disp, sym)) {
                const char *guest = "";
                uint32_t addr = 0;
                if (sscanf(sym->Name, "f_%8x", &addr) == 1) guest = func_name(addr);
                log("    %s+0x%llx %s", sym->Name, (unsigned long long)disp, guest);
            } else {
                log("    %016llx", (unsigned long long)frame.AddrPC.Offset);
            }
        }
    }
    ResumeThread(thread);
}

static bool watchdog_running;

// The calling thread's stack, for error reports.
void print_native_stack() {
    static bool init;
    HANDLE process = GetCurrentProcess();
    if (!init && !watchdog_running) {
        SymSetOptions(SYMOPT_UNDNAME | SYMOPT_DEFERRED_LOADS);
        SymInitialize(process, nullptr, TRUE);
        init = true;
    }
    void *frames[32];
    USHORT n = RtlCaptureStackBackTrace(1, 32, frames, nullptr);
    for (USHORT i = 0; i < n; i++) {
        char buf[sizeof(SYMBOL_INFO) + 256];
        SYMBOL_INFO *sym = (SYMBOL_INFO *)buf;
        sym->SizeOfStruct = sizeof(SYMBOL_INFO);
        sym->MaxNameLen = 255;
        DWORD64 disp = 0;
        if (!SymFromAddr(process, (DWORD64)frames[i], &disp, sym)) continue;
        uint32_t addr = 0;
        const char *guest = sscanf(sym->Name, "f_%8x", &addr) == 1 ? func_name(addr) : "";
        log("    %s+0x%llx %s", sym->Name, (unsigned long long)disp, guest);
        if (!strcmp(sym->Name, "main")) break;
    }
}

void watchdog_start() {
    watchdog_running = true;
    HANDLE process = GetCurrentProcess();
    HANDLE main_thread;
    DuplicateHandle(process, GetCurrentThread(), process, &main_thread, 0, FALSE, DUPLICATE_SAME_ACCESS);
    SymSetOptions(SYMOPT_UNDNAME | SYMOPT_DEFERRED_LOADS);
    SymInitialize(process, nullptr, TRUE);
    std::thread([process, main_thread] {
        for (;;) {
            std::this_thread::sleep_for(std::chrono::seconds(3));
            uint32_t pc = rt_last_loop_pc;
            extern Ctx *main_ctx;
            uint32_t irq_lines_debug();
            extern volatile uint64_t swap_count;
            static uint64_t last_swaps;
            log("watch: %.1f frames/s", (swap_count - last_swaps) / 3.0);
            last_swaps = swap_count;
            static uint64_t last_render, last_time;
            uint64_t t = now_ns(), r = sw_render_ns;
            if (last_time) log("watch: rendering took %.0f%% of the time", 100.0 * (r - last_render) / (t - last_time));
            last_render = r;
            last_time = t;
            log("watch: last guest loop %08x (%s), triangles %d, fills %d; irq lines %02x, SR %08x, Cause %08x", pc,
                func_name(pc), sw_counts[1], sw_counts[3], irq_lines_debug(), main_ctx ? main_ctx->cop0[COP0_STATUS] : 0,
                main_ctx ? main_ctx->cop0[COP0_CAUSE] : 0);
            print_stack(process, main_thread);
            static int reports;
            if (++reports == 6) dump_trace(3000);
        }
    }).detach();
}

} // namespace rt
