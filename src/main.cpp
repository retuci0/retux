#include "cpu/idt.hpp"
#include "cpu/tss.hpp"
#include "cpu/pic.hpp"
#include "cpu/apic.hpp"
#include "cpu/irq.hpp"

#include "io/serial.hpp"
#include "io/vga.hpp"
#include "io/pit.hpp"
#include "io/hpet.hpp"

#include "boot/acpi.hpp"

#include "memory/pmm.hpp"
#include "memory/vmm.hpp"
#include "memory/heap.hpp"


// `extern "C"` disables C++ name mangling
extern "C" void kernel_main(u64 multiboot_info_addr) {
    serial::init();
    serial::print("\n");

    idt::init();
    tss::init();

    pmm::init(multiboot_info_addr);
    vmm::remap_kernel();
    heap::init();

    // --- interrupt routing: local + I/O APIC ---
    pic::disable();

    if (!acpi::init(multiboot_info_addr)) {
        serial::print("acpi: couldn't find MADT / I-O APIC - halting\n");
        while (true) asm volatile("cli; hlt");
    }

    apic::init_lapic();
    apic::init_ioapic();
    irq::init();

    asm volatile("sti");
    serial::print("interrupts enabled (local+IO APIC)\n");

    // HPET is preferred (real hardware clock, not a rounded PIT divisor);
    // fall back to the PIT on machines without a usable HPET.
    constexpr u32 TICK_HZ = 1000;  // 1ms tick
    if (!hpet::init(TICK_HZ)) {
        pit::init(TICK_HZ);
    }

    vga::clear();
    vga::print("hello, world!\n");

    while (true) {
        asm volatile("hlt");
    }
}
