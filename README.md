# retux "kernel"

> extremely barebones implementation of a kernel in c

## why

why the fuck not lmao. also i wanted to learn c so fuck yeah. best way to start.

## what does it do

it greets you with a grub menu, where to load the so called kernel, you need to select "Retux Kernel" in the menu with the arrow keys.

after that, all it does (for now) is say "uhhhh hi i guess" and let you write stuff underneath

i will add commands in the future.

## what does the name mean

it is a combination of the names "linux" and "retucio". also the linux penguin's name is tux, so even better.

## how do i use it

don't.

if you still want to give it a try for some reason, you will need the following tools installed:

- `qemu`: for the vm
- `grub`: to use `grub-mkrescue` to build the `.iso`
- `gcc`: to compile the thing
- `mtools`: needed by `grub-mkrescue`

then, just run `make run`. a qemu window will open up, greeting you with the kernel.

in short:
1. `sudo pacman -S --needed qemu gcc grub mtools` (for other distros than arch, use its respective package manager)
2. `make run`, or just `make` for building it.

## it doesn't work!!!!

not my problem lmao. it works on my machine.
###### note: i pushed a commit that broke it lmao so yes my problem, i fixed it tho

## contributions

if you for some odd reason feel like wasting your time in this, go ahead, i'm always open to prs.

## sources i used

- [this book](https://en.wikipedia.org/wiki/Hacking:_The_Art_of_Exploitation) that taught me basics of c and other low level stuff
- [this other book too](https://beej.us/guide/bgc/html/split/)
- [multiboot specification](https://en.wikipedia.org/wiki/Multiboot_specification)
- [linux kernel coding style](https://www.kernel.org/doc/html/v4.10/process/coding-style.html)
- [character scancodes in decimal](https://www.lookuptables.com/coding/keyboard-scan-codes)

## known issues

- key release detection isn't functional, so pressing shift once will make ALL following key presses uppercase
- when pressing the backspace key at the beggining of a line, instead of moving the cursor to the latest valid character in the line above, it
moves it to the far right, acting as if there were a bunch of spaces after the last actual character, hence making writing text more tedious.
- pressing the ctrl key inserts a reverse slash ('\')

## todo list
- arrow keys
- actually detect key releases
- commands and actual shit and allat