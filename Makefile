CXX      := x86_64-elf-g++
LD       := x86_64-elf-ld
ASM      := nasm

CXXFLAGS := -g -ffreestanding -fno-exceptions -fno-rtti -mno-red-zone \
            -mcmodel=large -Wall -Wextra -std=c++17 -MMD -MP -c -Isrc \
            -mno-sse -mno-sse2 -mno-sse3 -mno-mmx -mno-avx -mno-avx2 \
            -mgeneral-regs-only
ASFLAGS  := -f elf64

SRC      := src
OUT      := out

CXX_SOURCES := $(shell find $(SRC) -name '*.cpp')
ASM_SOURCES := $(shell find $(SRC) -name '*.asm')

CXX_OBJS := $(CXX_SOURCES:$(SRC)/%.cpp=$(OUT)/%.o)
ASM_OBJS := $(ASM_SOURCES:$(SRC)/%.asm=$(OUT)/%.o)
OBJS     := $(CXX_OBJS) $(ASM_OBJS)
DEPS     := $(CXX_OBJS:.o=.d) $(ASM_OBJS:.o=.d)

all: $(OUT)/kernel.bin

$(OUT)/%.o: $(SRC)/%.cpp
	mkdir -p $(dir $@)
	$(CXX) $(CXXFLAGS) $< -o $@

$(OUT)/%.o: $(SRC)/%.asm
	mkdir -p $(dir $@)
	$(ASM) $(ASFLAGS) -MD $(@:.o=.d) $< -o $@

$(OUT)/kernel.bin: $(OBJS) linker.ld
	$(LD) -n -T linker.ld -o $@ $(OBJS)

-include $(DEPS)


INITRDROOT := initrdroot
INITRD     := $(OUT)/initrd.tar

$(INITRD): $(shell find $(INITRDROOT) -type f 2>/dev/null)
	mkdir -p $(INITRDROOT)
	mkdir -p $(OUT)
	tar --format=ustar -cf $(INITRD) -C $(INITRDROOT) .

initrd: $(INITRD)

$(OUT)/kernel.iso: $(OUT)/kernel.bin $(INITRD) grub.cfg
	mkdir -p isodir/boot/grub
	cp $(OUT)/kernel.bin isodir/boot/kernel.bin
	cp $(INITRD) isodir/boot/initrd.tar
	cp grub.cfg isodir/boot/grub/grub.cfg
	grub-mkrescue -o $(OUT)/kernel.iso isodir 2>/dev/null

iso: $(OUT)/kernel.iso


DISK      := $(OUT)/disk.img
DISKROOT  := diskroot

$(DISK): $(shell find $(DISKROOT) -type f 2>/dev/null)
	mkdir -p $(DISKROOT)
	mkdir -p $(OUT)
	mke2fs -t ext2 -b 1024 -I 128 \
	    -O ^64bit,^metadata_csum,^huge_file,^flex_bg \
	    -d $(DISKROOT) -F $(DISK) 64M

disk: $(DISK)

run: $(OUT)/kernel.iso $(DISK)
	qemu-system-x86_64 -M q35 \
	    -cdrom $(OUT)/kernel.iso \
	    -drive id=disk0,if=none,file=$(DISK),format=raw \
	    -device ahci,id=ahci0 \
	    -device ide-hd,drive=disk0,bus=ahci0.0 \
	    -serial stdio -no-reboot -no-shutdown

debug: $(OUT)/kernel.iso $(DISK)
	qemu-system-x86_64 -M q35 \
	    -cdrom $(OUT)/kernel.iso \
	    -drive id=disk0,if=none,file=$(DISK),format=raw \
	    -device ahci,id=ahci0 \
	    -device ide-hd,drive=disk0,bus=ahci0.0 \
	    -serial stdio -no-reboot -no-shutdown \
	    -d int,cpu_reset -D out/qemu.log

clean:
	rm -rf $(OUT) isodir out diskroot initrdroot


.PHONY: all iso disk initrd run clean
