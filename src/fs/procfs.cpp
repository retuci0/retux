#include "fs/procfs.hpp"

#include "mem/pmm.hpp"
#include "mem/heap.hpp"

#include "io/hpet.hpp"
#include "io/pit.hpp"

#include "lib/string.hpp"
#include "lib/types.hpp"


namespace {

    // fixed inode numbers - no dynamic node table needed for three files.
    constexpr u64 INO_ROOT     = 0;
    constexpr u64 INO_UPTIME   = 1;
    constexpr u64 INO_MEMINFO  = 2;
    constexpr u64 INO_CPUINFO  = 3;

    struct Entry { u64 ino; const char* name; };
    constexpr Entry ENTRIES[] = {
        {INO_UPTIME,  "uptime"},
        {INO_MEMINFO, "meminfo"},
        {INO_CPUINFO, "cpuinfo"},
    };
    constexpr u64 NUM_ENTRIES = sizeof(ENTRIES) / sizeof(ENTRIES[0]);

    // write value's decimal digits at `out`, return the byte count (no NUL -
    // caller tracks length). like serial::print_dec but into a buffer.
    size_t write_dec(char* out, u64 value) {
        char tmp[20];
        int i = 20;
        if (value == 0) { out[0] = '0'; return 1; }
        while (value > 0) { tmp[--i] = static_cast<char>('0' + (value % 10)); value /= 10; }
        size_t len = 20 - i;
        string::memcpy(out, tmp + i, len);
        return len;
    }

    // same HPET/PIT fallback `cpu/syscall.cpp`'s `sys_clock_gettime` uses -
    // guards against reading the OTHER driver's uninitialized state.
    u64 uptime_ms() {
        return (hpet::ticks() > 0) ? hpet::milliseconds() : pit::ticks();
    }

    // `/proc/uptime` - real Linux has two numbers (uptime, idle time); we
    // don't track idle time separately, so both fields are the same value -
    // fine, nothing that reads this file cares about the second field here.
    size_t format_uptime(char* buf, size_t cap) {
        u64 ms = uptime_ms();
        u64 secs = ms / 1000, centi = (ms % 1000) / 10;
        size_t off = 0;
        off += write_dec(buf + off, secs);
        buf[off++] = '.';
        if (centi < 10) buf[off++] = '0';
        off += write_dec(buf + off, centi);
        buf[off++] = ' ';
        off += write_dec(buf + off, secs);
        buf[off++] = '.';
        if (centi < 10) buf[off++] = '0';
        off += write_dec(buf + off, centi);
        buf[off++] = '\n';
        (void)cap;  // fixed small buffer, always fits - see mount()'s call site
        return off;
    }

    // enough fields for fastfetch's Memory module (MemTotal/MemFree at
    // minimum) - real /proc/meminfo has dozens more, not worth faking.
    size_t format_meminfo(char* buf, size_t cap) {
        u64 total_kb = pmm::total_frame_count() * 4;
        u64 free_kb  = (pmm::total_frame_count() - pmm::used_frame_count()) * 4;
        size_t off = 0;
        auto field = [&](const char* label, u64 kb) {
            size_t llen = string::strlen(label);
            string::memcpy(buf + off, label, llen); off += llen;
            off += write_dec(buf + off, kb);
            const char* suffix = " kB\n";
            size_t slen = string::strlen(suffix);
            string::memcpy(buf + off, suffix, slen); off += slen;
        };
        field("MemTotal:       ", total_kb);
        field("MemFree:        ", free_kb);
        field("MemAvailable:   ", free_kb);
        field("Buffers:        ", 0);
        field("Cached:         ", 0);
        field("SwapTotal:      ", 0);
        field("SwapFree:       ", 0);
        (void)cap;
        return off;
    }

    // one plausible "processor 0" stanza - fastfetch's CPU module mainly
    // reads "model name".
    size_t format_cpuinfo(char* buf, size_t cap) {
        const char* text =
            "processor\t: 0\n"
            "vendor_id\t: GenuineIntel\n"
            "model name\t: QEMU Virtual CPU\n"
            "cpu MHz\t\t: 2000.000\n"
            "cache size\t: 0 KB\n"
            "cpu cores\t: 1\n"
            "\n";
        size_t len = string::strlen(text);
        string::memcpy(buf, text, len);
        (void)cap;
        return len;
    }

