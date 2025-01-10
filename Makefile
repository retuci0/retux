SRC_DIR = kernel/src
INCLUDE_DIR = kernel/include
OUTPUT_DIR = build
BOOT_DIR = boot
ISO_DIR = iso
LINKER_SCRIPT = kernel/linker.ld

C_SOURCES = $(wildcard $(SRC_DIR)/**/*.c) $(SRC_DIR)/kernel.c
KERNEL_OBJ = $(patsubst $(SRC_DIR)/%.c, $(OUTPUT_DIR)/%.o, $(C_SOURCES))

KERNEL_BIN = $(OUTPUT_DIR)/kernel.bin
ISO_FILE = retux.iso

CFLAGS = -ffreestanding -m32 -fno-pic -fno-plt -I$(INCLUDE_DIR)
LDFLAGS = -m elf_i386 -T $(LINKER_SCRIPT)

all: $(ISO_FILE)

$(KERNEL_BIN): $(KERNEL_OBJ)
	ld $(LDFLAGS) -o $(KERNEL_BIN) $(KERNEL_OBJ)

$(OUTPUT_DIR)/%.o: $(SRC_DIR)/%.c
	mkdir -p $(dir $@)
	gcc $(CFLAGS) -c $< -o $@

$(BOOT_DIR)/kernel.bin: $(KERNEL_BIN)
	mkdir -p $(BOOT_DIR)
	cp $(KERNEL_BIN) $(BOOT_DIR)/kernel.bin

# generate the iso file using grub
$(ISO_FILE): $(BOOT_DIR)/kernel.bin
	mkdir -p $(ISO_DIR)/boot/grub
	cp $(BOOT_DIR)/kernel.bin $(ISO_DIR)/boot/
	cp boot/grub/grub.cfg $(ISO_DIR)/boot/grub/
	grub-mkrescue -o $(ISO_FILE) $(ISO_DIR)

# clean binaries
clean:
	rm -f $(BOOT_DIR)/kernel.bin
	rm -rf $(ISO_DIR)
	rm -rf $(OUTPUT_DIR)
	rm -f $(ISO_FILE)

# run the kernel on a virtual i386 machine with qemu
run: $(ISO_FILE)
	qemu-system-i386 -cdrom $(ISO_FILE)
