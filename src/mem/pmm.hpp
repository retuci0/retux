#pragma once

#include "lib/types.hpp"


namespace pmm {

    // must be called once before any alloc/free.
    // reads the Multiboot2 memory map from boot_info_addr and builds the
    // bitmap over all usable RAM, then marks the kernel image as already used.
    void init(u64 boot_info_addr);

    //rReturns the physical address of a free 4KB frame and marks it used.
    // panics (halts) if no free frames remain because there's no heap yet.
    u64 alloc_frame();

    // marks the 4KB frame at phys_addr as free.
    // phys_addr must be 4KB-aligned and previously allocated.
    void free_frame(u64 phys_addr);

    // for debug purposes
    void print_stats();

}
