#pragma once

#include "lib/types.hpp"


// helpers for getting into ring 3. Landing A only exposes a
// spawn-the-embedded-test-blob function - once the ELF loader lands, this
// grows a real `spawn_from_elf(path, ...)` companion.

namespace task::user {

    // allocate a fresh USER page for code + a USER page for a stack,
    // copy the embedded ring-3 payload from `user_test.asm` into the
    // code page, and spawn a task whose entry function `iretq`s into it.
    // safe to call any time after `sched::init()` + `syscall::init()`.
    void spawn_test();

}
