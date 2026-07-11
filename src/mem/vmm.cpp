#include "mem/vmm.hpp"

#include "mem/pmm.hpp"
#include "mem/heap.hpp"

#include "lib/types.hpp"
#include "lib/string.hpp"


// all section boundary symbols are defined in `linker.ld`.
extern "C" char _text_start[];   extern "C" char _text_end[];
extern "C" char _rodata_start[]; extern "C" char _rodata_end[];
extern "C" char _data_start[];   extern "C" char _data_end[];
extern "C" char _bss_start[];    extern "C" char _bss_end[];

namespace {

    // mask for extracting the physical address from a PTE.
    // bits 51:12 hold the frame address; bits 11:0 and 63 are flags.
    constexpr u64 PHYS_MASK = 0x000FFFFFFFFFF000ULL;
    constexpr u64 HUGE_PAGE = 1ULL << 7;

    // physical address of the boot/kernel PML4, captured by remap_kernel().
    // create_address_space() shares entries out of THIS, not "whatever CR3 is
    // loaded" (which could be some other task's private space by then).
    u64 g_kernel_pml4 = 0;

    // PML4 slot for a high-half base. the heap and physmap are static-after-
    // boot regions every task's tables share verbatim (see create_address_space).
    constexpr u64 pml4_index(u64 virt) { return (virt >> 39) & 0x1FF; }
    constexpr u64 HEAP_PML4_IDX    = pml4_index(heap::VIRT_BASE);
    constexpr u64 PHYSMAP_PML4_IDX = pml4_index(vmm::PHYSMAP_BASE);

    // the only genuinely private range inside PML4[0] (see
    // create_address_space). shared by create/clone/destroy_address_space -
    // getting this window wrong in only one would free or clone memory
    // nothing owns.
    //
    // PML4[0] can't be classified private-vs-shared by the HUGE_PAGE bit like
    // other slots: the kernel image lives here too, and remap_kernel() split
    // its covering huge pages into 4KB PTEs - so "present and not huge" isn't
    // reliably "this task's private mapping". only this explicit window is.
    constexpr u64 RESERVED_LOW  = 0x0000'0000'0020'0000ULL;  //   2 MiB
    constexpr u64 RESERVED_HIGH = 0x0000'0000'0100'0000ULL;  //  16 MiB

    // physical address stored in a PTE (strips flag bits).
    inline u64 entry_phys(u64 entry) {
        return entry & PHYS_MASK;
    }

    // a PTE's physical address as a dereferenceable kernel pointer, via the
    // physmap - a raw cast only works for the boot low-4GiB identity range,
    // not an arbitrary frame under some per-task CR3.
    inline u64* entry_to_kptr(u64 entry) {
        return reinterpret_cast<u64*>(vmm::phys_to_virt(entry_phys(entry)));
    }

    // pointer to the currently-active PML4 (from CR3, via the physmap).
    inline u64* get_pml4() {
        u64 cr3;
        asm volatile("mov %%cr3, %0" : "=r"(cr3));
        return reinterpret_cast<u64*>(vmm::phys_to_virt(cr3 & PHYS_MASK));
    }

    // ensure table[idx] points at a valid next-level table, returning it.
    // allocates a zeroed frame if absent; if present, only widens it with any
    // missing USER/WRITABLE - a leaf needs every intermediate level to grant
    // at least the permissions it does, and never narrow what another caller set.
    u64* ensure_table(u64* table, u64 idx, u64 flags) {
        if (!(table[idx] & vmm::PRESENT)) {
            u64 frame = pmm::alloc_frame();
            auto* t = reinterpret_cast<u64*>(vmm::phys_to_virt(frame));
            for (int i = 0; i < 512; ++i) t[i] = 0;  // stray bits would read as present
            table[idx] = frame | flags | vmm::PRESENT;
        } else {
            table[idx] |= (flags & (vmm::USER | vmm::WRITABLE));
        }
        return entry_to_kptr(table[idx]);
    }

