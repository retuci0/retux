#pragma once

#include "lib/types.hpp"


// minimal ELF64 loader: reads a static non-PIE x86-64 executable off the VFS
// and maps its PT_LOAD segments with per-segment permissions. no relocation,
// no dynamic linking, no PIE (ET_DYN).

namespace elf {

    // enough to jump into the binary and build its Linux-ABI stack.
    // phdr_vaddr/phnum/phentsize feed AT_PHDR/AT_PHNUM/AT_PHENT, which musl's
    // __init_tls uses to find PT_TLS.
    struct LoadResult {
        u64 entry;        // e_entry
        u64 phdr_vaddr;   // vaddr the program header table itself landed at
        u64 phnum;        // e_phnum
        u64 phentsize;    // e_phentsize
        u64 highest_addr; // page-aligned end of the highest PT_LOAD segment -
                           // where a Linux-ABI `brk` region should start
    };

    // validate the Ehdr (64-bit LE x86-64 ET_EXEC) and map every PT_LOAD as
    // PRESENT|USER with W/NX from p_flags, bss tail zeroed. true + fills *out
    // on success; false (*out untouched) on any failure.
    bool load(const char* path, LoadResult* out);

}
