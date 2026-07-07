# retux roadmap/todo

> no timeline. it gets worked on when it gets worked on. roughly ordered by dependency.

## ~~phase 1: core infrastructure~~
- [x] Multiboot2 + 64-bit long mode
- [x] real GDT (`boot.asm` bare minimum)
- [x] IDT + exception handlers (register dump on panic)
- [x] PMM (physical memory manager, bitmap)
- [x] VMM (virtual memory manager: page tables, huge-page splitting, `remap_kernel`)
- [x] heap allocator (`kmalloc`/`kfree` with coalescing)

## ~~phase 2: drivers & interrupt subsystem~~
- [x] PIC remap (legacy 8259) OR set up APIC (local + I/O). 
- [x] PIT or HPET timer driver (for preemptive multitasking ticks)
- [x] PCI bus enum (config space, find devices by class/subclass)
- [x] PS/2 Keyboard driver (scancode set 1 -> ASCII translation)
- [x] basic TTY (scrollback, backspace, basic ANSI colors)

## phase 3: storage & fs
- [x] AHCI Driver (SATA). 
      *PIO-identified reads on the HBA port first. DMA can come later.*
- [x] disk partition parsing (MBR / GPT) to find **ext2** partitions.
- [x] **VFS (virtual filesystem) layer** (abstraction: `mount`, `open`, `read`, `write`, `readdir`).
- [ ] **`initrd` support (tar)** - loaded as a Multiboot2 module so loading userspace programs before the disk driver is fully stable is possible.
- [x] **`ext2` driver** (superblock, group descriptors, inodes, indirect block traversal; no jouurnaling for now). 

## phase 4: multitasking & userspace
- [ ] preemptive multitasking (context switching, scheduler, `switch_to`).
- [ ] system call interface (syscall/sysret or `int 0x80`).
- [ ] ELF Loader (parse `Ehdr`/`Phdr`, load segments into user VMA, jump to entry).
- [ ] port a C Library (`newlib` or `mlibc`) - implement the bare-metal syscall stubs.
- [ ] implement a simple shell (`retsh`?) that can fork/exec commands from the initrd/ext2 volume.

## phase 5: polish & system services
- [ ] ACPI (FADT) - implement `reboot` and proper `shutdown` via SLP_TYP.
- [ ] `devfs` - expose `/dev/tty0`, `/dev/sda`, `/dev/null` via VFS.
- [ ] `procfs` - expose process lists and memory stats via VFS.
