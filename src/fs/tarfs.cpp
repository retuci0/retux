#include "fs/tarfs.hpp"

#include "fs/vfs.hpp"
#include "lib/string.hpp"
#include "io/serial.hpp"
#include "mem/heap.hpp"


namespace {

    // https://www.gnu.org/software/tar/manual/html_node/Standard.html
    struct UstarHeader {
        char name[100];
        char mode[8];
        char uid[8];
        char gid[8];
        char size[12];  // octal ASCII, NUL or space terminated
        char mtime[12];
        char chksum[8];
        char typeflag;  // '0'/'\0' = file, '5' = directory, others = links etc.
        char linkname[100];
        char magic[6];  // "ustar\0" (POSIX) or "ustar  " (GNU, no NUL)
        char version[2];
        char uname[32];
        char gname[32];
        char devmajor[8];
        char devminor[8];
        char prefix[155];  // long-name prefix: full path = prefix + "/" + name
        char pad[12];
    } __attribute__((packed));

    static_assert(sizeof(UstarHeader) == 512, "USTAR header must be exactly 512 bytes");

    // parse an octal ASCII field (space/NUL padded, space/NUL terminated).
    u64 parse_octal(const char* field, size_t len) {
        u64 value = 0;
        for (size_t i = 0; i < len; ++i) {
            char c = field[i];
            if (c < '0' || c > '7') break;  // hit padding/terminator
            value = (value << 3) | static_cast<u64>(c - '0');
        }
        return value;
    }


    struct TarNode {
        char name[100];     // this node's own path component (not the full path)
        bool is_dir;
        u32  data_offset;   // byte offset into the archive of file content (files only)
        u32  size;          // file size in bytes (0 for directories)
        int  first_child;   // -1 if none
        int  next_sibling;  // -1 if none
    };

    struct TarState {
        const u8* base;
        u64       size;
        TarNode*  nodes;
        int       num_nodes;
        int       cap_nodes;
    };

    int alloc_node(TarState* st) {
        if (st->num_nodes >= st->cap_nodes) return -1;  // shouldn't happen, capacity is pre-sized
        int idx = st->num_nodes++;
        TarNode& n = st->nodes[idx];
        n.name[0]     = '\0';
        n.is_dir      = false;
        n.data_offset = 0;
        n.size        = 0;
        n.first_child = -1;
        n.next_sibling = -1;
        return idx;
    }

    // find a direct child of `parent` named `name`, or -1 if none exists yet.
    int find_child(TarState* st, int parent, const char* name) {
        int cur = st->nodes[parent].first_child;
        while (cur != -1) {
            if (string::strcmp(st->nodes[cur].name, name) == 0) return cur;
            cur = st->nodes[cur].next_sibling;
        }
        return -1;
    }

    // get (or create as an implicit directory) the child of `parent` named `name`.
    int get_or_create_child(TarState* st, int parent, const char* name) {
        int existing = find_child(st, parent, name);
        if (existing != -1) return existing;

        int idx = alloc_node(st);
        if (idx == -1) return -1;
        TarNode& n = st->nodes[idx];
        string::strncpy(n.name, name, sizeof(n.name) - 1);
        n.name[sizeof(n.name) - 1] = '\0';
        n.is_dir = true;  // assume directory until told otherwise by an explicit entry

        n.next_sibling = st->nodes[parent].first_child;
        st->nodes[parent].first_child = idx;
        return idx;
    }

