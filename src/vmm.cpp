#include "vmm.hpp"
#include "pmm.hpp"

#include "types.hpp"


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

    // physical address stored in a PTE (strips flag bits).
    inline u64 entry_phys(u64 entry) {
        return entry & PHYS_MASK;
    }

    // read CR3 and return a pointer to the live PML4 table.
    // works because virtual == physical for everything in 
    // the kernel's address range, so we can use the
    // raw physical address directly as a pointer.
    inline u64* get_pml4() {
        u64 cr3;
        asm volatile("mov %%cr3, %0" : "=r"(cr3));
        return reinterpret_cast<u64*>(cr3 & PHYS_MASK);
    }

    // ensure `table[idx]` points to a valid next-level page table.
    // if the entry is not present, allocate a fresh zeroed frame and
    // install it. returns a pointer to the next-level table.
    u64* ensure_table(u64* table, u64 idx, u64 flags) {
        if (!(table[idx] & vmm::PRESENT)) {
            u64 frame = pmm::alloc_frame();
            // zero the new table - uncleared entries would look "present"
            // if any junk bit happened to be set
            auto* t = reinterpret_cast<u64*>(frame);
            for (int i = 0; i < 512; ++i) t[i] = 0;
            table[idx] = frame | flags | vmm::PRESENT;
        }
        return reinterpret_cast<u64*>(entry_phys(table[idx]));
    }

    // split the 2MB huge page at pd[pd_idx] into 512 individual 4KB PT
    // entries covering exactly the same physical range.
    // 
    // the boot map sets all huge pages present+writable with no NX 
    // we carry those flags through so nothing breaks until the caller
    // explicitly remaps individual pages with stricter permissions.
    void split_huge_page(u64* pd, u64 pd_idx) {
        u64 entry = pd[pd_idx];

        // bits 51:21 hold the 2MB-aligned physical base address.
        u64 base  = entry & 0x000FFFFFFFE00000ULL;
        // carry lower flags (present, writable, etc.) but strip the huge
        // bit (7) and the PD-level PAT bit (12) which doesn't apply to PTEs.
        u64 flags = (entry & 0xFFF) & ~(HUGE_PAGE | (1ULL << 12));

        u64 pt_frame = pmm::alloc_frame();
        auto* pt = reinterpret_cast<u64*>(pt_frame);
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

}

namespace vmm {

    void map(u64 virt, u64 phys, u64 flags) {
        // align down to page boundaries, we map whole pages only.
        virt &= ~0xFFFull;
        phys &= ~0xFFFull;

        // decompose the virtual address into its four 9-bit table indices.
        u64 pml4_idx = (virt >> 39) & 0x1FF;
        u64 pdpt_idx = (virt >> 30) & 0x1FF;
        u64 pd_idx   = (virt >> 21) & 0x1FF;
        u64 pt_idx   = (virt >> 12) & 0x1FF;

        // walk down the hierarchy, creating missing tables on the way.
        // intermediate entries always get PRESENT | WRITABLE so we can
        // reach them regardless of the final page's flags.
        u64* pml4 = get_pml4();
        u64* pdpt = ensure_table(pml4, pml4_idx, PRESENT | WRITABLE);
        u64* pd   = ensure_table(pdpt, pdpt_idx, PRESENT | WRITABLE);

        // if the PD entry covering `virt` is currently a 2MB huge page,
        // we must split it before we can install a 4KB entry inside it.
        if ((pd[pd_idx] & PRESENT) && (pd[pd_idx] & HUGE_PAGE)) {
            split_huge_page(pd, pd_idx);
        }

        u64* pt       = ensure_table(pd, pd_idx, PRESENT | WRITABLE);
        pt[pt_idx]    = phys | flags;

        // flush this single address from the TLB. without this, the CPU
        // may continue using a cached (stale) translation for `virt`.
        asm volatile("invlpg (%0)" : : "r"(virt) : "memory");
    }

    void unmap(u64 virt) {
        virt &= ~0xFFFull;

        u64 pml4_idx = (virt >> 39) & 0x1FF;
        u64 pdpt_idx = (virt >> 30) & 0x1FF;
        u64 pd_idx   = (virt >> 21) & 0x1FF;
        u64 pt_idx   = (virt >> 12) & 0x1FF;

        u64* pml4 = get_pml4();
        if (!(pml4[pml4_idx] & PRESENT)) return;
        auto* pdpt = reinterpret_cast<u64*>(entry_phys(pml4[pml4_idx]));

        if (!(pdpt[pdpt_idx] & PRESENT)) return;
        auto* pd = reinterpret_cast<u64*>(entry_phys(pdpt[pdpt_idx]));

        // if a 2MB huge page covers this address, split it first so we
        // can zero just the one 4KB entry rather than the whole 2MB range.
        if ((pd[pd_idx] & PRESENT) && (pd[pd_idx] & HUGE_PAGE)) {
            split_huge_page(pd, pd_idx);
        }

        if (!(pd[pd_idx] & PRESENT)) return;
        auto* pt = reinterpret_cast<u64*>(entry_phys(pd[pd_idx]));

        pt[pt_idx] = 0;
        asm volatile("invlpg (%0)" : : "r"(virt) : "memory");
    }

    u64 virt_to_phys(u64 virt) {
        u64 pml4_idx = (virt >> 39) & 0x1FF;
        u64 pdpt_idx = (virt >> 30) & 0x1FF;
        u64 pd_idx   = (virt >> 21) & 0x1FF;
        u64 pt_idx   = (virt >> 12) & 0x1FF;
        u64 offset   =  virt        & 0xFFF;

        u64* pml4 = get_pml4();
        if (!(pml4[pml4_idx] & PRESENT)) return 0;
        auto* pdpt = reinterpret_cast<u64*>(entry_phys(pml4[pml4_idx]));

        if (!(pdpt[pdpt_idx] & PRESENT)) return 0;
        // 1GB huge page (rare, but handle it anyways)
        if (pdpt[pdpt_idx] & HUGE_PAGE) {
            return (entry_phys(pdpt[pdpt_idx]) & 0x000FFFFFC0000000ULL) + (virt & 0x3FFFFFFF);
        }
        auto* pd = reinterpret_cast<u64*>(entry_phys(pdpt[pdpt_idx]));

        if (!(pd[pd_idx] & PRESENT)) return 0;
        // 2MB huge page
        if (pd[pd_idx] & HUGE_PAGE) {
            return (entry_phys(pd[pd_idx]) & 0x000FFFFFFFE00000ULL) + (virt & 0x1FFFFF);
        }
        auto* pt = reinterpret_cast<u64*>(entry_phys(pd[pd_idx]));

        if (!(pt[pt_idx] & PRESENT)) return 0;
        return entry_phys(pt[pt_idx]) + offset;
    }

    void remap_kernel() {
        // helper to remap a range of pages with a given flag set.
        // calls map() on each 4KB page in the range
        auto remap_range = [](u64 start, u64 end, u64 flags) {
            for (u64 addr = start; addr < end; addr += 0x1000) {
                // virt == phys
                map(addr, addr, flags);
            }
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
    }

}