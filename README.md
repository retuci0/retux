# retux "kernel"

> extremely barebones implementation of a kernel in c

## why

why the fuck not lmao. also i wanted to learn c so fuck yeah. best way to start.

## what does it do

it greets you with a grub menu, where to load the so called kernel, you need to select "Retux Kernel" in the menu with the arrow keys.

after that, all it does (for now) is say "uhhhh hi i guess" and sit there.

i will add commands in the future.

## how do i use it

don't.

if you still want to give it a try for some reason, you will need the following tools installed:

- `qemu`: for the vm
- `grub`: to use `grub-mkrescue` to build the `.iso`
- `gcc`: to compile the thing
- `mtools`: needed by `grub-mkrescue`

after that, give execute permissions to `run.sh`, `build.sh` and `buildiso.sh`

finally, run `run.sh`. a qemu window will open up, greeting you with the kernel.

in short:
1. `sudo pacman -S --needed qemu gcc grub mtools` (for other distros than arch, use its respective package manager)
2. `chmod +x run.sh build.sh buildiso.sh`
3. `./run.sh`

## it doesn't work!!!!

not my problem lmao. it works on my machine.

## contributions

if you for some odd reason feel like wasting your time in this, go ahead, i'm awlays open to prs.

## sources i used

- [this book](https://en.wikipedia.org/wiki/Hacking:_The_Art_of_Exploitation) that taught me basics of c and other low level stuff
- [this other book too](https://beej.us/guide/bgc/html/split/)
- [multiboot specification](https://en.wikipedia.org/wiki/Multiboot_specification)
- [linux kernel coding style](https://www.kernel.org/doc/html/v4.10/process/coding-style.html)