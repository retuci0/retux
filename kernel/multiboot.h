#ifndef MULTIBOOT_H
#define MULTIBOOT_H

#include <stdint.h>

// multiboot specification thing or idk for grub to work
// see: https://en.wikipedia.org/wiki/Multiboot_specification
__attribute__((section(".multiboot")))
const uint32_t multiboot_header[] = {
    0x1BADB002,  // grub magic number
    0x0,         // flags
    -(0x1BADB002)  // checksum
};

#endif  // MULTIBOOT_H
