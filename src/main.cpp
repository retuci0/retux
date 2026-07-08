#include "lib/types.hpp"
#include "lib/hex.hpp"

#include "boot/mb2.hpp"

#include "cpu/idt.hpp"
#include "cpu/tss.hpp"
#include "cpu/pic.hpp"
#include "cpu/apic.hpp"
#include "cpu/irq.hpp"

#include "io/serial.hpp"
#include "io/vga.hpp"
#include "io/pit.hpp"
#include "io/hpet.hpp"
#include "io/keyboard.hpp"

#include "boot/acpi.hpp"

#include "mem/pmm.hpp"
#include "mem/vmm.hpp"
#include "mem/heap.hpp"

#include "dev/pci.hpp"
#include "dev/ahci.hpp"
#include "tty/tty.hpp"

#include "fs/vfs.hpp"
#include "lib/string.hpp"


extern "C" void kernel_main(u64 multiboot_info_addr) {
    serial::init();
    serial::print("\n");

    idt::init();
    tss::init();

    pmm::init(multiboot_info_addr);
    vmm::remap_kernel();
    heap::init();

    // --- interrupt routing ---
    pic::disable();

    if (!acpi::init(multiboot_info_addr)) {
        serial::print("acpi: couldn't find MADT / IO APIC - halting\n");
        while (true) asm volatile("cli; hlt");
    }

    apic::init_lapic();
    apic::init_ioapic();
    irq::init();

    asm volatile("sti");
    serial::print("interrupts enabled (local+IO APIC)\n");

    // --- timer: HPET or PIT ---
    constexpr u32 TICK_HZ = 1000;
    if (!hpet::init(TICK_HZ)) {
        pit::init(TICK_HZ);
    }

    tty::init();
    keyboard::init();

    serial::print("pci: scanning...\n");
    // just log all devices (for fun)
    pci::enumerate([](const pci::Device& d) {
        serial::print("pci: ");
        char buf[17];
        hex::to_string(d.vendor_id, buf); serial::print(buf);
        serial::print(":");
        hex::to_string(d.device_id, buf); serial::print(buf);
        serial::print(" class=");
        hex::to_string(d.class_code, buf); serial::print(buf);
        serial::print(".");
        hex::to_string(d.subclass, buf); serial::print(buf);
        serial::print("\n");
        return false;
    });

    // check for an initrd first and only fall back to scanning the disk for ext2 if it's absent
    bool have_initrd = false;
    mb2::for_each_tag(multiboot_info_addr, [](const mb2::Tag* tag) -> bool {
        if (tag->type != mb2::TAG_MODULE) return false;
        const auto* mod = reinterpret_cast<const mb2::ModuleTag*>(tag);

        serial::print("multiboot: module \""); serial::print(mod->cmdline);
        serial::print("\" at 0x");
        char buf[17];
        hex::to_string(mod->mod_start, buf); serial::print(buf);
        serial::print(" - 0x");
        hex::to_string(mod->mod_end, buf); serial::print(buf);
        serial::print("\n");

        // safe to dereference
        const u8* base = reinterpret_cast<const u8*>(static_cast<u64>(mod->mod_start));
        u64 size = mod->mod_end - mod->mod_start;

        vfs::SuperBlock* sb = vfs::mount_initrd(base, size);
        if (sb) {
            vfs::root_sb = sb;
            return true;
        }
        return false;
    });
    have_initrd = (vfs::root_sb != nullptr);

    if (!have_initrd) {
        ahci::init();
        vfs::init(ahci::read_sectors);
    }

    if (vfs::root_sb) {
        // list the root directory (for fun, and to prove the fs actually works)
        vfs::Inode* root = vfs::root_sb->ops->root_inode(vfs::root_sb);
        if (root && root->ops && root->ops->readdir) {
            serial::print("vfs: root directory:\n");
            u8 buf[512];
            u64 offset = 0;
            while (true) {
                ssize_t bytes = root->ops->readdir(root, offset, buf, sizeof(buf));
                if (bytes <= 0) break;
                u8* p = buf;
                while (p < buf + bytes) {
                    p += 8;  // skip inode number, we just want the name here
                    const char* name = reinterpret_cast<const char*>(p);
                    size_t len = string::strnlen(name, sizeof(buf) - (p - buf));
                    serial::print("  "); serial::print(name); serial::print("\n");
                    p += len + 1;
                }
                offset += bytes;
            }
        }
    }


    tty::print("\n=== retux kernel ready ===\n");
    tty::print("type something! alt+F1..F4 to switch VTs.\n\n");

    // echo keys
    while (true) {
        char c = keyboard::getchar();
        if (c) {
            tty::print(c);
        }
        asm volatile("hlt");
    }
}
