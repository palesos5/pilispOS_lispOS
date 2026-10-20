# LispOS — Makefile (этап M0: BIOS/x86 32-bit загрузка)
NASM    ?= nasm
CC      ?= gcc
LD      ?= ld
OBJCOPY ?= objcopy
QEMU    ?= qemu-system-i386

BUILD := build

CFLAGS  := -std=gnu11 -ffreestanding -nostdlib -O2 -Wall -Wextra -Werror \
           -m32 -fno-pie -fno-stack-protector -fno-pic \
           -Iinclude/kernel -Iinclude
ASFLAGS := -f elf32
LDFLAGS := -m elf_i386 -T kernel/linker.ld -nostdlib --build-id=none --gc-sections

KERNEL_OBJS := $(BUILD)/kernel_entry.o $(BUILD)/main.o $(BUILD)/serial.o

.PHONY: all clean run test image

all: $(BUILD)/boot.bin $(BUILD)/kernel.bin image

$(BUILD):
	mkdir -p $(BUILD)

# --- MBR загрузчик (raw bin, ровно 512 байт) ---
$(BUILD)/boot.bin: boot/boot.asm | $(BUILD)
	$(NASM) -f bin $< -o $@
	@size=$$(wc -c < $@); \
	if [ "$$size" -ne 512 ]; then echo "ERROR: boot.bin must be 512 bytes, got $$size" >&2; exit 1; fi

# --- ядро: asm entry -> C -> линковка flat binary ---
$(BUILD)/kernel_entry.o: kernel/kernel_entry.asm | $(BUILD)
	$(NASM) $(ASFLAGS) $< -o $@

$(BUILD)/main.o: kernel/main.c include/kernel/bootinfo.h | $(BUILD)
	$(CC) $(CFLAGS) -c $< -o $@

$(BUILD)/serial.o: kernel/serial.c | $(BUILD)
	$(CC) $(CFLAGS) -c $< -o $@

$(BUILD)/kernel.elf: $(KERNEL_OBJS) kernel/linker.ld
	$(LD) $(LDFLAGS) -o $@ $(KERNEL_OBJS)

$(BUILD)/kernel.bin: $(BUILD)/kernel.elf
	$(OBJCOPY) -O binary $< $@

image: $(BUILD)/lispos.img

$(BUILD)/lispos.img: $(BUILD)/boot.bin $(BUILD)/kernel.bin
	sh build/make_image.sh

run: $(BUILD)/lispos.img
	$(QEMU) -drive format=raw,file=$< -serial stdio -no-reboot

# Автотест: ждём маркер в serial-выводе QEMU (exit по -device shutdown через expect-подобный скрипт)
test: $(BUILD)/lispos.img
	sh tests/run_m0.sh

clean:
	rm -rf $(BUILD)
