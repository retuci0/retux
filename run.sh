#!/bin/sh

./build.sh
./buildiso.sh
qemu-system-x86_64 -cdrom retux.iso
