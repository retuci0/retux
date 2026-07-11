#pragma once

#include "lib/types.hpp"


// default FPU/SSE state template for the per-task FXSAVE area
// (Task::fpu_state). boot.asm enables SSE for ring 3; switch.asm swaps the
// state per task.

namespace fpu {

    // capture the real post-fninit FXSAVE image once at boot. fresh tasks seed
    // fpu_state from this, not zero - an all-zero image unmasks every FP
    // exception and would fault on the first denormal.
    void init();

    // the 512-byte default-state template captured by init().
    const u8* default_state();

}
