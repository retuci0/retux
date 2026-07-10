#pragma once

#include "lib/types.hpp"


// minimal ELF64 loader. reads a static, non-PIE x86-64 executable off the
// VFS and maps its PT_LOAD segments into the (single, shared) address
// space with per-segment permissions - no relocation, no dynamic linking,
// no auxv. good enough for hand-built userspace test binaries; a real libc
// would need PIE (ET_DYN) support this doesn't have.

namespace elf {

    // everything a caller needs to both jump into the binary AND build a
    // correct Linux-ABI initial stack for it (see `task::user::spawn_from_elf`
    // in `task/user.cpp`) - `phdr_vaddr`/`phnum`/`phentsize` feed AT_PHDR/
    // AT_PHNUM/AT_PHENT, which musl's `__init_tls` needs to find the PT_TLS
    // segment (if any) inside the already-mapped image.
    struct LoadResult {
        u64 entry;        // e_entry
        u64 phdr_vaddr;   // vaddr the program header table itself landed at
        u64 phnum;        // e_phnum
        u64 phentsize;    // e_phentsize
        u64 highest_addr; // page-aligned end of the highest PT_LOAD segment -
                           // where a Linux-ABI `brk` region should start
    };

    // opens `path`, validates the Ehdr (64-bit LE x86-64 ET_EXEC), and maps
    // every PT_LOAD Phdr's pages as PRESENT|USER with WRITABLE/NO_EXECUTE
    // derived from p_flags - file bytes are copied in and the p_memsz -
    // p_filesz tail is left zeroed (bss). on success returns true and fills
    // *out; on any failure (missing file, bad magic/class/machine/type,
    // truncated program headers, OOM) returns false and *out is untouched.
    bool load(const char* path, LoadResult* out);

}
