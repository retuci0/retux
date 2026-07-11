#include "cpu/fpu.hpp"

#include "lib/types.hpp"


namespace {

    // FXSAVE area is 512 bytes and must be 16-byte aligned or the
    // instruction #GPs. `alignas` on a static works fine here (unlike
    // `Task::fpu_state`, which is heap-allocated - kmalloc only promises
    // 8-byte alignment, see task.cpp).
    alignas(16) u8 g_default_state[512];

}

namespace fpu {

    void init() {
        asm volatile("fninit");
        asm volatile("fxsave %0" : "=m"(g_default_state));
    }

    const u8* default_state() {
        return g_default_state;
    }

}
