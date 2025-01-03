KERNEL_SRC = kernel/kernel.c kernel/vga.c kernel/keyboard.c
LINKER_SCRIPT = kernel/linker.ld
OUTPUT_DIR = build
BOOT_DIR = boot
ISO_DIR = iso

KERNEL_OBJ = $(OUTPUT_DIR)/kernel.o $(OUTPUT_DIR)/vga.o $(OUTPUT_DIR)/keyboard.o
KERNEL_BIN = $(OUTPUT_DIR)/kernel.bin
ISO_FILE = retux.iso

CFLAGS = -ffreestanding -m32 -fno-pic -fno-plt

LDFLAGS = -m elf_i386 -T $(LINKER_SCRIPT)

all: $(ISO_FILE)

$(KERNEL_BIN): $(KERNEL_OBJ)
	ld $(LDFLAGS) -o $(KERNEL_BIN) $(KERNEL_OBJ)

$(OUTPUT_DIR)/%.o: kernel/%.c
	mkdir -p $(OUTPUT_DIR)
	gcc $(CFLAGS) -c $< -o $@

$(BOOT_DIR)/kernel.bin: $(KERNEL_BIN)
	mkdir -p $(BOOT_DIR)
	cp $(KERNEL_BIN) $(BOOT_DIR)/kernel.bin

$(ISO_FILE): $(BOOT_DIR)/kernel.bin
	mkdir -p $(ISO_DIR)/boot/grub
	cp $(BOOT_DIR)/kernel.bin $(ISO_DIR)/boot/
	cp boot/grub/grub.cfg $(ISO_DIR)/boot/grub/
	grub-mkrescue -o $(ISO_FILE) $(ISO_DIR)

clean:
	rm $(BOOT_DIR)/kernel.bin
	rm -rf $(ISO_DIR)
	rm -rf $(OUTPUT_DIR)
	rm $(ISO_FILE)

run: $(ISO_FILE)
	qemu-system-x86_64 -cdrom $(ISO_FILE)