    // walk/create every component of `path`, then stamp the final component
    // with whatever the tar entry actually said it was (file vs directory,
    // size, data location).
    void insert_path(TarState* st, const char* path, bool is_dir, u32 data_offset, u32 size) {
        // skip leading "./" and any leading slashes
        if (path[0] == '.' && path[1] == '/') path += 2;
        while (*path == '/') ++path;
        if (*path == '\0') return;  // "." or "/" itself - nothing to insert

        int cur = 0;  // root
        char component[100];

        while (*path) {
            size_t i = 0;
            while (*path && *path != '/' && i < sizeof(component) - 1) {
                component[i++] = *path++;
            }
            component[i] = '\0';
            while (*path == '/') ++path;  // collapse repeated/trailing slashes

            bool last = (*path == '\0');
            int child = get_or_create_child(st, cur, component);
            if (child == -1) return;  // out of node capacity, drop the rest sneakily

            if (last) {
                st->nodes[child].is_dir      = is_dir;
                st->nodes[child].data_offset = data_offset;
                st->nodes[child].size        = size;
            }
            cur = child;
        }
    }

    // count how many 512-byte headers are in the archive
    int count_entries(const u8* base, u64 size) {
        int count = 0;
        u64 offset = 0;
        while (offset + 512 <= size) {
            const auto* hdr = reinterpret_cast<const UstarHeader*>(base + offset);
            if (hdr->name[0] == '\0') break;  // end-of-archive marker
            ++count;
            u64 entry_size = parse_octal(hdr->size, sizeof(hdr->size));
            u64 data_blocks = (entry_size + 511) / 512;
            offset += 512 + data_blocks * 512;
        }
        return count;
    }

    // build the full path for a header, honouring the POSIX ustar `prefix`
    // field used for names longer than 100 bytes.
    void build_full_path(const UstarHeader* hdr, char* out, size_t out_size) {
        size_t prefix_len = string::strnlen(hdr->prefix, sizeof(hdr->prefix));
        size_t name_len   = string::strnlen(hdr->name, sizeof(hdr->name));

        size_t pos = 0;
        if (prefix_len > 0) {
            size_t n = prefix_len < out_size - 1 ? prefix_len : out_size - 1;
            string::memcpy(out, hdr->prefix, n);
            pos = n;
            if (pos < out_size - 1) out[pos++] = '/';
        }
        size_t n = name_len < out_size - 1 - pos ? name_len : out_size - 1 - pos;
        string::memcpy(out + pos, hdr->name, n);
        pos += n;
        out[pos] = '\0';
    }


    // --- VFS operation implementations ---

    vfs::InodeOps      s_inode_ops;
    vfs::SuperBlockOps s_sb_ops;

    vfs::Inode* make_vfs_inode(vfs::SuperBlock* sb, int node_idx) {
        auto* st = reinterpret_cast<TarState*>(sb->private_data);
        if (node_idx < 0 || node_idx >= st->num_nodes) return nullptr;

        auto* inode = reinterpret_cast<vfs::Inode*>(heap::kmalloc(sizeof(vfs::Inode)));
        if (!inode) return nullptr;

        // private_data is just the node index, boxed on the heap
        auto* boxed_idx = reinterpret_cast<int*>(heap::kmalloc(sizeof(int)));
        if (!boxed_idx) { heap::kfree(inode); return nullptr; }
        *boxed_idx = node_idx;

        inode->ino          = static_cast<u64>(node_idx);
        inode->size         = st->nodes[node_idx].size;
        inode->sb           = sb;
        inode->ops          = &s_inode_ops;
        inode->private_data = boxed_idx;
        return inode;
    }

    ssize_t tar_read(vfs::Inode* inode, u64 offset, void* buf, size_t count) {
        auto* st       = reinterpret_cast<TarState*>(inode->sb->private_data);
        int   node_idx = *reinterpret_cast<int*>(inode->private_data);
        const TarNode& node = st->nodes[node_idx];

        if (node.is_dir) return -1;
        if (offset >= node.size) return 0;
        if (offset + count > node.size) count = node.size - offset;

        string::memcpy(buf, st->base + node.data_offset + offset, count);
        return static_cast<ssize_t>(count);
    }

