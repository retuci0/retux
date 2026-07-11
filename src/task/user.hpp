#pragma once

#include "task/task.hpp"

#include "cpu/syscall.hpp"

#include "lib/types.hpp"


// helpers for getting into ring 3.

namespace task::user {

    // spawn a task that iretqs into the user_test.asm payload (copied into a
    // fresh USER code page, with a USER stack). call after sched/syscall init.
    void spawn_test();

    // spawn a task in its own address space that loads a static/non-PIE Linux
    // (musl) ELF at `path`, builds its initial stack, and drops into ring 3.
    // safe to run several concurrently - each has its own page tables, so the
    // shared fixed addresses (code at 0x400000) don't collide.
    //
    // ELF loading happens lazily in the task's entry fn (mappings must land
    // under the task's own CR3), so a bad path isn't caught here: returns null
    // only on OOM; a failed load just logs and exits the task.
    task::Task* spawn_from_elf(const char* path);

    // fork(): clone the caller into a new task - private copy of the address
    // space (clone_address_space), shallow copy of fds[], same brk/mmap/TLS.
    // `f` is the caller's syscall frame; the child resumes as if returning from
    // the same syscall via fork_enter_ring3 (task/fork_entry.asm), not the
    // normal dispatch path.
    //
    // returns the child pid to the parent, or -errno; the child never sees
    // this - its rax=0 is forced by fork_enter_ring3.
    i64 sys_fork(const syscall::Frame* f);

    // execve(path, argv, envp): replaces the caller's image in place (full
    // contract in task/user.cpp). trusted USER pointers, copied into the kernel
    // before the address space changes. returns -errno on failure (caller runs
    // on, unchanged); doesn't return on success.
    i64 sys_execve(const char* path, char* const* argv, char* const* envp);

}
