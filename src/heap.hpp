#pragma once

#include <stddef.h>


namespace heap {

    // must be called once after pmm and vmm are initialized
    void init();

    // alloc `size` bytes of memory - returns pointer to allocated block
    void* kmalloc(size_t size);

    // free a block previously allocated by `kmalloc()`
    void kfree(void* ptr);
    
}