# retux kernel

> now rewritten in C++!


## why?

because the previous version was AI slop and I had no idea what I was doing. this time, I made sure to actually know what
I was doing. even though I learnt most of what I know from Claude, it was still written by me.


## what it does (so far)

not much, on purpose:

- boots via GRUB2 + Multiboot2
- climbs from 32-bit protected mode into 64-bit long mode by hand - own page tables, own GDT, no shortcuts
- says hello over both VGA text mode and serial

that's it. that's the whole kernel right now. it boots, it proves long mode actually works, and it halts.


## building & running

you need:

- `nasm`
- an `x86_64-elf` cross-compiler (don't use your host `g++` since it will assume a Linux ABI that does NOT exist here)
- `grub`, `xorriso`, `mtools` (grub-mkrescue needs all three)
- `qemu-system-x86_64`

on Arch:

```
sudo pacman -S nasm grub xorriso mtools qemu-full
yay -S x86_64-elf-gcc x86_64-elf-binutils
```

then:

```
make run
```

assembles, compiles, links, packs a bootable ISO, boots it in QEMU with serial piped to your terminal. if you see
text on the QEMU screen and a line in your terminal, it worked.


## status / roadmap

not an OS yet. just a kernel that boots and says hi.

see [`TODO.md`](TODO.md)


## thanks

- the OSDev wiki, for everything that isn't covered by arguing with Claude about CR0 bits at 1 AM
- whoever wrote the Multiboot2 spec, for letting me skip writing a bootloader

> as always, PRs are always welcome. if you contribute, you will be added to the credits! (I'm sure y'all don't care but whatever)