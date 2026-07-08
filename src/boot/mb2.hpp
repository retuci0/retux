#pragma once

#include "lib/types.hpp"


// Multiboot2 spec §3.6 - the boot information structure passed in RDI.
// all tags are aligned to 8 bytes and laid out consecutively in memory.
// the list ends with a terminator tag (type=0, size=8).

namespace mb2 {

    // every tag starts with this header:
    // type tells you what is is
    // size tells you, well, the size
    // so you can skip tags you don't care about
    struct Tag {
        u32 type;
        u32 size;
    } __attribute__((packed));

    // the outer wrapper around the whole boot info structure.
    struct BootInfo {
        u32 total_size;
        u32 reserved;
        Tag first_tag;  // first tag starts immediately after
    } __attribute__((packed));

    // Tag type 6 - memory map.
    // `entries` is a flexible array: real entries start at `&tag.entries[0]`
    // and continue for `(tag.size - 16) / tag.entry_size` entries.
    struct MemMapTag {
        u32 type;           // = 6
        u32 size;
        u32 entry_size;     // size of each MemMapEntry (typically 24)
        u32 entry_version;  // currently 0
    } __attribute__((packed));

    // one entry in the memory map.
    struct MemMapEntry {
        u64 base_addr;
        u64 length;
        u32 type;       // 1 = available RAM, anything else = do not use
        u32 reserved;
    } __attribute__((packed));

    // tag type 3 - boot module (Multiboot2 spec §3.6.4).
    // GRUB loads each `module2 /path arg...` line into physical memory
    // somewhere it knows is free, and hands us [mod_start, mod_end) plus
    // whatever string followed the module's path on the config line.
    // there can be more than one of these in the tag list.
    struct ModuleTag {
        u32  type;       // = 3
        u32  size;
        u32  mod_start;  // physical address, inclusive
        u32  mod_end;    // physical address, exclusive
        char cmdline[];  // null-terminated, flexible array member
    } __attribute__((packed));

    constexpr u32 TAG_MODULE   = 3;
    constexpr u32 TAG_MEMMAP   = 6;
    constexpr u32 TAG_ACPI_OLD = 14;  // ACPI 1.0 RSDP, copied verbatim after the tag header
    constexpr u32 TAG_ACPI_NEW = 15;  // ACPI >= 2.0 RSDP (extended), same idea
    constexpr u32 TAG_END      = 0;
    constexpr u32 MEMMAP_AVAIL = 1;  // the only type we can give to the PMM

    // walk the tag list starting from boot_info_addr, return a pointer to
    // the first tag of the requested type, or nullptr if not found.
    inline const Tag* find_tag(u64 boot_info_addr, u32 wanted_type) {
        const auto* info = reinterpret_cast<const BootInfo*>(boot_info_addr);
        const auto* tag  = &info->first_tag;

        while (tag->type != TAG_END) {
            if (tag->type == wanted_type) return tag;
            // tags are 8-byte aligned - round size up to next multiple of 8
            u32 aligned = (tag->size + 7) & ~7u;
            tag = reinterpret_cast<const Tag*>(
                reinterpret_cast<const u8*>(tag) + aligned
            );
        }
        return nullptr;
    }

    // walk every tag in the list, calling `visitor` on each one. the visitor
    // returns true to stop iteration early (e.g. once it's found what it
    // wanted), false to keep going. used when a tag type can repeat (modules)
    // or when a caller wants to filter on more than just the type field.
    using TagVisitor = bool (*)(const Tag* tag);

    inline void for_each_tag(u64 boot_info_addr, TagVisitor visitor) {
        const auto* info = reinterpret_cast<const BootInfo*>(boot_info_addr);
        const auto* tag  = &info->first_tag;

        while (tag->type != TAG_END) {
            if (visitor(tag)) return;
            u32 aligned = (tag->size + 7) & ~7u;
            tag = reinterpret_cast<const Tag*>(
                reinterpret_cast<const u8*>(tag) + aligned
            );
        }
    }

}
