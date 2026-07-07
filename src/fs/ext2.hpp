#pragma once

#include "lib/types.hpp"
#include "fs/vfs.hpp"

namespace ext2 {

    // try to mount an ext2 filesystem on the partition accessed by `read_fn`.
    // `read_fn` takes LBAs relative to the partition start.
    // returns a vfs::SuperBlock* on success, nullptr if not ext2.
    vfs::SuperBlock* mount(vfs::ReadBlock read_fn);

}
