# retux kernel

> now rewritten in C++!


## why?

because the previous version was AI slop and I had no idea what I was doing. this time, I made sure to actually know what
I was doing. even though I learnt most of what I know from Claude, it was still written by me.


## what it does (so far)

more than "boots and halts" now, but still nowhere near an OS:

- boots via GRUB2 + Multiboot2
- climbs from 32-bit protected mode into 64-bit long mode by hand - own page tables, own GDT, no shortcuts
- has a real memory story: a bitmap PMM, a 4-level VMM (with huge-page splitting and per-section kernel permissions), and a coalescing `kmalloc`/`kfree` heap
- IDT + exception handlers with a register dump on panic, PIC/APIC, a PIT/HPET tick, PCI enumeration, a PS/2 keyboard driver, and a basic scrollback TTY
- an AHCI (SATA) driver, MBR/GPT partition parsing, a VFS abstraction, and an ext2 driver - it'll mount a real ext2 disk image and walk its directory tree
- an `initrd`: a Multiboot2 module containing a plain USTAR archive, mounted in-memory by a small `tarfs` driver - so there's a filesystem to read from before the disk path has to be trusted

that's it so far. no userspace yet - that's next.


## building & running

you need:

- `nasm`
- an `x86_64-elf` cross-compiler (don't use your host `g++` since it will assume a Linux ABI that does NOT exist here)
- `grub`, `xorriso`, `mtools` (grub-mkrescue needs all three)
- `e2fsprogs` (for `mke2fs`, used to build the test disk image)
- `qemu-system-x86_64`

on Arch:

```
sudo pacman -S nasm grub xorriso mtools e2fsprogs qemu-full
yay -S x86_64-elf-gcc x86_64-elf-binutils
```

then:

```
make run
```

assembles, compiles, links, packs a bootable ISO (including an `initrd.tar` built from `initrdroot/`), builds a small
ext2 test disk (`disk.img`, from `diskroot/`), and boots it all in QEMU with an AHCI controller attached and serial
piped to your terminal. if you see text on the QEMU screen and a line in your terminal, it worked.

using `bear` to generate a `compile_commands.json` is recommended:

```bash
sudo pacman -S bear
bear -- make
```


## status / roadmap

not an OS yet, but it's got memory management, drivers, and a filesystem stack now. no userspace, no multitasking,
no syscalls - that's next.

see [`TODO.md`](TODO.md)


## thanks

- the OSDev wiki, for everything that isn't covered by arguing with Claude about CR0 bits at 1 AM
- whoever wrote the Multiboot2 spec, for letting me skip writing a bootloader
- the ext2 spec
- the USTAR/tar spec, for being simple enough that "no journaling for now" applies to an entire filesystem driver

> as always, PRs are always welcome. if you contribute, you will be added to the credits! (I'm sure y'all don't care but whatever)


## license

see [`LICENSE.md`](LICENSE.md)
