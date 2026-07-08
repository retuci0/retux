#include "fs/ext2.hpp"

#include "fs/vfs.hpp"
#include "lib/string.hpp"
#include "lib/hex.hpp"
#include "io/serial.hpp"
#include "mem/heap.hpp"


namespace {

    // --- on-disk stucts (all little-endian) ---

    struct Ext2Superblock {
        u32 s_inodes_count;
        u32 s_blocks_count;
        u32 s_r_blocks_count;
        u32 s_free_blocks_count;
        u32 s_free_inodes_count;
        u32 s_first_data_block;   // 0 for block_size>1024, 1 for 1024-byte blocks
        u32 s_log_block_size;     // block_size = 1024 << s_log_block_size
        u32 s_log_frag_size;
        u32 s_blocks_per_group;
        u32 s_frags_per_group;
        u32 s_inodes_per_group;
        u32 s_mtime;
        u32 s_wtime;
        u16 s_mnt_count;
        u16 s_max_mnt_count;
        u16 s_magic;              // 0xEF53
        u16 s_state;
        u16 s_errors;
        u16 s_minor_rev_level;
        u32 s_lastcheck;
        u32 s_checkinterval;
        u32 s_creator_os;
        u32 s_rev_level;          // 0=original, 1=dynamic (larger inodes supported)
        u16 s_def_resuid;
        u16 s_def_resgid;
        // rev_level >= 1 fields:
        u32 s_first_ino;
        u16 s_inode_size;
        // nothing beyond here is relevant tbh
    } __attribute__((packed));

    struct Ext2BlockGroupDesc {
        u32 bg_block_bitmap;
        u32 bg_inode_bitmap;
        u32 bg_inode_table;
        u16 bg_free_blocks_count;
        u16 bg_free_inodes_count;
        u16 bg_used_dirs_count;
        u16 bg_pad;
        u32 bg_reserved[3];
    } __attribute__((packed));

    struct Ext2Inode {
        u16 i_mode;
        u16 i_uid;
        u32 i_size;
        u32 i_atime;
        u32 i_ctime;
        u32 i_mtime;
        u32 i_dtime;
        u16 i_gid;
        u16 i_links_count;
        u32 i_blocks;             // in 512-byte units
        u32 i_flags;
        u32 i_osd1;
        u32 i_block[15];          // [0..11]=direct, [12]=indirect, [13]=dbl, [14]=tpl
        u32 i_generation;
        u32 i_file_acl;
        u32 i_dir_acl;
        u32 i_faddr;
        u8  i_osd2[12];
    } __attribute__((packed));

    struct Ext2DirEntry {
        u32 inode;
        u16 rec_len;
        u8  name_len;
        u8  file_type;
        // name follows immediately, name_len bytes, not null-terminated
    } __attribute__((packed));


    // --- filesystem state (heap-allocated, lives for kernel lifetime) ---

    struct Ext2State {
        vfs::ReadBlock read;          // reads sectors relative to partition start
        u32 block_size;
        u32 sectors_per_block;
        u32 inodes_per_group;
        u32 inode_size;               // bytes; always >= 128
        u32 first_data_block;
        u32 blocks_per_group;
        u32 num_groups;

        static constexpr u32 MAX_GROUPS = 128;
        Ext2BlockGroupDesc bgdt[MAX_GROUPS];
    };

    // private data embedded in every vfs::Inode we create
    struct Ext2InodeRef {
        Ext2State* state;
        u32        ino;
        Ext2Inode  disk;  // cached on-disk inode (128 bytes)
    };

    // ops tables - assigned in ext2::mount before any inode is created
    static vfs::InodeOps    s_inode_ops;
    static vfs::SuperBlockOps s_sb_ops;

    // --- I/O stuff ---

    // read one ext2 block (block_no * sectors_per_block sectors) into buf.
    // buf must be at least block_size bytes.
    static bool read_block(Ext2State* st, u32 block_no, void* buf) {
        u64 lba = static_cast<u64>(block_no) * st->sectors_per_block;
        return st->read(lba, st->sectors_per_block, buf);
    }

