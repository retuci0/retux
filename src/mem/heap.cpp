#include "mem/heap.hpp"

#include "mem/pmm.hpp"
#include "mem/vmm.hpp"

#include "lib/types.hpp"

#include "io/serial.hpp"

#include <stddef.h>


namespace {

    // --- heap virtual address range ---
    constexpr u64 HEAP_START = 0xFFFF800000000000ULL;
    constexpr u64 HEAP_MAX   = 0xFFFF8000FFFFFFFFULL;   // 4 GiB total

    constexpr u64 PAGE_SIZE = 4096;
    constexpr u64 ALIGNMENT = 8;  // align allocs to 8 bytes

    // --- block metadata ---
    // each block has a header (size + free flag) and a footer (size + free flag).
    // free blocks also store a `next` pointer inside the data area.
    struct Block {
        u64 size;   // total size of block (including header, data area, footer)
        bool free;  // 1 = free; 0 = allocated
        // (padding may be inserted by compiler but ultimately we control alignment)
    };

    struct Footer {
        u64 size;
        bool free;
    };

    // minimum block size: header + next pointer + footer
    // the next pointer is stored at the beginning of the data area
    constexpr size_t MIN_BLOCK = sizeof(Block) + sizeof(Block*) + sizeof(Footer);

    static_assert(MIN_BLOCK % ALIGNMENT == 0, "MIN_BLOCK not aligned");


    // --- helpers ---

    // get `Block*` from raw data
    inline Block* block_from_data(void* data) {
        return (Block*) ((char*) data - sizeof(Block));
    }

    // get raw data from `Block*`
    inline void* data_from_block(Block* block) {
        return (char*) block + sizeof(Block);
    }

    // get footer of `Block*`
    inline Footer* footer_of_block(Block* block) {
        return (Footer*) ((char*) block + block->size - sizeof(Footer));
    }

    // get next `Block*`
    inline Block* next_block(Block* block) {
        return (Block*) ((char*) block + block->size);
    }

    // get previous `Block*`
    inline Block* prev_block(Block* block) {
        // do not access memory outside the heap
        if ((char*)block - sizeof(Footer) < (char*)HEAP_START)
            return nullptr;
        Footer* footer = (Footer*) ((char*) block - sizeof(Footer));
        u64 prev_size = footer->size;
        if ((char*) block - prev_size < (char*) HEAP_START) {
            return nullptr;
        }
        return (Block*) ((char*) block - prev_size);
    }

    // set next `Block*`
    inline void set_next(Block* block, Block* next) {
        *((Block**) ((char*) block + sizeof(Block))) = next;
    }

    // get next `Block*`
    inline Block* get_next(Block* block) {
        return *((Block**) ((char*) block + sizeof(Block)));
    }

    Block* free_list = nullptr;
    u64    heap_end  = HEAP_START;

    // add a block to the free list
    void add_block_to_free_list(Block* block) {
        block->free = true;
        set_next(block, free_list);
        free_list = block;
    }

    // remove a block from the free list
    void remove_block_from_free_list(Block* block, Block* prev) {
        if (prev) {
            set_next(prev, get_next(block));
        } else {
            free_list = get_next(block);
        }
    }

    void coalesce(Block* block) {
        // merge with next block
        Block* next = next_block(block);
        if ((char*) next < (char*) heap_end && next->free) {
            // remove next from free list
            Block* prev = nullptr;
            for (Block* curr = free_list; curr; prev = curr, curr = get_next(curr)) {
                if (curr == next) {
                    remove_block_from_free_list(curr, prev);
                    break;
                }
            }
            // expand current block
            block->size += next->size;
            // update footer of current block
            footer_of_block(block)->size = block->size;
            footer_of_block(block)->free = true;
        }

        // merge with previous block
        Block* prev = prev_block(block);
        if (prev && prev->free) {
            // remove prev from free list
            Block* pprev = nullptr;
            for (Block* cur = free_list; cur; pprev = cur, cur = get_next(cur)) {
                if (cur == prev) {
                    remove_block_from_free_list(cur, pprev);
                    break;
                }
            }
            // expand previous block
            prev->size += block->size;
            footer_of_block(prev)->size = prev->size;
            footer_of_block(prev)->free = true;
            block = prev;  // merged block is now `prev`
        }

        // add the merged block to the free list
        block->free = true;
        set_next(block, free_list);
        free_list = block;
    }

