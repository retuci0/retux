#!/bin/sh

mkdir -p iso/boot/grub
cp boot/kernel.bin iso/boot/
cp boot/grub/grub.cfg iso/boot/grub/

grub-mkrescue -o retux.iso iso/
