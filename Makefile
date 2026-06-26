CXX      := x86_64-elf-g++
LD       := x86_64-elf-ld
ASM      := nasm

CXXFLAGS := -ffreestanding -fno-exceptions -fno-rtti -mno-red-zone \
            -mcmodel=large -Wall -Wextra -std=c++17 -Iinclude -MMD -MP -c
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

$(OUT)/kernel.iso: $(OUT)/kernel.bin grub.cfg
	mkdir -p isodir/boot/grub
	cp $(OUT)/kernel.bin isodir/boot/kernel.bin
	cp grub.cfg isodir/boot/grub/grub.cfg
	grub-mkrescue -o $(OUT)/kernel.iso isodir 2>/dev/null

iso: $(OUT)/kernel.iso

run: $(OUT)/kernel.iso
	qemu-system-x86_64 -cdrom $(OUT)/kernel.iso -serial stdio

clean:
	rm -rf $(OUT) isodir

.PHONY: all iso run clean