    // resolve logical block index within a file to the physical ext2 block number.
    // handles direct (0-11), single-indirect (12), double-indirect (13), and
    // triple-indirect (14) blocks.
    static u32 resolve_block(Ext2State* st, const Ext2Inode& di, u32 logical) {
        const u32 ptrs_per_block = st->block_size / 4;

        // direct blocks (0-11)
        if (logical < 12) return di.i_block[logical];
        logical -= 12;

        // single indirect (block[12] points to a block of pointers)
        if (logical < ptrs_per_block) {
            u32 ind = di.i_block[12];
            if (!ind) return 0;
            u8* buf = reinterpret_cast<u8*>(heap::kmalloc(st->block_size));
            if (!buf) return 0;
            bool ok = read_block(st, ind, buf);
            u32 result = ok ? reinterpret_cast<u32*>(buf)[logical] : 0;
            heap::kfree(buf);
            return result;
        }
        logical -= ptrs_per_block;

        // double indirect (block[13] -> block of pointers -> block of pointers)
        if (logical < ptrs_per_block * ptrs_per_block) {
            u32 dind = di.i_block[13];
            if (!dind) return 0;
            u8* buf = reinterpret_cast<u8*>(heap::kmalloc(st->block_size));
            if (!buf) return 0;

            bool ok = read_block(st, dind, buf);
            u32 ind = ok ? reinterpret_cast<u32*>(buf)[logical / ptrs_per_block] : 0;
            heap::kfree(buf);
            if (!ind) return 0;

            buf = reinterpret_cast<u8*>(heap::kmalloc(st->block_size));
            if (!buf) return 0;
            ok = read_block(st, ind, buf);
            u32 result = ok ? reinterpret_cast<u32*>(buf)[logical % ptrs_per_block] : 0;
            heap::kfree(buf);
            return result;
        }
        logical -= ptrs_per_block * ptrs_per_block;

        // triple indirect (block[14] -> ptrs -> ptrs -> ptrs)
        if (logical < ptrs_per_block * ptrs_per_block * ptrs_per_block) {
            u32 tind = di.i_block[14];
            if (!tind) return 0;

            u8* buf = reinterpret_cast<u8*>(heap::kmalloc(st->block_size));
            if (!buf) return 0;
            bool ok = read_block(st, tind, buf);
            u32 dind = ok ? reinterpret_cast<u32*>(buf)[logical / (ptrs_per_block * ptrs_per_block)] : 0;
            heap::kfree(buf);
            if (!dind) return 0;
            logical %= ptrs_per_block * ptrs_per_block;

            buf = reinterpret_cast<u8*>(heap::kmalloc(st->block_size));
            if (!buf) return 0;
            ok = read_block(st, dind, buf);
            u32 ind = ok ? reinterpret_cast<u32*>(buf)[logical / ptrs_per_block] : 0;
            heap::kfree(buf);
            if (!ind) return 0;

            buf = reinterpret_cast<u8*>(heap::kmalloc(st->block_size));
            if (!buf) return 0;
            ok = read_block(st, ind, buf);
            u32 result = ok ? reinterpret_cast<u32*>(buf)[logical % ptrs_per_block] : 0;
            heap::kfree(buf);
            return result;
        }

        serial::print("ext2: logical block out of range (file too large)\n");
        return 0;
    }

    // read the disk inode numbered `ino` (1-based) into `out`
    static bool fetch_inode(Ext2State* st, u32 ino, Ext2Inode* out) {
        if (ino == 0) return false;
        u32 group = (ino - 1) / st->inodes_per_group;
        u32 local = (ino - 1) % st->inodes_per_group;
        if (group >= st->num_groups) return false;

        u32 table_block = st->bgdt[group].bg_inode_table;
        u32 byte_off    = local * st->inode_size;
        u32 blk_off     = byte_off / st->block_size;
        u32 off_in_blk  = byte_off % st->block_size;

        u8* blk = reinterpret_cast<u8*>(heap::kmalloc(st->block_size));
        if (!blk) return false;

        bool ok = read_block(st, table_block + blk_off, blk);
        if (ok) string::memcpy(out, blk + off_in_blk, sizeof(Ext2Inode));
        heap::kfree(blk);
        return ok;
    }

