#pragma once

#include "lib/types.hpp"


namespace vfs {

    struct SuperBlock;
    struct Inode;
    struct File;

    struct SuperBlockOps {
        Inode* (*root_inode)(SuperBlock* sb);
        // look up inode by number, required for path traversal
        Inode* (*get_inode)(SuperBlock* sb, u64 ino);
    };

    struct InodeOps {
        // read file data at byte offset; returns bytes read or -1 on error
        ssize_t (*read)(Inode* inode, u64 offset, void* buf, size_t count);
        // fill buf with VFS-format directory entries starting at offset;
        // each entry: [u64 inode_no][null-terminated name]. returns bytes
        // written, 0 when exhausted, -1 on error.
        ssize_t (*readdir)(Inode* inode, u64 offset, void* buf, size_t count);
    };

    struct FileOps {
        ssize_t (*read)(File* file, void* buf, size_t count);
    };

    struct Inode {
        u64 ino;
        u64 size;
        bool is_dir;  // populated by every driver's inode constructor
                      // (ext2::make_vfs_inode, tarfs's) - `fstat` (cpu/
                      // syscall.cpp) needs it for S_IFDIR vs S_IFREG.
        SuperBlock* sb;
        InodeOps*   ops;
        void*       private_data;
    };

    struct SuperBlock {
        SuperBlockOps* ops;
        void*          private_data;
    };

    struct File {
        Inode*   inode;
        u64      offset;
        FileOps* ops;
    };

    // reads `count` 512-byte sectors from `lba` (relative to partition start) into `buf`
    using ReadBlock = bool (*)(u64 lba, u32 count, void* buf);

    extern SuperBlock* root_sb;

    // try to mount the partition exposed by `read_block` as a known filesystem.
    // returns a SuperBlock on success, nullptr if unrecognised.
    SuperBlock* mount(ReadBlock read_block, u64 part_lba);

    // mount an in-memory tar (USTAR) image - i.e. an initrd loaded as a
    // Multiboot2 module - as a filesystem. `base` must remain valid for the
    // lifetime of the mount. returns nullptr if it's not a valid archive.
    SuperBlock* mount_initrd(const u8* base, u64 size);

    // mount `sb` at a fixed prefix (e.g. "/proc"); open() checks this small
    // table before root_sb. no unmounting, no nested prefixes.
    void mount_at(const char* prefix, SuperBlock* sb);

    // open a file by absolute path. returns File* or nullptr.
    File* open(const char* path);

    // read the whole file into a heap-allocated buffer (caller kfree()s it).
    u8* read_whole_file(File* file, size_t* out_size);

    // scan the disk for partitions, try to mount ext2 on each, set root_sb.
    // `raw_read` reads sectors from the whole disk (LBA 0 = start of disk).
    void init(ReadBlock raw_read);

}
