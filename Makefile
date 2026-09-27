CROSS_CC := $(shell command -v x86_64-elf-gcc 2>/dev/null)
CC := $(if $(CROSS_CC),$(CROSS_CC),gcc)
NASM ?= nasm
GRUB_MKRESCUE ?= grub-mkrescue
QEMU ?= qemu-system-x86_64

TARGET := klye.iso
BUILD_DIR := build
ISO_DIR := $(BUILD_DIR)/isodir
KERNEL := $(BUILD_DIR)/klye.elf

CPPFLAGS := -Iinclude
CFLAGS := -std=c11 -O2 -g -ffreestanding -fno-stack-protector \
          -fno-builtin -fno-pic -mno-red-zone -mcmodel=kernel \
          -msse -mfpmath=sse -mno-80387 \
          -fomit-frame-pointer -fno-unwind-tables -fno-asynchronous-unwind-tables \
          -Wall -Wextra -Werror
ASFLAGS := -ffreestanding -fno-stack-protector -fno-pic -mno-red-zone \
           -mno-80387
LDFLAGS := -nostdlib -no-pie -Wl,-T,linker.ld -Wl,--gc-sections \
           -Wl,-z,max-page-size=0x1000 -Wl,-z,noexecstack
NASMFLAGS := -f elf64 -g -F dwarf

HOST_CC ?= gcc
HOST_CFLAGS := -std=c11 -O2 -Wall -Wextra
ROOTFS := rootfs
TOOLS := $(BUILD_DIR)/tools
VFS_IMAGE := $(BUILD_DIR)/vfs_image.c

C_SOURCES := kernel.c scheduler.c mem.c heap.c libc.c gfx.c font.c input.c vfs.c launcher.c kby.c kas.c apps.c shell.c wm.c gui.c
ASM_SOURCES := interrupts.S context.S fpu.S
OBJECTS := $(addprefix $(BUILD_DIR)/,$(C_SOURCES:.c=.o)) \
           $(addprefix $(BUILD_DIR)/,$(ASM_SOURCES:.S=.o)) \
           $(BUILD_DIR)/vfs_image.o \
           $(BUILD_DIR)/boot.o

.PHONY: all iso run clean tools rootfs

all: $(TARGET)

iso: $(TARGET)

$(BUILD_DIR):
	mkdir -p $@

$(TOOLS):
	mkdir -p $@

tools: $(BUILD_DIR)/mkvfs $(BUILD_DIR)/kbasm

$(BUILD_DIR)/mkvfs: tools/mkvfs.c | $(TOOLS)
	$(HOST_CC) $(HOST_CFLAGS) -o $@ $<

$(BUILD_DIR)/kbasm: tools/kbasm.c include/kby_ops.h | $(TOOLS)
	$(HOST_CC) $(HOST_CFLAGS) -Iinclude -o $@ $<

# assemble every .kby source into a sibling .kbin before imaging
$(ROOTFS)/%.kbin: $(ROOTFS)/%.kby $(BUILD_DIR)/kbasm
	$(BUILD_DIR)/kbasm $< $@

# programs named by a /bin launcher are assembled straight into /bin
$(ROOTFS)/bin/%.kbin: $(ROOTFS)/home/klye/kby/%.kby $(BUILD_DIR)/kbasm
	$(BUILD_DIR)/kbasm $< $@

KBY_SOURCES := $(shell find $(ROOTFS)/home -name '*.kby' 2>/dev/null)
# only programs named by a "kind = kby" launcher are staged into /bin
KBY_BIN_PROGRAMS := $(patsubst %,$(ROOTFS)/bin/%,$(shell sed -n 's/^kind *= *kby$$//p' $(ROOTFS)/bin/* 2>/dev/null >/dev/null; for l in $(ROOTFS)/bin/*; do \
  if grep -q '^kind *= *kby$$' $$l 2>/dev/null; then \
    grep '^entry *= *' $$l | sed 's/^entry *= *//'; fi; done))
KBY_BINARIES := $(KBY_SOURCES:.kby=.kbin) $(KBY_BIN_PROGRAMS)

$(VFS_IMAGE): $(BUILD_DIR)/mkvfs $(KBY_BINARIES) \
             $(shell find $(ROOTFS) -type f ! -name '*.kby' 2>/dev/null)
	$(BUILD_DIR)/mkvfs $(ROOTFS) $@

$(BUILD_DIR)/vfs_image.o: $(VFS_IMAGE)
	$(CC) $(CPPFLAGS) $(CFLAGS) -c $< -o $@

$(ISO_DIR)/boot/grub:
	mkdir -p $@

$(BUILD_DIR)/boot.o: boot.asm | $(BUILD_DIR)
	$(NASM) $(NASMFLAGS) $< -o $@

$(BUILD_DIR)/%.o: %.c | $(BUILD_DIR)
	$(CC) $(CPPFLAGS) $(CFLAGS) -c $< -o $@

$(BUILD_DIR)/%.o: %.S | $(BUILD_DIR)
	$(CC) $(CPPFLAGS) $(ASFLAGS) -c $< -o $@

$(KERNEL): $(OBJECTS) linker.ld
	$(CC) $(LDFLAGS) $(OBJECTS) -o $@

$(TARGET): $(KERNEL) grub/grub.cfg | $(ISO_DIR)/boot/grub
	cp $(KERNEL) $(ISO_DIR)/boot/klye.elf
	cp grub/grub.cfg $(ISO_DIR)/boot/grub/grub.cfg
	$(GRUB_MKRESCUE) -o $@ $(ISO_DIR)

run: $(TARGET)
	$(QEMU) -cdrom $(TARGET) -m 512M

clean:
	rm -rf $(BUILD_DIR) $(TARGET)
