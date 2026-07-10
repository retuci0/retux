#pragma once

#include "task/task.hpp"

#include "lib/types.hpp"


// helpers for getting into ring 3.

namespace task::user {

    // allocate a fresh USER page for code + a USER page for a stack,
    // copy the embedded ring-3 payload from `user_test.asm` into the
    // code page, and spawn a task whose entry function `iretq`s into it.
    // safe to call any time after `sched::init()` + `syscall::init()`.
    void spawn_test();

    // give the new task its own private address space (`vmm::create_address_
    // space()`) and spawn it with an entry function that loads the ELF
    // executable at `path` (via `elf::load()`), maps a multi-page user
    // stack, builds a Linux-ABI `[argc][argv][envp][auxv]` block on it
    // (argv[0] = `path`, no environment yet), sets up `brk`/`mmap` regions,
    // and `iretq`s into the binary's entry point - i.e. this targets real
    // static/non-PIE Linux binaries (musl), not just hand-built test blobs.
    // safe to spawn more than one of these concurrently - each gets its own
    // page tables, so identical fixed virtual addresses (code at `0x400000`,
    // etc.) across tasks don't collide.
    //
    // unlike the previous version of this function, ELF loading now happens
    // LAZILY inside the task's own entry function (same reason
    // `spawn_test()` already does its setup lazily: every mapping has to
    // happen while the task's own CR3 is actually active, which is only
    // true once the scheduler has switched into it) - so a bad path or
    // corrupt ELF is no longer caught synchronously here. this returns
    // nullptr only on allocation failure (OOM); an ELF that fails to load
    // logs to serial and the task exits immediately instead.
    task::Task* spawn_from_elf(const char* path);

}
