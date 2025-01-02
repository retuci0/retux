#!/bin/sh

KERNEL_SRC=kernel/kernel.c
LINKER_SCRIPT=kernel/linker.ld
OUTPUT_DIR=build
BOOT_DIR=boot

rm -rf $OUTPUT_DIR
mkdir -p $OUTPUT_DIR
mkdir -p $BOOT_DIR

gcc -ffreestanding -m32 -fno-pic -fno-plt -c $KERNEL_SRC -o $OUTPUT_DIR/kernel.o

ld -m elf_i386 -T $LINKER_SCRIPT -o $OUTPUT_DIR/kernel.bin $OUTPUT_DIR/kernel.o

cp $OUTPUT_DIR/kernel.bin $BOOT_DIR/
