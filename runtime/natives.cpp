// Guest functions replaced by native code (the `skip` list in recomp.toml).
#include "runtime.h"
#include "../generated/funcs.h"

// startup: the image header at the start of the executable, never called.
extern "C" void f_80000000(Ctx *) { rt::fatal("call to the image header (startup)"); }

// UnLock: the I/O ASIC unlock handshake run at boot. v0 = 0: unlocked.
extern "C" void f_80010c68(Ctx *c) { c->r[2] = 0; }


