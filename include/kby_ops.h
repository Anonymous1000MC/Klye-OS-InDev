#ifndef KLYE_KBY_OPS_H
#define KLYE_KBY_OPS_H

/* Shared by the host assembler (tools/kbasm.c) and the in-kernel assembler
 * (kas.c) so the mnemonic table cannot drift apart.
 *
 * has_arg encoding:
 *   0  no operands
 *   1  one 8-bit literal
 *   3  one 32-bit literal
 *   4  one length-prefixed string
 *   5  a label reference, patched to a 16-bit offset
 *   7  draw op, every operand a 32-bit number
 *   8  numeric operands then a length-prefixed string
 *   9  draw op, 32-bit numbers then a trailing 8-bit literal
 */

struct opdef {
    const char *name;
    int has_arg;
    unsigned char byte;
};

static const struct opdef kby_opcodes[] = {
    { ".nop", 0, 0x00 },
    { ".push8", 1, 0x01 },
    { ".push32", 3, 0x02 },
    { ".pushstr", 4, 0x03 },
    { ".pop", 0, 0x04 },
    { ".dup", 0, 0x05 },
    { ".load", 1, 0x06 },
    { ".store", 1, 0x07 },
    { ".add", 0, 0x08 },
    { ".sub", 0, 0x09 },
    { ".mul", 0, 0x0A },
    { ".div", 0, 0x0B },
    { ".mod", 0, 0x0C },
    { ".cmp", 0, 0x65 },
    { ".jmp", 5, 0x0D },
    { ".jz", 5, 0x0E },
    { ".jnz", 5, 0x0F },
    { ".call", 5, 0x10 },
    { ".ret", 0, 0x11 },
    { ".halt", 0, 0x12 },
    { ".print", 0, 0x20 },
    { ".println", 0, 0x21 },
    { ".clear", 0, 0x30 },
    { ".rect", 7, 0x31 },
    { ".rounded", 9, 0x32 },
    { ".border", 9, 0x33 },
    { ".pixel", 0, 0x34 },
    { ".circle", 0, 0x35 },
    { ".line", 0, 0x36 },
    { ".text", 8, 0x37 },
    { ".textc", 8, 0x38 },
    { ".vfs_exists", 0, 0x50 },
    { ".vfs_size", 0, 0x51 },
    { ".vfs_read", 0, 0x52 },
    { ".vfs_write", 0, 0x53 },
    { ".vfs_append", 0, 0x54 },
    { ".ticks", 0, 0x60 },
    { ".key_poll", 0, 0x61 },
    { ".mouse_x", 0, 0x62 },
    { ".mouse_y", 0, 0x63 },
    { ".mouse_down", 0, 0x64 },
    { ".frame", 0, 0x67 },
    { ".win_open", 8, 0x68 },
    { ".win_close", 0, 0x69 },
    { ".vsync", 0, 0x6A },
    { ".num", 0, 0x66 },
    { 0, 0, 0 }
};

#define KBY_OPDEF_COUNT ((int)(sizeof(kby_opcodes) / sizeof(kby_opcodes[0])))

#endif