    // split the 2MB huge page at pd[pd_idx] into 512 4KB PT entries over the
    // same physical range, carrying its flags through so nothing breaks until
    // the caller remaps individual pages with stricter permissions.
    void split_huge_page(u64* pd, u64 pd_idx) {
        u64 entry = pd[pd_idx];

        u64 base  = entry & 0x000FFFFFFFE00000ULL;  // bits 51:21, 2MB-aligned base
        // carry flags, minus the huge bit (7) and PD-level PAT bit (12) which
        // don't apply to PTEs.
        u64 flags = (entry & 0xFFF) & ~(HUGE_PAGE | (1ULL << 12));

        u64 pt_frame = pmm::alloc_frame();
        auto* pt = reinterpret_cast<u64*>(vmm::phys_to_virt(pt_frame));
        for (int i = 0; i < 512; ++i) {
            pt[i] = (base + static_cast<u64>(i) * 0x1000) | flags;
        }
        // replace the huge PD entry with a pointer to the new PT
        pd[pd_idx] = pt_frame | vmm::PRESENT | vmm::WRITABLE;
        // no invlpg here - the 512 TLB entries covering this 2MB range
        // are still valid because we preserved the same physical mappings.
        // The caller's invlpg (in map()) will flush the specific page being
        // changed, which is fine.
    }

    // core of `map()`, factored out to take an explicit PML4 pointer rather
    // than always reading the live CR3 (via `get_pml4()`) - `map()` itself
    // (below) is the common case (install into whichever address space is
    // CURRENTLY active), but `clone_address_space()` needs to install pages
    // into a brand new, not-yet-active PML4 without disturbing the live
    // one. deliberately does NOT `invlpg` - that only makes sense for the
    // currently-loaded CR3; the target address space's eventual first real
    // activation (a fresh CR3 load, in `sched.cpp`'s `schedule()`) flushes
    // the whole TLB anyway.
    void map_into(u64* pml4, u64 virt, u64 phys, u64 flags) {
        virt &= ~0xFFFull;
        phys &= ~0xFFFull;

        u64 pml4_idx = (virt >> 39) & 0x1FF;
        u64 pdpt_idx = (virt >> 30) & 0x1FF;
        u64 pd_idx   = (virt >> 21) & 0x1FF;
        u64 pt_idx   = (virt >> 12) & 0x1FF;

        u64 intermediate = vmm::PRESENT | vmm::WRITABLE;
        if (flags & vmm::USER) intermediate |= vmm::USER;

        u64* pdpt = ensure_table(pml4, pml4_idx, intermediate);
        u64* pd   = ensure_table(pdpt, pdpt_idx, intermediate);

        if ((pd[pd_idx] & vmm::PRESENT) && (pd[pd_idx] & HUGE_PAGE)) {
            split_huge_page(pd, pd_idx);
        }

        u64* pt    = ensure_table(pd, pd_idx, intermediate);
        pt[pt_idx] = phys | flags;
    }

    // recursively free every present non-HUGE subtree, then the table frame.
    // `level` = levels of indirection left below table_phys (0 = PT, its
    // entries are leaves; 1 = PD; 2 = PDPT).
    //
    // skips HUGE entries. safe for every slot but PML4[0]: elsewhere every
    // mapping is a plain 4KB PTE via map(), so present-and-not-huge means
    // "our own page". PML4[0] mixes in the kernel image (whose huge pages
    // remap_kernel() split into shared 4KB PTEs) - so its handlers walk the
    // RESERVED_LOW/HIGH window explicitly instead of calling this.
    void free_table(u64 table_phys, int level) {
        u64* table = reinterpret_cast<u64*>(vmm::phys_to_virt(table_phys));
        for (int i = 0; i < 512; ++i) {
            if (!(table[i] & vmm::PRESENT) || (table[i] & HUGE_PAGE)) continue;
            u64 child_phys = entry_phys(table[i]);
            if (level == 0) {
                pmm::free_frame(child_phys);
            } else {
                free_table(child_phys, level - 1);
            }
        }
        pmm::free_frame(table_phys);
    }

}

namespace vmm {

    void map(u64 virt, u64 phys, u64 flags) {
        map_into(get_pml4(), virt, phys, flags);
        asm volatile("invlpg (%0)" : : "r"(virt & ~0xFFFull) : "memory");  // drop stale TLB entry
    }

