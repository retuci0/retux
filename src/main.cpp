#include "lib/types.hpp"
#include "boot/mb2.hpp"

#include "cpu/idt.hpp"
#include "cpu/tss.hpp"
#include "cpu/pic.hpp"
#include "cpu/apic.hpp"
#include "cpu/irq.hpp"

#include "io/serial.hpp"
#include "io/pit.hpp"
#include "io/hpet.hpp"
#include "io/keyboard.hpp"

#include "boot/acpi.hpp"

#include "mem/pmm.hpp"
#include "mem/vmm.hpp"
#include "mem/heap.hpp"

#include "dev/ahci.hpp"
#include "tty/tty.hpp"

#include "fs/vfs.hpp"


// `extern "C"` disables name mangling
extern "C" void kernel_main(u64 multiboot_info_addr) {
    serial::init();
    serial::print("\nretux: booting\n");

    idt::init();

    tss::init();

    pmm::init(multiboot_info_addr);

    vmm::remap_kernel();

    heap::init();
    serial::print("boot: cpu and memory ready\n");

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
    serial::print("boot: interrupts ready\n");

    // --- timer: HPET or PIT ---
    constexpr u32 TICK_HZ = 1000;
    if (hpet::init(TICK_HZ)) {
        serial::print("timer: HPET\n");
    } else {
        pit::init(TICK_HZ);
        serial::print("timer: PIT\n");
    }

    tty::init();
    keyboard::init();
    serial::print("boot: console ready\n");

    // check for an initrd first and only fall back to scanning the disk for ext2 if it's absent
    bool have_initrd = false;
    mb2::for_each_tag(multiboot_info_addr, [](const mb2::Tag* tag) -> bool {
        if (tag->type != mb2::TAG_MODULE) return false;
        const auto* mod = reinterpret_cast<const mb2::ModuleTag*>(tag);

        const u8* base = reinterpret_cast<const u8*>(static_cast<u64>(mod->mod_start));
        u64 size = mod->mod_end - mod->mod_start;

        vfs::SuperBlock* sb = vfs::mount_initrd(base, size);
        if (sb) {
            vfs::root_sb = sb;
            serial::print("fs: initrd mounted\n");
            return true;
        }
        return false;
    });
    have_initrd = (vfs::root_sb != nullptr);

    if (!have_initrd) {
        serial::print("fs: no initrd, probing disk\n");
        ahci::init();
        vfs::init(ahci::read_sectors);
    }

    serial::print("retux: ready\n");

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
