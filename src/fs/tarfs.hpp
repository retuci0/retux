#pragma once

#include "lib/types.hpp"
#include "fs/vfs.hpp"

// tarfs mounts a USTAR archive that already sits fully in memory (a
// Multiboot2 module) as a read-only VFS filesystem.
namespace tarfs {

    // parse the archive at [base, base+size) and build a directory tree over
    // it. returns a vfs::SuperBlock* on success, nullptr if bad magic on
    // the first header.
    // file reads point straight back into `base`, there is no copying.
    vfs::SuperBlock* mount(const u8* base, u64 size);

}
