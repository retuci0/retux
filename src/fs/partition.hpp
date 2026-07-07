#pragma once

#include "lib/types.hpp"
#include "fs/vfs.hpp"


namespace partition {

    // describes a single partition.
    struct Partition {
        u64 start_lba;
        u64 sector_count;
        u8  type;  // for MBR; for GPT we can store a GUID later
        bool valid;
    };

    // given a `read_block` function that reads from the raw device (LBA 0..),
    // scan for partitions and fill the array (max 16). returns number found.
    int scan(vfs::ReadBlock raw_read, Partition* out, int max);

    struct PartitionReadContext {
        vfs::ReadBlock raw_read;
        u64 base_lba;
    };

    // a wrapper that can be passed as a ReadBlock.
    // `user_data` will be a pointer to a PartitionReadContext.
    bool read_partition(u64 lba, u32 count, void* buf, void* user_data);

}
