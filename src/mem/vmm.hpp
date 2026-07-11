#pragma once

#include "lib/types.hpp"


namespace vmm {

    // page table entry flags.
    constexpr u64 PRESENT    = 1ULL << 0;  // page is mapped
    constexpr u64 WRITABLE   = 1ULL << 1;  // writes allowed
    constexpr u64 USER       = 1ULL << 2;  // ring 3 accessible
    constexpr u64 NO_EXECUTE = 1ULL << 63; // instruction fetch not allowed
                                           //   (requires NXE bit in EFER)

    // kernel mapping combos, for remap_kernel readability.
    constexpr u64 KERNEL_RX = PRESENT;                          // code: read + execute
    constexpr u64 KERNEL_RO = PRESENT | NO_EXECUTE;             // rodata: read only
    constexpr u64 KERNEL_RW = PRESENT | WRITABLE | NO_EXECUTE;  // data/bss: read + write

    constexpr u64 PWT = 1ULL << 3; constexpr u64 PCD = 1ULL << 4;

    // "physmap" - all physical RAM mapped 1:1-offset at this base, identically
    // in every address space (boot.asm's p3_table_physmap, PML4 index 384).
    // lets the kernel turn a physical address into a pointer regardless of the
    // active CR3 - needed once per-task page tables exist.
    constexpr u64 PHYSMAP_BASE = 0xFFFF'C000'0000'0000ULL;
    inline u64 phys_to_virt(u64 phys) { return PHYSMAP_BASE + phys; }

    // map the 4KB page at `virt` to frame `phys` with `flags`. creates
    // intermediate tables (from the PMM) and splits any covering huge page.
    void map(u64 virt, u64 phys, u64 flags);

    // unmap the 4KB page at `virt`. does NOT free the frame, nor collapse
    // now-empty intermediate tables.
    void unmap(u64 virt);

    // change flags on an already-mapped page (for mprotect), same frame. no-op
    // if `virt` isn't mapped.
    void protect(u64 virt, u64 flags);

    // physical address `virt` maps to, or 0 if unmapped. handles huge pages.
    u64 virt_to_phys(u64 virt);

    // remap the kernel's own pages with per-section permissions (.text RX,
    // .rodata RO, .data/.bss RW), replacing the boot RWX huge pages. call
    // after pmm::init().
    void remap_kernel();

    // deep-copy every page privately owned by src_cr3 (what
    // create_address_space() leaves unpopulated - the ELF/brk window and any
    // mmap/stack slots) into fresh frames at the same VAs in dst_cr3. for
    // sys_fork(); dst_cr3 must be a fresh create_address_space().
    void clone_address_space(u64 dst_cr3, u64 src_cr3);

    // free every page privately owned by `cr3`, its page-table frames, and the
    // PML4 - never anything create_address_space() shared (kernel/MMIO/heap/
    // physmap). called from exit_current() and execve().
    void destroy_address_space(u64 cr3);

    // build a fresh private PML4: shares the heap + physmap entries and the PDs
    // covering the kernel image / device MMIO (inside PML4[0]), everything else
    // unmapped for the task's own ELF/stack/brk/mmap. call after remap_kernel().
    // returns the PML4 physical address (for Task::cr3).
    u64 create_address_space();

}