    // allocate and populate a vfs::Inode for ext2 inode number `ino`
    static vfs::Inode* make_vfs_inode(vfs::SuperBlock* sb, u32 ino) {
        auto* st = reinterpret_cast<Ext2State*>(sb->private_data);

        auto* ref = reinterpret_cast<Ext2InodeRef*>(heap::kmalloc(sizeof(Ext2InodeRef)));
        if (!ref) return nullptr;
        ref->state = st;
        ref->ino   = ino;
        if (!fetch_inode(st, ino, &ref->disk)) { heap::kfree(ref); return nullptr; }

        auto* inode = reinterpret_cast<vfs::Inode*>(heap::kmalloc(sizeof(vfs::Inode)));
        if (!inode) { heap::kfree(ref); return nullptr; }

        inode->ino          = ino;
        inode->size         = ref->disk.i_size;
        inode->sb           = sb;
        inode->ops          = &s_inode_ops;
        inode->private_data = ref;
        return inode;
    }

    // --- VFS operation implementations ---

    static ssize_t ext2_read(vfs::Inode* inode, u64 offset, void* buf, size_t count) {
        auto* ref   = reinterpret_cast<Ext2InodeRef*>(inode->private_data);
        auto* st    = ref->state;
        const auto& di = ref->disk;

        u32 file_size = di.i_size;
        if (offset >= file_size) return 0;
        if (offset + count > file_size) count = file_size - offset;

        u8*    dst   = reinterpret_cast<u8*>(buf);
        size_t total = 0;

        u8* blk_buf = reinterpret_cast<u8*>(heap::kmalloc(st->block_size));
        if (!blk_buf) return -1;

        while (total < count) {
            u64 file_off   = offset + total;
            u32 logical    = static_cast<u32>(file_off / st->block_size);
            u32 off_in_blk = static_cast<u32>(file_off % st->block_size);
            u32 avail      = st->block_size - off_in_blk;
            u32 to_copy    = static_cast<u32>(count - total);
            if (to_copy > avail) to_copy = avail;

            u32 phys_blk = resolve_block(st, di, logical);
            if (!phys_blk) break;

            if (!read_block(st, phys_blk, blk_buf)) break;
            string::memcpy(dst + total, blk_buf + off_in_blk, to_copy);
            total += to_copy;
        }

        heap::kfree(blk_buf);
        return static_cast<ssize_t>(total);
    }

    static ssize_t ext2_readdir(vfs::Inode* inode, u64 offset, void* buf, size_t count) {
        auto* ref      = reinterpret_cast<Ext2InodeRef*>(inode->private_data);
        auto* st       = ref->state;
        const auto& di = ref->disk;

        u8*    out        = reinterpret_cast<u8*>(buf);
        size_t written     = 0;
        u32    dir_size    = di.i_size;
        u64    stream_pos  = 0;
        bool   full        = false;

        u8* blk_buf = reinterpret_cast<u8*>(heap::kmalloc(st->block_size));
        if (!blk_buf) return -1;

        for (u32 blk_start = 0; !full && blk_start < dir_size; blk_start += st->block_size) {
            u32 logical  = blk_start / st->block_size;
            u32 phys_blk = resolve_block(st, di, logical);
            if (!phys_blk || !read_block(st, phys_blk, blk_buf)) break;

            u32 blk_end = blk_start + st->block_size;
            if (blk_end > dir_size) blk_end = dir_size;

            u32 pos = 0;
            while (!full && pos < (blk_end - blk_start)) {
                auto* de = reinterpret_cast<Ext2DirEntry*>(blk_buf + pos);
                if (de->rec_len == 0) break;

                if (de->inode != 0 && de->name_len > 0) {
                    size_t needed = 8 + de->name_len + 1;
                    if (stream_pos >= offset) {
                        if (written + needed > count) { full = true; break; }
                        *reinterpret_cast<u64*>(out + written) = de->inode;
                        written += 8;
                        string::memcpy(out + written,
                                    blk_buf + pos + sizeof(Ext2DirEntry),
                                    de->name_len);
                        out[written + de->name_len] = '\0';
                        written += de->name_len + 1;
                    }
                    stream_pos += needed;
                }
                pos += de->rec_len;
            }
        }

        heap::kfree(blk_buf);
        return static_cast<ssize_t>(written);
    }

