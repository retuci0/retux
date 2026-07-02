#include "pmm.hpp"
#include "mb2.hpp"
#include "hex.hpp"
#include "serial.hpp"

#include "types.hpp"


namespace {

    constexpr u64 PAGE_SIZE   = 4096;
    // maximum physical memory that can be tracked
    constexpr u64 MAX_PHYS    = 0x100000000ULL;         // 4GB
    constexpr u64 MAX_FRAMES  = MAX_PHYS / PAGE_SIZE;  // 1.048.576 frames
    constexpr u64 BITMAP_SIZE = MAX_FRAMES / 8;        // 131.072 bytes = 128KB

    // one bit per 4KB physical frame. 1 = used/reserved, 0 = free.
    // starts with everything reserved, we explicitly mark only
    // the regions the bootloader says are available RAM as free.
    // that way any range we forgot to initialise is safe (reserved)
    // rather than silently handed out.
    u8 bitmap[BITMAP_SIZE];

    u64 total_frames = 0;
    u64 used_frames  = 0;


    // --- bit manipulation helpers ---

    inline void set_used(u64 frame) {
        bitmap[frame / 8] |=  (1 << (frame % 8));
    }

    inline void set_free(u64 frame) {
        bitmap[frame / 8] &= ~(1 << (frame % 8));
    }

    inline bool is_used(u64 frame) {
        return bitmap[frame / 8] & (1 << (frame % 8));
    }


    // --- range helpers ---

    // mark every frame in [base, base+length) as free.
    // rounds base UP and (base+length) DOWN to page boundaries so we never
    // hand out a partial page.
    void mark_range_free(u64 base, u64 length) {
        u64 start = (base + PAGE_SIZE - 1) & ~(PAGE_SIZE - 1);  // ceil
        u64 end   = (base + length)        & ~(PAGE_SIZE - 1);  // floor
        if (start >= end) return;
        for (u64 addr = start; addr < end; addr += PAGE_SIZE) {
            u64 frame = addr / PAGE_SIZE;
            if (frame >= MAX_FRAMES) break;
            if (is_used(frame)) {
                // was marked reserved by the all-ones init; now it's free
                set_free(frame);
                ++total_frames;
            }
        }
    }

    // mark every frame in [base, base+length) as used.
    // used to carve the kernel image out of available RAM after we've
    // built the free list from the memory map.
    void mark_range_used(u64 base, u64 length) {
        u64 start = base                            & ~(PAGE_SIZE - 1);  // floor
        u64 end   = (base + length + PAGE_SIZE - 1) & ~(PAGE_SIZE - 1);  // ceil
        for (u64 addr = start; addr < end; addr += PAGE_SIZE) {
            u64 frame = addr / PAGE_SIZE;
            if (frame >= MAX_FRAMES) break;
            if (!is_used(frame)) {
                set_used(frame);
                ++used_frames;
            }
        }
    }

}


// _kernel_start and _kernel_end are defined in `linker.ld` and mark the
// exact physical extent of the loaded kernel image
extern "C" char _kernel_start[];
extern "C" char _kernel_end[];


namespace pmm {

    void init(u64 boot_info_addr) {
        // fill with 0xFF so frames we never see in the memory map stay reserved
        for (u64 i = 0; i < BITMAP_SIZE; ++i) bitmap[i] = 0xFF;
        total_frames = 0;
        used_frames  = 0;

        // find the Multiboot2 memory map tag.
        const auto* tag = mb2::find_tag(boot_info_addr, mb2::TAG_MEMMAP);
        if (!tag) {
            serial::print("pmm: no memory map tag — cannot initialise\n");
            asm volatile("cli; hlt");
        }

        const auto* mmap = reinterpret_cast<const mb2::MemMapTag*>(tag);
        const u8*   entry_ptr = reinterpret_cast<const u8*>(mmap + 1);
        const u8*   mmap_end  = reinterpret_cast<const u8*>(mmap) + mmap->size;

        // flip those frames from reserved to free in the bitmap for each available region
        while (entry_ptr < mmap_end) {
            const auto* entry = reinterpret_cast<const mb2::MemMapEntry*>(entry_ptr);
            if (entry->type == mb2::MEMMAP_AVAIL) {
                mark_range_free(entry->base_addr, entry->length);
            }
            entry_ptr += mmap->entry_size;
        }

        // mark the kernel image itself as used.
        u64 ks = reinterpret_cast<u64>(_kernel_start);
        u64 ke = reinterpret_cast<u64>(_kernel_end);
        mark_range_used(ks, ke - ks);

        // mark the first 1MB as reserved regardless of what the mem map says
        mark_range_used(0, 0x100000);
    }

    u64 alloc_frame() {
        for (u64 i = 0; i < MAX_FRAMES; ++i) {
            if (!is_used(i)) {
                set_used(i);
                ++used_frames;
                return i * PAGE_SIZE;
            }
        }
        serial::print("pmm: out of physical memory\n");
        asm volatile("cli; hlt");
        return 0;  // unreachable
    }

    void free_frame(u64 phys_addr) {
        u64 frame = phys_addr / PAGE_SIZE;
        if (frame >= MAX_FRAMES || !is_used(frame)) return;
        set_free(frame);
        --used_frames;
    }

    void print_stats() {
        char buf[17];
        serial::print("pmm: ");
        hex::to_string(total_frames, buf); serial::print(buf);
        serial::print(" total frames, ");
        hex::to_string(used_frames, buf);  serial::print(buf);
        serial::print(" used, ");
        hex::to_string(total_frames - used_frames, buf); serial::print(buf);
        serial::print(" free\n");
    }

}