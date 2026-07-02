#pragma once

#include "types.hpp"


namespace vmm {

    // page table entry flags.
    constexpr u64 PRESENT    = 1ULL << 0;  // page is mapped
    constexpr u64 WRITABLE   = 1ULL << 1;  // writes allowed
    constexpr u64 USER       = 1ULL << 2;  // ring 3 accessible
    constexpr u64 NO_EXECUTE = 1ULL << 63; // instruction fetch not allowed
                                           //   (requires NXE bit in EFER)

    // convenience combinations for kernel mappings.
    // using these rather than raw flags makes remap_kernel readable.
    constexpr u64 KERNEL_RX = PRESENT;                          // code: read + execute
    constexpr u64 KERNEL_RO = PRESENT | NO_EXECUTE;             // rodata: read only
    constexpr u64 KERNEL_RW = PRESENT | WRITABLE | NO_EXECUTE;  // data/bss: read + write

    constexpr u64 PWT = 1ULL << 3; constexpr u64 PCD = 1ULL << 4;

    // map the 4KB page containing `virt` to the physical frame at `phys`,
    // with the given flags. creates intermediate page-table levels as needed
    // (consuming frames from the PMM). splits any existing 2MB huge page
    // that covers `virt` before installing the mapping thing.
    void map(u64 virt, u64 phys, u64 flags);

    // remove the mapping for the 4KB page containing `virt`.
    // does NOT free the physical frame - caller is responsible.
    // does NOT collapse empty intermediate tables (although not needed yet).
    void unmap(u64 virt);

    // walk the live page tables and return the physical address that `virt`
    // handles 2MB huge pages.
    u64 virt_to_phys(u64 virt);

    // remap the kernel's own pages with correct per-section permissions,
    // replacing the boot-time "everything RWX" 2MB huge pages with
    // fine-grained 4KB mappings:
    //   .text   -> KERNEL_RX  (read + execute, not writable)
    //   .rodata -> KERNEL_RO  (read,           not writable, not executable)
    //   .data   -> KERNEL_RW  (read + write,   not executable)
    //   .bss    -> KERNEL_RW  (""")
    // must be called after pmm::init().
    void remap_kernel();

}