#include "mem/vmm.hpp"

#include "mem/pmm.hpp"
#include "mem/heap.hpp"

#include "lib/types.hpp"


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

    // physical address of the ORIGINAL boot/kernel PML4 - captured once by
    // `remap_kernel()`, which runs long before any per-task PML4 exists.
    // `create_address_space()` shares entries out of THIS specifically,
    // never "whatever CR3 happens to be loaded" (which, called later, could
    // already be some other task's private one).
    u64 g_kernel_pml4 = 0;

    // which PML4 slot a fixed high-half virtual base lives in - both the
    // heap (heap::VIRT_BASE) and the physmap (vmm::PHYSMAP_BASE) are global,
    // static-after-boot regions that every task's page tables share
    // verbatim (see create_address_space()).
    constexpr u64 pml4_index(u64 virt) { return (virt >> 39) & 0x1FF; }
    constexpr u64 HEAP_PML4_IDX    = pml4_index(heap::VIRT_BASE);
    constexpr u64 PHYSMAP_PML4_IDX = pml4_index(vmm::PHYSMAP_BASE);

    // physical address stored in a PTE (strips flag bits).
    inline u64 entry_phys(u64 entry) {
        return entry & PHYS_MASK;
    }

    // a PTE's physical address, as a dereferenceable kernel pointer - via
    // the physmap (vmm::phys_to_virt), NOT a raw cast. once per-task page
    // tables exist, whatever CR3 happens to be loaded generally does NOT
    // have an arbitrary physical frame identity-mapped at its own address
    // (that's only true for the boot/kernel's own low-4GiB range) - the
    // physmap is what makes this work regardless of which CR3 is active.
    inline u64* entry_to_kptr(u64 entry) {
        return reinterpret_cast<u64*>(vmm::phys_to_virt(entry_phys(entry)));
    }

    // read CR3 and return a pointer to the live PML4 table - i.e. whichever
    // address space is CURRENTLY active, via the physmap (see
    // entry_to_kptr's comment for why that's necessary, not just tidy).
    inline u64* get_pml4() {
        u64 cr3;
        asm volatile("mov %%cr3, %0" : "=r"(cr3));
        return reinterpret_cast<u64*>(vmm::phys_to_virt(cr3 & PHYS_MASK));
    }

    // ensure `table[idx]` points to a valid next-level page table.
    // if the entry is not present, allocate a fresh zeroed frame and
    // install it with `flags`. if it IS already present, widen it to
    // include any of `flags` (specifically USER / WRITABLE) that it's
    // missing - since a leaf mapping down this branch requires every
    // intermediate level to grant at least the permissions the leaf does.
    // returns a pointer to the next-level table.
    u64* ensure_table(u64* table, u64 idx, u64 flags) {
        if (!(table[idx] & vmm::PRESENT)) {
            u64 frame = pmm::alloc_frame();
            // zero the new table - uncleared entries would look "present"
            // if any junk bit happened to be set. via the physmap: a
            // freshly allocated frame generally isn't mapped anywhere else
            // in the currently-active address space yet.
            auto* t = reinterpret_cast<u64*>(vmm::phys_to_virt(frame));
            for (int i = 0; i < 512; ++i) t[i] = 0;
            table[idx] = frame | flags | vmm::PRESENT;
        } else {
            // entry exists (usually from boot.asm's identity map or a
            // prior map() call). only OR bits IN, never clear anything -
            // some other caller may have set it wider than us already.
            table[idx] |= (flags & (vmm::USER | vmm::WRITABLE));
        }
        return entry_to_kptr(table[idx]);
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
        // intermediate entries always get PRESENT | WRITABLE (so the walk
        // itself doesn't fault); USER is added conditionally, since
        // x86-64 requires it at EVERY level of the walk for a ring-3
        // access to succeed. without this, the leaf PTE could set USER
        // and still get a #PF because some intermediate lacks it.
        u64 intermediate = PRESENT | WRITABLE;
        if (flags & USER) intermediate |= USER;

        u64* pml4 = get_pml4();
        u64* pdpt = ensure_table(pml4, pml4_idx, intermediate);
        u64* pd   = ensure_table(pdpt, pdpt_idx, intermediate);

        // if the PD entry covering `virt` is currently a 2MB huge page,
        // we must split it before we can install a 4KB entry inside it.
        if ((pd[pd_idx] & PRESENT) && (pd[pd_idx] & HUGE_PAGE)) {
            split_huge_page(pd, pd_idx);
        }

        u64* pt       = ensure_table(pd, pd_idx, intermediate);
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
        auto* pdpt = entry_to_kptr(pml4[pml4_idx]);

        if (!(pdpt[pdpt_idx] & PRESENT)) return;
        auto* pd = entry_to_kptr(pdpt[pdpt_idx]);

        // if a 2MB huge page covers this address, split it first so we
        // can zero just the one 4KB entry rather than the whole 2MB range.
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
        // re-`map()` the same frame with the new flags. this loses the low
        // 12 bits of `phys` from `virt_to_phys`'s page-offset arithmetic
        // only if `virt` wasn't page-aligned to begin with - `map()`
        // aligns both down to the page boundary anyway, so pass `virt`
        // through as-is and let it do that.
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

        // capture the CURRENT (boot/kernel) PML4's physical address for
        // create_address_space() to share entries out of, later - this
        // must run before any per-task PML4 could possibly be active.
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

        // PML4[0] (the low 4GiB, PDPT indices 0-3) holds a lot more than
        // just the kernel image: every device MMIO region mapped at its own
        // fixed physical/identity address by boot-time driver init - the
        // local APIC (0xFEE00000), I/O APIC, HPET, AHCI BARs, etc (`apic`/
        // `hpet`/`ahci`'s `vmm::map()` calls, all made once, before any
        // per-task PML4 exists) - all need to keep working from every
        // address space, or e.g. an IRQ's EOI write to the LAPIC page-faults
        // the instant this task's CR3 is active. share ALL of it, EXCEPT a
        // reserved low window for user code (a non-PIE ELF's PT_LOAD
        // segments land at a fixed `0x400000` - see `task/user.cpp`'s
        // `USER_CODE_VIRT`) - that one stays private per task.
        //
        // this low-range entanglement (kernel/device state and non-PIE user
        // code sharing the same 4GiB window) is exactly what a higher-half
        // kernel avoids by construction - out of scope here (see the CR3
        // plan doc's "explicitly deferred"), so this reserved-window
        // approach is a pragmatic stand-in: safe as long as nothing ever
        // needs a boot-time mapping inside [RESERVED_LOW, RESERVED_HIGH),
        // which holds today (the kernel image sits below it, device MMIO
        // on this QEMU machine sits well above it).
        constexpr u64 RESERVED_LOW  = 0x0000'0000'0020'0000ULL;  //   2 MiB
        constexpr u64 RESERVED_HIGH = 0x0000'0000'0100'0000ULL;  //  16 MiB - 12MiB of headroom past 0x400000

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

}