    // renders one inode's content into a freshly kmalloc'd buffer - called
    // once per `get_inode()` (i.e. once per path lookup / open), not
    // per-read. small, fixed-size scratch is plenty for all three formats.
    struct ProcContent { char* data; u64 size; };

    ProcContent render(u64 ino) {
        char scratch[512];
        size_t len = 0;
        switch (ino) {
            case INO_UPTIME:  len = format_uptime(scratch, sizeof(scratch)); break;
            case INO_MEMINFO: len = format_meminfo(scratch, sizeof(scratch)); break;
            case INO_CPUINFO: len = format_cpuinfo(scratch, sizeof(scratch)); break;
            default: return {nullptr, 0};
        }
        char* data = static_cast<char*>(heap::kmalloc(len));
        string::memcpy(data, scratch, len);
        return {data, len};
    }

    ssize_t proc_read(vfs::Inode* inode, u64 offset, void* buf, size_t count) {
        auto* content = reinterpret_cast<ProcContent*>(inode->private_data);
        if (!content || !content->data) return -1;
        if (offset >= content->size) return 0;
        if (offset + count > content->size) count = content->size - offset;
        string::memcpy(buf, content->data + offset, count);
        return static_cast<ssize_t>(count);
    }

    ssize_t proc_readdir(vfs::Inode* inode, u64 offset, void* buf, size_t count) {
        if (inode->ino != INO_ROOT) return -1;

        u8* out = reinterpret_cast<u8*>(buf);
        size_t written = 0;
        u64 stream_pos = 0;

        for (u64 i = 0; i < NUM_ENTRIES; ++i) {
            size_t name_len = string::strlen(ENTRIES[i].name);
            size_t needed = 8 + name_len + 1;
            if (stream_pos >= offset) {
                if (written + needed > count) break;
                *reinterpret_cast<u64*>(out + written) = ENTRIES[i].ino;
                written += 8;
                string::memcpy(out + written, ENTRIES[i].name, name_len + 1);
                written += name_len + 1;
            }
            stream_pos += needed;
        }
        return static_cast<ssize_t>(written);
    }

    vfs::InodeOps s_inode_ops;
    vfs::SuperBlockOps s_sb_ops;

    vfs::Inode* make_inode(vfs::SuperBlock* sb, u64 ino) {
        auto* inode = reinterpret_cast<vfs::Inode*>(heap::kmalloc(sizeof(vfs::Inode)));
        inode->sb  = sb;
        inode->ops = &s_inode_ops;

        if (ino == INO_ROOT) {
            inode->ino = INO_ROOT;
            inode->size = 0;
            inode->is_dir = true;
            inode->private_data = nullptr;
            return inode;
        }

        ProcContent* content = static_cast<ProcContent*>(heap::kmalloc(sizeof(ProcContent)));
        *content = render(ino);
        inode->ino = ino;
        inode->size = content->size;
        inode->is_dir = false;
        inode->private_data = content;
        return inode;
    }

    vfs::Inode* proc_root_inode(vfs::SuperBlock* sb) {
        return make_inode(sb, INO_ROOT);
    }

    vfs::Inode* proc_get_inode(vfs::SuperBlock* sb, u64 ino) {
        if (ino > INO_CPUINFO) return nullptr;
        return make_inode(sb, ino);
    }

}

namespace procfs {

    vfs::SuperBlock* mount() {
        s_inode_ops.read    = proc_read;
        s_inode_ops.readdir = proc_readdir;
        s_sb_ops.root_inode = proc_root_inode;
        s_sb_ops.get_inode  = proc_get_inode;

        auto* sb = reinterpret_cast<vfs::SuperBlock*>(heap::kmalloc(sizeof(vfs::SuperBlock)));
        sb->ops = &s_sb_ops;
        sb->private_data = nullptr;
        return sb;
    }

}
