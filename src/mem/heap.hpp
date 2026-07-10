#pragma once

#include "lib/types.hpp"

#include <stddef.h>


namespace heap {

    // base of the kernel heap's virtual address range - public so
    // `vmm::create_address_space()` can compute which PML4 slot to share
    // across every task's page tables (the heap is global kernel state,
    // same as the physmap - see vmm.hpp's PHYSMAP_BASE).
    constexpr u64 VIRT_BASE = 0xFFFF'8000'0000'0000ULL;

    // must be called once after pmm and vmm are initialized
    void init();

    // alloc `size` bytes of memory - returns pointer to allocated block
    void* kmalloc(size_t size);

    // free a block previously allocated by `kmalloc()`
    void kfree(void* ptr);
    
}