    // grow the heap by allocating `pages` additional 4KB pages, map them,
    // and create a new free block covering them.
    void grow_heap(size_t pages) {
        u64 virt = heap_end;
        if (virt + pages * PAGE_SIZE > HEAP_MAX) {
            serial::print("heap: out of virtual address space\n");
            while (1) asm("hlt");  // halt
        }

        for (size_t i = 0; i < pages; ++i) {
            u64 phys = pmm::alloc_frame();
            if (!phys) {
                serial::print("heap: out of physical memory\n");
                while (1) asm("hlt");  // halt
            }
            vmm::map(virt + i * PAGE_SIZE, phys, vmm::KERNEL_RW);
        }

        heap_end = virt + pages * PAGE_SIZE;   // update before coalesce

        Block* new_block = (Block*) virt;
        new_block->size = pages * PAGE_SIZE;
        new_block->free = true;
        footer_of_block(new_block)->size = new_block->size;
        footer_of_block(new_block)->free = true;

        coalesce(new_block);  // add the merged block to the free list
    }

    // allocate a block of at least `size` bytes (including metadata)
    // returns the block or nullptr if none found
    Block* allocate_block(size_t size) {
        size_t total = size + sizeof(Block) + sizeof(Footer);
        // align total to `ALIGNMENT`
        total = (total + ALIGNMENT - 1) & ~(ALIGNMENT - 1);
        if (total < MIN_BLOCK) total = MIN_BLOCK;

        Block* prev = nullptr;
        for (Block* curr = free_list; curr; prev = curr, curr = get_next(curr)) {
            if (curr->size >= total) {
                // suitable block found
                remove_block_from_free_list(curr, prev);

                // split if the leftover space can form a new block
                size_t remaining = curr->size - total;
                if (remaining >= MIN_BLOCK) {
                    Block* new_block = (Block*) ((char*) curr + total);
                    new_block->size = remaining;
                    new_block->free = true;
                    footer_of_block(new_block)->size = remaining;
                    footer_of_block(new_block)->free = true;
                    add_block_to_free_list(new_block);
                    curr->size = total;
                    footer_of_block(curr)->size = total;
                }

                // mark as allocated
                curr->free = false;
                footer_of_block(curr)->free = false;
                return curr;
            }
        }
        return nullptr;  // no free block found
    }

}


// --- public API ---
namespace heap {

    void init() {
        // allocate an initial 1MiB of heap
        grow_heap(256);  // 256 pages
    }

    void* kmalloc(size_t size) {
        if (size == 0) return nullptr;

        Block* block = allocate_block(size);
        if (!block) {
            // no free block: grow the heap
            size_t total_needed = size + sizeof(Block) + sizeof(Footer);
            size_t pages_needed = (total_needed + PAGE_SIZE - 1) / PAGE_SIZE;
            // grow by at least 4 pages at a time to reduce overhead
            size_t pages = (pages_needed > 4) ? pages_needed : 4;
            grow_heap(pages);
            block = allocate_block(size);
            if (!block) {
                serial::print("heap: kmalloc failed after growing heap\n");
                while (1) asm("hlt");
                return nullptr;
            }
        }

        return data_from_block(block);
    }

    void kfree(void* ptr) {
        if (!ptr) return;
        Block* block = block_from_data(ptr);
        if (!block->free) {
            block->free = true;
            footer_of_block(block)->free = true;
            coalesce(block);
        }
    }

}