    ssize_t tar_readdir(vfs::Inode* inode, u64 offset, void* buf, size_t count) {
        auto* st       = reinterpret_cast<TarState*>(inode->sb->private_data);
        int   node_idx = *reinterpret_cast<int*>(inode->private_data);
        const TarNode& node = st->nodes[node_idx];

        if (!node.is_dir) return -1;

        u8*    out       = reinterpret_cast<u8*>(buf);
        size_t written    = 0;
        u64    stream_pos = 0;

        int child = node.first_child;
        while (child != -1) {
            const TarNode& c = st->nodes[child];
            size_t name_len = string::strlen(c.name);
            size_t needed   = 8 + name_len + 1;

            if (stream_pos >= offset) {
                if (written + needed > count) break;  // caller's buffer is full
                *reinterpret_cast<u64*>(out + written) = static_cast<u64>(child);
                written += 8;
                string::memcpy(out + written, c.name, name_len);
                out[written + name_len] = '\0';
                written += name_len + 1;
            }
            stream_pos += needed;
            child = c.next_sibling;
        }

        return static_cast<ssize_t>(written);
    }

    vfs::Inode* tar_root_inode(vfs::SuperBlock* sb) {
        return make_vfs_inode(sb, 0);
    }

    vfs::Inode* tar_get_inode(vfs::SuperBlock* sb, u64 ino) {
        return make_vfs_inode(sb, static_cast<int>(ino));
    }

}


namespace tarfs {

    vfs::SuperBlock* mount(const u8* base, u64 size) {
        if (size < 512) {
            serial::print("tarfs: archive too small to contain a header\n");
            return nullptr;
        }

        const auto* first = reinterpret_cast<const UstarHeader*>(base);
        if (string::strncmp(first->magic, "ustar", 5) != 0) {
            serial::print("tarfs: bad magic, not a USTAR archive\n");
            return nullptr;
        }

        int entry_count = count_entries(base, size);
        // generous margin: every entry might introduce several missing
        // intermediate directories, plus one slot for the root itself.
        int capacity = entry_count * 8 + 16;

        auto* st = reinterpret_cast<TarState*>(heap::kmalloc(sizeof(TarState)));
        if (!st) return nullptr;

        st->base      = base;
        st->size      = size;
        st->nodes     = reinterpret_cast<TarNode*>(heap::kmalloc(sizeof(TarNode) * capacity));
        st->num_nodes = 0;
        st->cap_nodes = capacity;
        if (!st->nodes) { heap::kfree(st); return nullptr; }

        int root = alloc_node(st);  // index 0, always the root directory
        st->nodes[root].name[0] = '\0';
        st->nodes[root].is_dir  = true;

        u64 offset = 0;
        while (offset + 512 <= size) {
            const auto* hdr = reinterpret_cast<const UstarHeader*>(base + offset);
            if (hdr->name[0] == '\0') break;  // end-of-archive marker

            u64 entry_size  = parse_octal(hdr->size, sizeof(hdr->size));
            u64 data_blocks = (entry_size + 511) / 512;
            u64 data_offset = offset + 512;

            char path[256];
            build_full_path(hdr, path, sizeof(path));
            bool is_dir = (hdr->typeflag == '5');

            // regular files ('\0' or '0') and other crap (symlinks, hardlinks,
            // device nodes, etc.) get treated as a plain file entry for now
            insert_path(st, path, is_dir,
                        is_dir ? 0 : static_cast<u32>(data_offset),
                        is_dir ? 0 : static_cast<u32>(entry_size));

            offset += 512 + data_blocks * 512;
        }

        s_inode_ops.read    = tar_read;
        s_inode_ops.readdir = tar_readdir;
        s_sb_ops.root_inode = tar_root_inode;
        s_sb_ops.get_inode  = tar_get_inode;

        auto* sb = reinterpret_cast<vfs::SuperBlock*>(heap::kmalloc(sizeof(vfs::SuperBlock)));
        if (!sb) { heap::kfree(st->nodes); heap::kfree(st); return nullptr; }

        sb->ops          = &s_sb_ops;
        sb->private_data = st;

        return sb;
    }

}