    static vfs::Inode* ext2_root_inode(vfs::SuperBlock* sb) {
        return make_vfs_inode(sb, 2);  // root directory is always inode 2
    }

    static vfs::Inode* ext2_get_inode(vfs::SuperBlock* sb, u64 ino) {
        return make_vfs_inode(sb, static_cast<u32>(ino));
    }

}


namespace ext2 {

    vfs::SuperBlock* mount(vfs::ReadBlock read_fn) {
        // superblock is at byte offset 1024 from partition start = sectors 2-3
        u8 sb_buf[1024];
        if (!read_fn(2, 2, sb_buf)) {
            serial::print("ext2: failed to read superblock\n");
            return nullptr;
        }

        const auto* sb = reinterpret_cast<const Ext2Superblock*>(sb_buf);
        if (sb->s_magic != 0xEF53) {
            serial::print("ext2: bad magic, not ext2\n");
            return nullptr;
        }

        u32 block_size       = 1024u << sb->s_log_block_size;
        u32 sectors_per_blk  = block_size / 512;
        u32 inode_size       = (sb->s_rev_level >= 1 && sb->s_inode_size >= 128)
                            ? sb->s_inode_size : 128;
        u32 num_groups       = (sb->s_blocks_count + sb->s_blocks_per_group - 1)
                            / sb->s_blocks_per_group;

        char buf[17];
        serial::print("ext2: block_size=");
        hex::to_string(block_size, buf); serial::print(buf);
        serial::print(" groups=");
        hex::to_string(num_groups, buf); serial::print(buf);
        serial::print(" inode_size=");
        hex::to_string(inode_size, buf); serial::print(buf);
        serial::print("\n");

        if (num_groups > Ext2State::MAX_GROUPS) {
            serial::print("ext2: too many block groups (max 128)\n");
            return nullptr;
        }

        auto* state = reinterpret_cast<Ext2State*>(heap::kmalloc(sizeof(Ext2State)));
        if (!state) return nullptr;

        state->read              = read_fn;
        state->block_size        = block_size;
        state->sectors_per_block = sectors_per_blk;
        state->inodes_per_group  = sb->s_inodes_per_group;
        state->inode_size        = inode_size;
        state->first_data_block  = sb->s_first_data_block;
        state->blocks_per_group  = sb->s_blocks_per_group;
        state->num_groups        = num_groups;

        // BGDT is in the block immediately after the superblock block
        u32 bgdt_block   = sb->s_first_data_block + 1;
        u32 bgdt_bytes   = num_groups * sizeof(Ext2BlockGroupDesc);
        u32 bgdt_sectors = (bgdt_bytes + 511) / 512;
        u64 bgdt_lba     = static_cast<u64>(bgdt_block) * sectors_per_blk;

        u8* bgdt_buf = reinterpret_cast<u8*>(heap::kmalloc((bgdt_sectors + 1) * 512));
        if (!bgdt_buf) { heap::kfree(state); return nullptr; }

        if (!read_fn(bgdt_lba, bgdt_sectors, bgdt_buf)) {
            serial::print("ext2: failed to read BGDT\n");
            heap::kfree(bgdt_buf); heap::kfree(state); return nullptr;
        }
        string::memcpy(state->bgdt, bgdt_buf, bgdt_bytes);
        heap::kfree(bgdt_buf);

        // wire up ops tables
        s_inode_ops.read    = ext2_read;
        s_inode_ops.readdir = ext2_readdir;
        s_sb_ops.root_inode = ext2_root_inode;
        s_sb_ops.get_inode  = ext2_get_inode;

        auto* vfs_sb = reinterpret_cast<vfs::SuperBlock*>(heap::kmalloc(sizeof(vfs::SuperBlock)));
        if (!vfs_sb) { heap::kfree(state); return nullptr; }

        vfs_sb->ops          = &s_sb_ops;
        vfs_sb->private_data = state;

        serial::print("ext2: mounted successfully\n");
        return vfs_sb;
    }

}
