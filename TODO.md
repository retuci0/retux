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

## ~~phase 3: storage & fs~~
- [x] AHCI Driver (SATA). 
      *PIO-identified reads on the HBA port first. DMA can come later.*
- [x] disk partition parsing (MBR / GPT) to find **ext2** partitions.
- [x] **VFS (virtual filesystem) layer** (abstraction: `mount`, `open`, `read`, `write`, `readdir`).
- [x] **`initrd` support (tar)** - loaded as a Multiboot2 module so loading userspace programs before the disk driver is fully stable is possible.
- [x] **`ext2` driver** (superblock, group descriptors, inodes, indirect block traversal; no jouurnaling for now). 

## phase 4: multitasking & userspace
- [x] preemptive multitasking (context switching, scheduler, `switch_to`).
- [x] system call interface (syscall/sysret or `int 0x80`).
- [x] ELF Loader (parse `Ehdr`/`Phdr`, load segments into user VMA, jump to entry).
- [x] ~~port a C Library (`newlib` or `mlibc`)~~ - went further: retux's
      syscall ABI is now Linux-compatible enough (real syscall numbers,
      `arch_prctl`/TLS, `brk`/`mmap`, file I/O, `uname` claiming "Linux") to
      run real statically-linked, non-PIE **musl** binaries unmodified,
      built via `testbins/` - no libc port needed since we run upstream
      musl directly. SSE had to be enabled (`boot.asm`) - real compiler
      output assumes SSE2 baseline. still `ET_EXEC`-only (no PIE / dynamic
      linking) as of this writing.
- [x] per-task address spaces (CR3) - `vmm::create_address_space()` gives
      each Linux task its own private PML4, sharing only the kernel image,
      heap, and a new physmap (`vmm::phys_to_virt` - physical RAM mapped
      identically in every address space, needed once "physical address
      doubles as a kernel pointer" can no longer lean on the low-4GiB
      identity map alone). two Linux binaries now run genuinely
      concurrently, both using the same fixed `0x400000` etc without
      colliding. surfaced two real, previously-latent scheduler bugs along
      the way (both fixed): `reap_zombie()` wasn't unlinking a dead task
      from the ready-queue ring before freeing it (use-after-free on the
      next walk), and `TSS.RSP0` was never updated per-task (an IRQ landing
      mid-ring-3 would corrupt whichever task's stack RSP0 last pointed at).
      still leaks a task's private page tables + frames on exit (no address-
      space teardown) - fine for a demo, not for anything long-running.
- [ ] address-space teardown on task exit (free a dead task's private page
      tables + code/stack/`brk`/`mmap` frames, not just its kernel stack).
- [ ] `fork`/`exec` + a simple shell (`retsh`?) that can run commands from
      the initrd/ext2 volume.

## phase 5: polish & system services
- [ ] ACPI (FADT) - implement `reboot` and proper `shutdown` via SLP_TYP.
- [ ] `devfs` - expose `/dev/tty0`, `/dev/sda`, `/dev/null` via VFS.
- [ ] `procfs` - expose process lists and memory stats via VFS.