    void unmap(u64 virt) {
        virt &= ~0xFFFull;

        u64 pml4_idx = (virt >> 39) & 0x1FF;
        u64 pdpt_idx = (virt >> 30) & 0x1FF;
        u64 pd_idx   = (virt >> 21) & 0x1FF;
        u64 pt_idx   = (virt >> 12) & 0x1FF;

        u64* pml4 = get_pml4();
        if (!(pml4[pml4_idx] & PRESENT)) return;
        auto* pdpt = entry_to_kptr(pml4[pml4_idx]);

        if (!(pdpt[pdpt_idx] & PRESENT)) return;
        auto* pd = entry_to_kptr(pdpt[pdpt_idx]);

        // split a covering huge page first, so we clear one 4KB entry not 2MB.
        if ((pd[pd_idx] & PRESENT) && (pd[pd_idx] & HUGE_PAGE)) {
            split_huge_page(pd, pd_idx);
        }

        if (!(pd[pd_idx] & PRESENT)) return;
        auto* pt = entry_to_kptr(pd[pd_idx]);

        pt[pt_idx] = 0;
        asm volatile("invlpg (%0)" : : "r"(virt) : "memory");
    }

    void protect(u64 virt, u64 flags) {
        u64 phys = virt_to_phys(virt);
        if (phys == 0) return;  // not mapped - nothing to change
        // re-map the same frame with new flags; map() page-aligns both.
        map(virt, phys, flags);
    }

    u64 virt_to_phys(u64 virt) {
        u64 pml4_idx = (virt >> 39) & 0x1FF;
        u64 pdpt_idx = (virt >> 30) & 0x1FF;
        u64 pd_idx   = (virt >> 21) & 0x1FF;
        u64 pt_idx   = (virt >> 12) & 0x1FF;
        u64 offset   =  virt        & 0xFFF;

        u64* pml4 = get_pml4();
        if (!(pml4[pml4_idx] & PRESENT)) return 0;
        auto* pdpt = entry_to_kptr(pml4[pml4_idx]);

        if (!(pdpt[pdpt_idx] & PRESENT)) return 0;
        // 1GB huge page (rare, but handle it anyways)
        if (pdpt[pdpt_idx] & HUGE_PAGE) {
            return (entry_phys(pdpt[pdpt_idx]) & 0x000FFFFFC0000000ULL) + (virt & 0x3FFFFFFF);
        }
        auto* pd = entry_to_kptr(pdpt[pdpt_idx]);

        if (!(pd[pd_idx] & PRESENT)) return 0;
        // 2MB huge page
        if (pd[pd_idx] & HUGE_PAGE) {
            return (entry_phys(pd[pd_idx]) & 0x000FFFFFFFE00000ULL) + (virt & 0x1FFFFF);
        }
        auto* pt = entry_to_kptr(pd[pd_idx]);

        if (!(pt[pt_idx] & PRESENT)) return 0;
        return entry_phys(pt[pt_idx]) + offset;
    }

    void remap_kernel() {
        auto remap_range = [](u64 start, u64 end, u64 flags) {
            for (u64 addr = start; addr < end; addr += 0x1000) map(addr, addr, flags);
        };

        u64 text_s   = reinterpret_cast<u64>(_text_start);
        u64 text_e   = reinterpret_cast<u64>(_text_end);
        u64 rodata_s = reinterpret_cast<u64>(_rodata_start);
        u64 rodata_e = reinterpret_cast<u64>(_rodata_end);
        u64 data_s   = reinterpret_cast<u64>(_data_start);
        u64 data_e   = reinterpret_cast<u64>(_data_end);
        u64 bss_s    = reinterpret_cast<u64>(_bss_start);
        u64 bss_e    = reinterpret_cast<u64>(_bss_end);

        remap_range(text_s,   text_e,   KERNEL_RX);  // code:   R-X
        remap_range(rodata_s, rodata_e, KERNEL_RO);  // rodata: R--
        remap_range(data_s,   data_e,   KERNEL_RW);  // data:   RW-
        remap_range(bss_s,    bss_e,    KERNEL_RW);  // bss:    RW-

        // capture the boot PML4 for create_address_space() to share out of,
        // while it's still the only one that exists.
        u64 cr3;
        asm volatile("mov %%cr3, %0" : "=r"(cr3));
        g_kernel_pml4 = cr3 & PHYS_MASK;
    }

