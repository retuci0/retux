#include "lib/types.hpp"

#include "cpu/cpu.hpp"
#include "cpu/idt.hpp"
#include "cpu/tss.hpp"
#include "cpu/pic.hpp"
#include "cpu/apic.hpp"
#include "cpu/irq.hpp"
#include "cpu/syscall.hpp"
#include "cpu/fpu.hpp"

#include "io/serial.hpp"
#include "io/pit.hpp"
#include "io/hpet.hpp"
#include "io/keyboard.hpp"

#include "boot/acpi.hpp"
#include "boot/mb2.hpp"

#include "mem/heap.hpp"
#include "mem/pmm.hpp"
#include "mem/vmm.hpp"

#include "dev/ahci.hpp"
#include "tty/tty.hpp"

#include "fs/vfs.hpp"
#include "fs/procfs.hpp"

#include "task/sched.hpp"
#include "task/user.hpp"




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

    // must run before sched::init()
    fpu::init();

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
        hpet::set_tick_callback(sched::tick);
        serial::print("timer: HPET\n");
    } else {
        pit::init(TICK_HZ);
        pit::set_tick_callback(sched::tick);
        serial::print("timer: PIT\n");
    }

    // scheduler: wraps this very context (kernel_main) as the "boot" task,
    // spawns a dedicated idle task, and hooks itself into the post-EOI
    // path so timer ticks can actually preempt something.
    sched::init();
    irq::set_post_eoi_hook(sched::maybe_reschedule);

    // per-CPU scratch (IA32_KernelGSBase) MUST be set BEFORE syscall::init
    // enables SYSCALL/SYSRET - the entry stub's first instruction is
    // `swapgs`, which would swap in garbage otherwise.
    cpu::init();
    syscall::init();

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

    vfs::mount_at("/proc", procfs::mount());

    serial::print("retux: ready\n");

    tty::print("\n=== retux kernel ready ===\n");
    tty::print("alt+F1..F4 to switch VTs.\n\n");

    task::user::spawn_from_elf("/bin/retsh");

    // kernel_main's context is the "boot" task now - nothing left to do, so
    // it just idles, still scheduled a slice at a time like anything else.
    while (true) {
        asm volatile("hlt");
    }
}
