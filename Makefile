CROSS_CC := $(shell command -v x86_64-elf-gcc 2>/dev/null)
CC := $(if $(CROSS_CC),$(CROSS_CC),gcc)
NASM ?= nasm
GRUB_MKRESCUE ?= grub-mkrescue
QEMU ?= qemu-system-x86_64

TARGET := klye.iso
BUILD_DIR := build
ISO_DIR := $(BUILD_DIR)/isodir
KERNEL := $(BUILD_DIR)/klye.elf

CPPFLAGS := -Iinclude -Ithird_party/lua -Ithird_party/lua/freestanding
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

C_SOURCES := kernel.c scheduler.c mem.c heap.c libc.c gfx.c font.c input.c vfs.c launcher.c kby.c kas.c apps.c shell.c wm.c gui.c lua_host.c
ASM_SOURCES := interrupts.S context.S fpu.S setjmp.S

# Lua 5.4.7 core plus the base/string/table/math/utf8 libraries.  The io, os,
# debug, coroutine and package loaders are deliberately excluded: they want
# stdio, dlopen and signals that a kernel does not have.
LUA_SOURCES := third_party/lua/lapi.c third_party/lua/lcode.c \
  third_party/lua/lctype.c third_party/lua/ldebug.c third_party/lua/ldo.c \
  third_party/lua/lfunc.c third_party/lua/lgc.c third_party/lua/llex.c \
  third_party/lua/lmem.c third_party/lua/lobject.c third_party/lua/lopcodes.c \
  third_party/lua/lparser.c third_party/lua/lstate.c third_party/lua/lstring.c \
  third_party/lua/ltable.c third_party/lua/ltm.c third_party/lua/lvm.c \
  third_party/lua/lzio.c third_party/lua/lauxlib.c \
  third_party/lua/lbaselib.c third_party/lua/lstrlib.c \
  third_party/lua/ltablib.c third_party/lua/lmathlib.c \
  third_party/lua/lutf8lib.c
LUA_OBJECTS := $(addprefix $(BUILD_DIR)/,$(notdir $(LUA_SOURCES:.c=.lua.o)))
OBJECTS := $(addprefix $(BUILD_DIR)/,$(C_SOURCES:.c=.o)) $(LUA_OBJECTS) \
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

# Vendored Lua is built with relaxed warnings only for its own files.
$(BUILD_DIR)/%.lua.o: third_party/lua/%.c | $(BUILD_DIR)
	$(CC) $(CPPFLAGS) $(CFLAGS) -Wno-error -Wno-unused-but-set-variable \
	      -Wno-implicit-fallthrough -Wno-implicit-int-conversion \
	      -Wno-int-conversion -Wno-misleading-indentation -c $< -o $@

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
