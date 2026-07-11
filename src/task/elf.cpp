#include "task/elf.hpp"

#include "fs/vfs.hpp"

#include "mem/pmm.hpp"
#include "mem/vmm.hpp"
#include "mem/heap.hpp"

#include "lib/string.hpp"
#include "lib/types.hpp"

#include "io/serial.hpp"


namespace {

    // --- ELF64 on-disk structures (System V ABI) ---

    struct Elf64_Ehdr {
        u8  e_ident[16];
        u16 e_type;
        u16 e_machine;
        u32 e_version;
        u64 e_entry;
        u64 e_phoff;
        u64 e_shoff;
        u32 e_flags;
        u16 e_ehsize;
        u16 e_phentsize;
        u16 e_phnum;
        u16 e_shentsize;
        u16 e_shnum;
        u16 e_shstrndx;
    } __attribute__((packed));

    struct Elf64_Phdr {
        u32 p_type;
        u32 p_flags;
        u64 p_offset;
        u64 p_vaddr;
        u64 p_paddr;
        u64 p_filesz;
        u64 p_memsz;
        u64 p_align;
    } __attribute__((packed));

    constexpr u8 ELFMAG0 = 0x7F, ELFMAG1 = 'E', ELFMAG2 = 'L', ELFMAG3 = 'F';
    constexpr u8 EI_CLASS = 4, EI_DATA = 5;
    constexpr u8 ELFCLASS64  = 2;
    constexpr u8 ELFDATA2LSB = 1;

    constexpr u16 ET_EXEC    = 2;
    constexpr u16 EM_X86_64  = 62;

    constexpr u32 PT_LOAD = 1;
    constexpr u32 PF_X = 1, PF_W = 2;

    constexpr u64 PAGE_SIZE = 0x1000;

    inline u64 page_down(u64 x) { return x & ~(PAGE_SIZE - 1); }
    inline u64 page_up(u64 x)   { return (x + PAGE_SIZE - 1) & ~(PAGE_SIZE - 1); }

    // map and fill every page of one PT_LOAD: fresh PMM frames, zeroed, then
    // the covered slice copied from file_data - the p_memsz-beyond-p_filesz
    // tail (bss) copies nothing and stays zero.
    bool load_segment(const u8* file_data, u64 file_size, const Elf64_Phdr& ph) {
        if (ph.p_filesz > ph.p_memsz) return false;
        if (ph.p_offset > file_size || ph.p_filesz > file_size - ph.p_offset) return false;

        u64 flags = vmm::PRESENT | vmm::USER;
        if (ph.p_flags & PF_W)          flags |= vmm::WRITABLE;
        if (!(ph.p_flags & PF_X))       flags |= vmm::NO_EXECUTE;

        u64 seg_start    = ph.p_vaddr;
        u64 seg_file_end = ph.p_vaddr + ph.p_filesz;

        u64 va     = page_down(ph.p_vaddr);
        u64 va_end = page_up(ph.p_vaddr + ph.p_memsz);

        for (; va < va_end; va += PAGE_SIZE) {
            u64 phys = pmm::alloc_frame();
            vmm::map(va, phys, flags);

            // physmap, not a raw cast - the fresh frame isn't identity-mapped
            // in the calling task's (private) address space.
            u8* kptr = reinterpret_cast<u8*>(vmm::phys_to_virt(phys));
            string::memset(kptr, 0, PAGE_SIZE);

            u64 page_end    = va + PAGE_SIZE;
            u64 copy_start  = seg_start    > va       ? seg_start    : va;
            u64 copy_end    = seg_file_end < page_end ? seg_file_end : page_end;
            if (copy_end > copy_start) {
                u64 file_off = ph.p_offset + (copy_start - seg_start);
                u64 len      = copy_end - copy_start;
                string::memcpy(kptr + (copy_start - va), file_data + file_off, len);
            }
        }

        return true;
    }

    bool parse_and_load(const u8* data, u64 size, elf::LoadResult* out) {
        if (size < sizeof(Elf64_Ehdr)) {
            serial::print("elf: file too small for an ELF header\n");
            return false;
        }

        const auto* eh = reinterpret_cast<const Elf64_Ehdr*>(data);

        if (eh->e_ident[0] != ELFMAG0 || eh->e_ident[1] != ELFMAG1 ||
            eh->e_ident[2] != ELFMAG2 || eh->e_ident[3] != ELFMAG3) {
            serial::print("elf: bad magic\n");
            return false;
        }
        if (eh->e_ident[EI_CLASS] != ELFCLASS64 || eh->e_ident[EI_DATA] != ELFDATA2LSB) {
            serial::print("elf: not a 64-bit little-endian ELF\n");
            return false;
        }
        if (eh->e_type != ET_EXEC) {
            serial::print("elf: not an ET_EXEC executable (PIE/ET_DYN unsupported)\n");
            return false;
        }
        if (eh->e_machine != EM_X86_64) {
            serial::print("elf: wrong machine type (not x86-64)\n");
            return false;
        }
        if (eh->e_phoff == 0 || eh->e_phnum == 0) {
            serial::print("elf: no program headers\n");
            return false;
        }
        // widen to u64 before multiplying - e_phnum * e_phentsize can't
        // overflow a u64 (both are u16), but keeps the addition below honest.
        u64 phdrs_bytes = static_cast<u64>(eh->e_phnum) * eh->e_phentsize;
        if (eh->e_phentsize < sizeof(Elf64_Phdr) || eh->e_phoff > size ||
            phdrs_bytes > size - eh->e_phoff) {
            serial::print("elf: program headers run past EOF\n");
            return false;
        }

        u64 phdr_vaddr   = 0;
        u64 highest_addr = 0;

        for (u16 i = 0; i < eh->e_phnum; ++i) {
            const auto* ph = reinterpret_cast<const Elf64_Phdr*>(
                data + eh->e_phoff + static_cast<u64>(i) * eh->e_phentsize);
            if (ph->p_type != PT_LOAD) continue;
            if (!load_segment(data, size, *ph)) {
                serial::print("elf: failed to load a PT_LOAD segment\n");
                return false;
            }

            // AT_PHDR wants the vaddr the program header table ended up
            // loaded at, not its file offset - find the PT_LOAD segment
            // whose file range contains e_phoff and translate through it.
            if (eh->e_phoff >= ph->p_offset &&
                eh->e_phoff - ph->p_offset < ph->p_filesz) {
                phdr_vaddr = ph->p_vaddr + (eh->e_phoff - ph->p_offset);
            }

            u64 seg_end = page_up(ph->p_vaddr + ph->p_memsz);
            if (seg_end > highest_addr) highest_addr = seg_end;
        }

        out->entry        = eh->e_entry;
        out->phdr_vaddr   = phdr_vaddr;
        out->phnum        = eh->e_phnum;
        out->phentsize    = eh->e_phentsize;
        out->highest_addr = highest_addr;
        return true;
    }

}

namespace elf {

    bool load(const char* path, LoadResult* out) {
        vfs::File* file = vfs::open(path);
        if (!file) {
            serial::print("elf: couldn't open "); serial::print(path); serial::print("\n");
            return false;
        }

        size_t size = 0;
        u8* data = vfs::read_whole_file(file, &size);
        heap::kfree(file);
        if (!data) {
            serial::print("elf: couldn't read "); serial::print(path); serial::print("\n");
            return false;
        }

        bool ok = parse_and_load(data, size, out);
        heap::kfree(data);
        return ok;
    }

}