    u64 create_address_space() {
        u64 new_pml4_phys = pmm::alloc_frame();
        auto* new_pml4 = reinterpret_cast<u64*>(phys_to_virt(new_pml4_phys));
        for (int i = 0; i < 512; ++i) new_pml4[i] = 0;

        auto* kernel_pml4 = reinterpret_cast<u64*>(phys_to_virt(g_kernel_pml4));

        // heap + physmap: global, static after boot - share the pointer.
        new_pml4[HEAP_PML4_IDX]    = kernel_pml4[HEAP_PML4_IDX];
        new_pml4[PHYSMAP_PML4_IDX] = kernel_pml4[PHYSMAP_PML4_IDX];

        // PML4[0] (low 4GiB) holds more than the kernel image: every device
        // MMIO region (LAPIC, I/O APIC, HPET, AHCI BARs), mapped once at boot,
        // must stay reachable from every address space or e.g. an IRQ's EOI
        // write page-faults under a task's CR3. share all of it EXCEPT the
        // reserved low window, which each task keeps private for its non-PIE
        // ELF at 0x400000 (task/user.cpp's USER_CODE_VIRT).
        //
        // this low-range entanglement is what a higher-half kernel avoids by
        // construction; the reserved window is a pragmatic stand-in, safe
        // while nothing needs a boot mapping inside [RESERVED_LOW, HIGH)
        // (kernel image below it, device MMIO well above).

        u64 new_pdpt_phys = pmm::alloc_frame();
        auto* new_pdpt = reinterpret_cast<u64*>(phys_to_virt(new_pdpt_phys));
        for (int i = 0; i < 512; ++i) new_pdpt[i] = 0;

        if (kernel_pml4[0] & PRESENT) {
            auto* kernel_pdpt = entry_to_kptr(kernel_pml4[0]);

            for (u64 pdpt_idx = 0; pdpt_idx < 4; ++pdpt_idx) {  // low 4GiB only
                if (!(kernel_pdpt[pdpt_idx] & PRESENT)) continue;
                auto* kernel_pd = entry_to_kptr(kernel_pdpt[pdpt_idx]);

                u64 new_pd_phys = pmm::alloc_frame();
                auto* new_pd = reinterpret_cast<u64*>(phys_to_virt(new_pd_phys));

                for (u64 pd_idx = 0; pd_idx < 512; ++pd_idx) {
                    u64 virt_2mb = (pdpt_idx << 30) | (pd_idx << 21);
                    bool reserved = virt_2mb >= RESERVED_LOW && virt_2mb < RESERVED_HIGH;
                    new_pd[pd_idx] = (!reserved && (kernel_pd[pd_idx] & PRESENT))
                                        ? kernel_pd[pd_idx] : 0;
                }

                new_pdpt[pdpt_idx] = new_pd_phys | PRESENT | WRITABLE | USER;
            }
        }
        new_pml4[0] = new_pdpt_phys | PRESENT | WRITABLE | USER;

        return new_pml4_phys;
    }

    // copy one private 4KB leaf into a fresh frame and map it at `virt` in
    // dst_pml4, keeping only the flags map_into cares about.
    void clone_leaf(u64* dst_pml4, u64 virt, u64 src_entry) {
        u64 flags = src_entry & (vmm::PRESENT | vmm::WRITABLE | vmm::USER | vmm::NO_EXECUTE);
        u64 new_phys = pmm::alloc_frame();
        string::memcpy(
            reinterpret_cast<void*>(vmm::phys_to_virt(new_phys)),
            reinterpret_cast<void*>(vmm::phys_to_virt(entry_phys(src_entry))),
            0x1000);
        map_into(dst_pml4, virt, new_phys, flags);
    }

    // clone every PT entry under `src_pt`, all sharing the same `pml4_idx`/
    // `pdpt_idx`/`pd_idx` (so only `pt_idx` varies the reconstructed `virt`).
    void clone_pt(u64* dst_pml4, u64* src_pt, u64 pml4_idx, u64 pdpt_idx, u64 pd_idx) {
        for (u64 pt_idx = 0; pt_idx < 512; ++pt_idx) {
            if (!(src_pt[pt_idx] & vmm::PRESENT)) continue;
            u64 virt = (pml4_idx << 39) | (pdpt_idx << 30) | (pd_idx << 21) | (pt_idx << 12);
            clone_leaf(dst_pml4, virt, src_pt[pt_idx]);
        }
    }

