#include "fs/vfs.hpp"

#include "fs/ext2.hpp"
#include "fs/partition.hpp"

#include "lib/string.hpp"
#include "lib/hex.hpp"

#include "io/serial.hpp"
#include "memory/heap.hpp"


namespace vfs {

    SuperBlock* root_sb = nullptr;

    // split a path into components and traverse.
    static Inode* lookup_path(Inode* root, const char* path) {
        if (!root || !path) return nullptr;

        // skip leading slashes
        while (*path == '/') ++path;
        if (*path == '\0') {
            // root directory itself
            return root;
        }

        Inode* cur = root;
        char component[256];

        while (*path) {
            // copy next component
            size_t i = 0;
            while (*path && *path != '/' && i < sizeof(component)-1) {
                component[i++] = *path++;
            }
            component[i] = '\0';
            if (*path == '/') ++path;  // skip slash

            // assume cur is a directory and has readdir ops.
            if (!cur->ops || !cur->ops->readdir) {
                serial::print("vfs: not a directory\n");
                return nullptr;
            }

            // read all entries. allocate a temporary buffer.
            // read in chunks of 512 bytes (or page size).
            const size_t chunk_size = 512;
            u8 buf[chunk_size];
            u64 offset = 0;
            bool found = false;
            u64  found_ino = 0;

            while (true) {
                ssize_t bytes = cur->ops->readdir(cur, offset, buf, chunk_size);
                if (bytes <= 0) break;
                // define a simple dirent format: first 8 bytes = inode number (u64), then name null-terminated.
                // the fs driver must fill that.
                u8* p = buf;
                while (p < buf + bytes) {
                    u64 ino = *(u64*)p;
                    p += 8;
                    const char* name = (const char*)p;
                    size_t name_len = string::strnlen(name, chunk_size - (p - buf));
                    if (string::strcmp(name, component) == 0) {
                        found_ino = ino;
                        found = true;
                        break;
                    }
                    p += name_len + 1;  // null-terminated
                }
                if (found) break;
                offset += bytes;
            }

            if (!found) {
                serial::print("vfs: component '"); serial::print(component); serial::print("' not found\n");
                return nullptr;
            }

            // resolve the matched name to an actual inode via the owning superblock.
            SuperBlock* sb = cur->sb;
            if (!sb || !sb->ops || !sb->ops->get_inode) {
                serial::print("vfs: superblock has no get_inode op\n");
                return nullptr;
            }

            cur = sb->ops->get_inode(sb, found_ino);
            if (!cur) {
                serial::print("vfs: failed to get inode for \""); serial::print(component); serial::print("\"\n");
                return nullptr;
            }
        }

        return cur;  // final inode
    }


    namespace {

        // vfs::ReadBlock is a plain function pointer with no context slot, but
        // reading a partition needs to know which raw_read to use and where the
        // partition starts. stash that here and hand ext2 a small adapter that
        // closes over it via partition::read_partition(). only one filesystem
        // is ever mounted at a time (root_sb), so a single static context is fine.
        partition::PartitionReadContext s_part_ctx;

        bool partition_relative_read(u64 lba, u32 count, void* buf) {
            return partition::read_partition(lba, count, buf, &s_part_ctx);
        }

    }

    SuperBlock* mount(ReadBlock read_block, u64 part_lba) {
        s_part_ctx.raw_read = read_block;
        s_part_ctx.base_lba = part_lba;
        return ext2::mount(partition_relative_read);
    }

    File* open(const char* path) {
        if (!root_sb || !root_sb->ops || !root_sb->ops->root_inode) return nullptr;
        Inode* root = root_sb->ops->root_inode(root_sb);
        if (!root) return nullptr;

        Inode* target = lookup_path(root, path);
        if (!target) return nullptr;

        // create a file object
        File* file = (File*)heap::kmalloc(sizeof(File));
        if (!file) return nullptr;
        file->inode = target;
        file->offset = 0;
        static FileOps default_ops;
        default_ops.read = [](File* f, void* buf, size_t count) -> ssize_t {
            if (!f->inode->ops || !f->inode->ops->read) return -1;
            ssize_t ret = f->inode->ops->read(f->inode, f->offset, buf, count);
            if (ret > 0) f->offset += ret;
            return ret;
        };
        file->ops = &default_ops;
        return file;
    }

    u8* read_whole_file(File* file, size_t* out_size) {
        if (!file || !file->ops || !file->ops->read) return nullptr;
        // allocate buffer for entire file size (plus one because i'm paranoid)
        size_t size = file->inode->size;
        u8* buf = (u8*)heap::kmalloc(size + 1);
        if (!buf) return nullptr;
        size_t total = 0;
        while (total < size) {
            ssize_t ret = file->ops->read(file, buf + total, size - total);
            if (ret <= 0) {
                heap::kfree(buf);
                return nullptr;
            }
            total += ret;
        }
        buf[size] = '\0';
        *out_size = size;
        return buf;
    }

    void init(ReadBlock raw_read) {
        partition::Partition parts[16];
        int n = partition::scan(raw_read, parts, 16);

        for (int i = 0; i < n; ++i) {
            if (!parts[i].valid) continue;
            serial::print("vfs: probing partition "); serial::print_dec(i); serial::print(" for ext2...\n");
            SuperBlock* sb = mount(raw_read, parts[i].start_lba);
            if (sb) {
                root_sb = sb;
                return;
            }
        }

        serial::print("vfs: trying whole-disk ext2 (no partition table)...\n");
        SuperBlock* sb = mount(raw_read, 0);
        if (sb) {
            root_sb = sb;
            return;
        }

        serial::print("vfs: no mountable filesystem found\n");
    }

}
