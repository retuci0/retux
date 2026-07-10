#pragma once

#include "lib/types.hpp"


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

    // "physmap" - all physical RAM mapped 1:1-offset at this fixed virtual
    // base, identically in EVERY address space (see boot.asm's
    // `p3_table_physmap`, installed at PML4 index 384). this is what lets
    // the kernel turn a bare physical address into a usable pointer without
    // depending on which CR3 happens to be loaded - required once per-task
    // page tables exist (create_address_space() below) and stop sharing the
    // low identity map wholesale.
    constexpr u64 PHYSMAP_BASE = 0xFFFF'C000'0000'0000ULL;
    inline u64 phys_to_virt(u64 phys) { return PHYSMAP_BASE + phys; }

    // map the 4KB page containing `virt` to the physical frame at `phys`,
    // with the given flags. creates intermediate page-table levels as needed
    // (consuming frames from the PMM). splits any existing 2MB huge page
    // that covers `virt` before installing the mapping thing.
    void map(u64 virt, u64 phys, u64 flags);

    // remove the mapping for the 4KB page containing `virt`.
    // does NOT free the physical frame - caller is responsible.
    // does NOT collapse empty intermediate tables (although not needed yet).
    void unmap(u64 virt);

    // change the flags on an already-mapped 4KB page, keeping the same
    // physical frame - for `mprotect` (cpu/syscall.cpp). no-op if `virt`
    // isn't currently mapped (mprotect will just silently "succeed" on it,
    // same as `map()` would if called fresh).
    void protect(u64 virt, u64 flags);

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

    // build a fresh, private PML4 for a new task: shares the kernel heap's
    // and physmap's PML4 entries (both are global, static after boot) plus
    // just the PD entries covering the kernel image itself
    // (`_kernel_start`.._kernel_end`, inside PML4[0] - the ONLY top-level
    // slot that also holds low user addresses like a non-PIE ELF's
    // `0x400000`) - everything else starts unmapped, ready for that task's
    // own ELF/stack/`brk`/`mmap` setup to populate privately. must be
    // called after `remap_kernel()`. returns the new PML4's physical
    // address (for `Task::cr3`).
    u64 create_address_space();

}
