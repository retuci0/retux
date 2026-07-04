#include "lib/types.hpp"
#include "lib/hex.hpp"

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

#include "memory/pmm.hpp"
#include "memory/vmm.hpp"
#include "memory/heap.hpp"

#include "dev/pci.hpp"
#include "dev/ahci.hpp"
#include "tty/tty.hpp"


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

    ahci::init();

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