    void clone_address_space(u64 dst_cr3, u64 src_cr3) {
        auto* src_pml4 = reinterpret_cast<u64*>(phys_to_virt(src_cr3 & PHYS_MASK));
        auto* dst_pml4 = reinterpret_cast<u64*>(phys_to_virt(dst_cr3 & PHYS_MASK));

        // PML4[0] mixes private and shared content; only the reserved window
        // is ever privately populated, so walk exactly that.
        if (src_pml4[0] & vmm::PRESENT) {
            auto* src_pdpt = entry_to_kptr(src_pml4[0]);
            if (src_pdpt[0] & vmm::PRESENT) {  // RESERVED_LOW/HIGH both < 1GiB
                auto* src_pd = entry_to_kptr(src_pdpt[0]);
                for (u64 pd_idx = 0; pd_idx < 512; ++pd_idx) {
                    u64 virt_2mb = pd_idx << 21;
                    if (virt_2mb < RESERVED_LOW || virt_2mb >= RESERVED_HIGH) continue;
                    if (!(src_pd[pd_idx] & vmm::PRESENT)) continue;  // never HUGE in this window
                    clone_pt(dst_pml4, entry_to_kptr(src_pd[pd_idx]), 0, 0, pd_idx);
                }
            }
        }

        // every other present slot (bar the two shared ones and slot 0) is
        // fully private - create_address_space() leaves them empty and only
        // this task's mmap/stack setup fills them. walk them unconditionally.
        for (u64 pml4_idx = 1; pml4_idx < 512; ++pml4_idx) {
            if (pml4_idx == HEAP_PML4_IDX || pml4_idx == PHYSMAP_PML4_IDX) continue;
            if (!(src_pml4[pml4_idx] & vmm::PRESENT)) continue;
            auto* src_pdpt = entry_to_kptr(src_pml4[pml4_idx]);

            for (u64 pdpt_idx = 0; pdpt_idx < 512; ++pdpt_idx) {
                if (!(src_pdpt[pdpt_idx] & vmm::PRESENT)) continue;
                auto* src_pd = entry_to_kptr(src_pdpt[pdpt_idx]);

                for (u64 pd_idx = 0; pd_idx < 512; ++pd_idx) {
                    if (!(src_pd[pd_idx] & vmm::PRESENT)) continue;
                    clone_pt(dst_pml4, entry_to_kptr(src_pd[pd_idx]), pml4_idx, pdpt_idx, pd_idx);
                }
            }
        }
    }

    // frees what's privately owned under PML4[0]: the reserved window's PTs +
    // leaves, then the PD/PDPT frames (always private, whatever their content).
    void destroy_pml4_slot0(u64 pdpt_phys) {
        auto* pdpt = reinterpret_cast<u64*>(vmm::phys_to_virt(pdpt_phys));
        for (u64 pdpt_idx = 0; pdpt_idx < 4; ++pdpt_idx) {
            if (!(pdpt[pdpt_idx] & vmm::PRESENT)) continue;
            u64 pd_phys = entry_phys(pdpt[pdpt_idx]);
            auto* pd = reinterpret_cast<u64*>(vmm::phys_to_virt(pd_phys));

            if (pdpt_idx == 0) {  // RESERVED_LOW/HIGH both < 1GiB
                for (u64 pd_idx = 0; pd_idx < 512; ++pd_idx) {
                    u64 virt_2mb = pd_idx << 21;
                    if (virt_2mb < RESERVED_LOW || virt_2mb >= RESERVED_HIGH) continue;
                    if (!(pd[pd_idx] & vmm::PRESENT)) continue;  // never HUGE in this window
                    free_table(entry_phys(pd[pd_idx]), 0);       // PT -> leaves
                }
            }
            // everything else in slot 0 is a borrowed/shared reference, never freed.
            pmm::free_frame(pd_phys);  // the PD frame itself is always private
        }
        pmm::free_frame(pdpt_phys);    // the PDPT frame itself is always private
    }

    void destroy_address_space(u64 cr3) {
        u64 pml4_phys = cr3 & PHYS_MASK;
        auto* pml4 = reinterpret_cast<u64*>(phys_to_virt(pml4_phys));

        if (pml4[0] & PRESENT) destroy_pml4_slot0(entry_phys(pml4[0]));

        for (u64 pml4_idx = 1; pml4_idx < 512; ++pml4_idx) {
            if (pml4_idx == HEAP_PML4_IDX || pml4_idx == PHYSMAP_PML4_IDX) continue;
            if (!(pml4[pml4_idx] & PRESENT)) continue;
            // level 2: this entry points at a PDPT (entries -> PD -> PT -> leaf).
            free_table(entry_phys(pml4[pml4_idx]), 2);
        }

        pmm::free_frame(pml4_phys);
    }

}
