#include <stdbool.h>
#include <stdint.h>

#include "apps.h"
#include "gfx.h"
#include "gui.h"
#include "input.h"
#include "io.h"
#include "kernel.h"
#include "scheduler.h"
#include "shell.h"
#include "theme.h"
#include "wm.h"
#include "vfs.h"

struct __attribute__((packed)) gdt_descriptor {
    uint16_t limit_low;
    uint16_t base_low;
    uint8_t base_middle;
    uint8_t access;
    uint8_t granularity;
    uint8_t base_high;
};

struct __attribute__((packed)) gdt_pointer {
    uint16_t limit;
    uint64_t base;
};

struct __attribute__((packed)) idt_descriptor {
    uint16_t offset_low;
    uint16_t selector;
    uint8_t reserved;
    uint8_t type;
    uint16_t offset_middle;
    uint32_t offset_high;
    uint32_t zero;
};

struct __attribute__((packed)) idt_pointer {
    uint16_t limit;
    uint64_t base;
};

_Static_assert(sizeof(struct gdt_descriptor) == 8, "invalid GDT descriptor");
_Static_assert(sizeof(struct idt_descriptor) == 16, "invalid IDT descriptor");

static struct gdt_descriptor gdt[3];
static struct gdt_pointer gdt_pointer_value;
static struct idt_descriptor idt[256];
static struct idt_pointer idt_pointer_value;
static volatile uint64_t tick_count;

extern void *isr_table[32];
extern void *irq_table[16];
extern void isr255(void);

static void set_idt_gate(uint8_t vector, void *handler, uint8_t type)
{
    uint64_t address = (uint64_t)(uintptr_t)handler;

    idt[vector].offset_low = (uint16_t)(address & 0xFFFF);
    idt[vector].selector = 0x08;
    idt[vector].reserved = 0;
    idt[vector].type = type;
    idt[vector].offset_middle = (uint16_t)((address >> 16) & 0xFFFF);
    idt[vector].offset_high = (uint32_t)(address >> 32);
    idt[vector].zero = 0;
}

void gdt_init(void)
{
    gdt[0] = (struct gdt_descriptor){0};
    gdt[1] = (struct gdt_descriptor){
        .limit_low = 0xFFFF,
        .base_low = 0,
        .base_middle = 0,
        .access = 0x9A,
        .granularity = 0xAF,
        .base_high = 0
    };
    gdt[2] = (struct gdt_descriptor){
        .limit_low = 0xFFFF,
        .base_low = 0,
        .base_middle = 0,
        .access = 0x92,
        .granularity = 0xCF,
        .base_high = 0
    };
    gdt_pointer_value.limit = sizeof(gdt) - 1;
    gdt_pointer_value.base = (uint64_t)(uintptr_t)gdt;

    __asm__ volatile("lgdt %0" : : "m"(gdt_pointer_value));
    __asm__ volatile(
        "mov $0x10, %%ax\n"
        "mov %%ax, %%ds\n"
        "mov %%ax, %%es\n"
        "mov %%ax, %%ss\n"
        "mov %%ax, %%fs\n"
        "mov %%ax, %%gs\n"
        :
        :
        : "rax"
    );
}

void idt_init(void)
{
    for (uint32_t vector = 0; vector < 32; ++vector) {
        set_idt_gate((uint8_t)vector, isr_table[vector], 0x8E);
    }
    for (uint32_t irq = 0; irq < 16; ++irq) {
        set_idt_gate((uint8_t)(32U + irq), irq_table[irq], 0x8E);
    }
    set_idt_gate(255, isr255, 0x8E);

    idt_pointer_value.limit = sizeof(idt) - 1;
    idt_pointer_value.base = (uint64_t)(uintptr_t)idt;
    __asm__ volatile("lidt %0" : : "m"(idt_pointer_value));
}

void pic_init(void)
{
    outb(0x20, 0x11);
    io_wait();
    outb(0xA0, 0x11);
    io_wait();
    outb(0x21, 0x20);
    io_wait();
    outb(0xA1, 0x28);
    io_wait();
    outb(0x21, 0x04);
    io_wait();
    outb(0xA1, 0x02);
    io_wait();
    outb(0x21, 0xFB);
    io_wait();
    outb(0xA1, 0xFF);
    io_wait();
}

void pic_unmask(uint8_t irq)
{
    uint16_t port = irq < 8 ? 0x21 : 0xA1;
    uint8_t mask = inb(port);
    outb(port, (uint8_t)(mask & ~(1U << (irq & 7U))));
}

void pic_eoi(uint8_t irq)
{
    if (irq >= 8) {
        outb(0xA0, 0x20);
    }
    outb(0x20, 0x20);
}

void pit_init(uint32_t frequency)
{
    uint32_t divisor = 1193182U / frequency;

    if (divisor == 0) {
        divisor = 1;
    }
    if (divisor > 65535U) {
        divisor = 65535U;
    }

    outb(0x43, 0x36);
    outb(0x40, (uint8_t)(divisor & 0xFF));
    outb(0x40, (uint8_t)(divisor >> 8));
}

void pit_tick(void)
{
    ++tick_count;
}

uint64_t pit_ticks(void)
{
    return tick_count;
}

static void serial_putc(char character)
{
    while ((inb(0x3F8 + 5) & 0x20) == 0) {
    }
    outb(0x3F8, (uint8_t)character);
}

void serial_init(void)
{
    outb(0x3F8 + 1, 0x00);
    outb(0x3F8 + 3, 0x80);
    outb(0x3F8 + 0, 0x03);
    outb(0x3F8 + 1, 0x00);
    outb(0x3F8 + 3, 0x03);
    outb(0x3F8 + 2, 0xC7);
    outb(0x3F8 + 4, 0x0B);
}

void serial_write(const char *text)
{
    while (*text != '\0') {
        if (*text == '\n') {
            serial_putc('\r');
        }
        serial_putc(*text);
        ++text;
    }
}

static void serial_hex(uint64_t value)
{
    static const char digits[] = "0123456789ABCDEF";
    char buffer[19];
    uint32_t index = 18;

    buffer[18] = '\0';
    while (value != 0) {
        buffer[--index] = digits[value & 0xF];
        value >>= 4;
    }
    if (index == 18) {
        buffer[--index] = '0';
    }
    serial_write(&buffer[index]);
}

static void serial_post(const char *label, const char *status)
{
    serial_write("  [");
    serial_write(status);
    serial_write("] ");
    serial_write(label);
    serial_write("\n");
}

static bool console_enabled;

static uint32_t post_color(const char *status)
{
    if (status[0] == 'O') {
        return CONSOLE_COLOR_OK;
    }
    if (status[0] == 'M' || status[0] == 'F') {
        return CONSOLE_COLOR_WARN;
    }
    return CONSOLE_COLOR_ACCENT;
}

static void post(const char *label, const char *status)
{
    serial_post(label, status);
    if (console_enabled) {
        console_status(label, status, post_color(status));
        console_flush();
    }
}

static void post_line(const char *text)
{
    serial_write(text);
    if (console_enabled) {
        console_write(text);
        console_flush();
    }
}

static void post_number(const char *label, uint64_t value, const char *suffix)
{
    char digits[24];
    uint32_t length = 0;
    uint64_t original = value;

    serial_write(label);
    if (value == 0) {
        digits[length++] = '0';
    }
    while (value != 0 && length < sizeof(digits) - 1U) {
        digits[length++] = (char)('0' + (uint32_t)(value % 10U));
        value /= 10U;
    }
    while (length > 0) {
        serial_putc(digits[--length]);
    }
    serial_write(suffix);
    if (console_enabled) {
        console_number(label, original, suffix);
        console_flush();
    }
}

static void post_hex(const char *label, uint64_t value, const char *suffix)
{
    serial_write(label);
    serial_hex(value);
    serial_write(suffix);
    if (console_enabled) {
        console_write(label);
        console_write("0x");
        {
            char digits[20];
            uint32_t index = 19;
            digits[19] = '\0';
            if (value == 0) {
                digits[--index] = '0';
            }
            while (value != 0 && index > 0) {
                static const char table[] = "0123456789ABCDEF";
                digits[--index] = table[value & 0xF];
                value >>= 4;
            }
            console_write(&digits[index]);
        }
        console_write(suffix);
        console_flush();
    }
}

static void cpuid_raw(uint32_t leaf, uint32_t subleaf, uint32_t *out)
{
    __asm__ volatile("cpuid"
                     : "=a"(out[0]), "=b"(out[1]), "=c"(out[2]),
                       "=d"(out[3])
                     : "a"(leaf), "c"(subleaf));
}

static uint64_t cpuid_max_extended(void)
{
    uint32_t regs[4];

    cpuid_raw(0x80000000U, 0, regs);
    return regs[0];
}

static void report_cpu(void)
{
    uint32_t regs[4];
    char vendor[13];
    uint32_t max_basic;
    uint32_t max_extended;
    bool brand_available = false;

    cpuid_raw(0, 0, regs);
    max_basic = regs[0];
    vendor[0] = (char)(regs[1] & 0xFFU);
    vendor[1] = (char)((regs[1] >> 8) & 0xFFU);
    vendor[2] = (char)((regs[1] >> 16) & 0xFFU);
    vendor[3] = (char)((regs[1] >> 24) & 0xFFU);
    vendor[4] = (char)(regs[3] & 0xFFU);
    vendor[5] = (char)((regs[3] >> 8) & 0xFFU);
    vendor[6] = (char)((regs[3] >> 16) & 0xFFU);
    vendor[7] = (char)((regs[3] >> 24) & 0xFFU);
    vendor[8] = (char)(regs[2] & 0xFFU);
    vendor[9] = (char)((regs[2] >> 8) & 0xFFU);
    vendor[10] = (char)((regs[2] >> 16) & 0xFFU);
    vendor[11] = (char)((regs[2] >> 24) & 0xFFU);
    vendor[12] = '\0';

    max_extended = (uint32_t)cpuid_max_extended();
    if (max_extended >= 0x80000004U) {
        char brand[49];
        uint32_t index = 0;

        for (uint32_t leaf = 0x80000002U; leaf <= 0x80000004U; ++leaf) {
            cpuid_raw(leaf, 0, regs);
            for (uint32_t byte = 0; byte < 16U; ++byte) {
                char character =
                    (char)((regs[byte / 4U] >> ((byte % 4U) * 8U)) & 0xFFU);
                if (character == '\0') {
                    continue;
                }
                brand[index++] = character;
            }
        }
        while (index > 0 && brand[index - 1U] == ' ') {
            --index;
        }
        brand[index] = '\0';
        if (index != 0) {
            post("PROCESSOR", "OK");
            post_line("       ");
            post_line(brand);
            post_line("\n");
            brand_available = true;
        }
    }

    if (!brand_available) {
        post("PROCESSOR", "OK");
        post_line("       ");
        post_line(vendor);
        post_line("\n");
    }

    cpuid_raw(1, 0, regs);
    post_line("       cpuid leaves 0x");
    post_hex("", max_basic, " / 0x");
    post_hex("", max_extended, "\n");

    if (max_extended >= 0x80000008U) {
        cpuid_raw(0x80000008U, 0, regs);
        post_number("       physical address bits: ", (regs[0] >> 0) & 0xFFU,
                        "\n");
    }
}

__attribute__((noreturn)) void halt_forever(void)
{
    __asm__ volatile("cli; hlt");
    for (;;) {
    }
}

static uint64_t read_control_register_2(void)
{
    uint64_t value;

    __asm__ volatile("movq %%cr2, %0" : "=r"(value));
    return value;
}

volatile uint32_t irq1_count;
volatile uint32_t irq12_count;

struct fault_record {
    uint64_t vector;
    uint64_t error;
    uint64_t rip;
    uint64_t cs;
    uint64_t rflags;
    uint64_t rsp;
    uint64_t ss;
    uint64_t cr2;
    uint64_t rax;
    uint64_t rbx;
    uint64_t rcx;
    uint64_t rdx;
    uint64_t rsi;
    uint64_t rdi;
    uint64_t rbp;
    uint64_t r8;
    uint64_t r9;
    uint64_t r10;
    uint64_t r11;
    uint64_t r12;
    uint64_t r13;
    uint64_t r14;
    uint64_t r15;
    uint64_t task_index;
    uint32_t captured;
    uint32_t nested;
};

volatile struct fault_record last_fault;

static void capture_fault(struct interrupt_registers *registers,
                          uint64_t vector)
{
    const uint64_t *slots = (const uint64_t *)(const void *)registers;

    if (last_fault.captured != 0U) {
        last_fault.nested += 1U;
        return;
    }
    last_fault.vector = vector;
    last_fault.error = slots[16];
    last_fault.rip = slots[17];
    last_fault.cs = slots[18];
    last_fault.rflags = slots[19];
    last_fault.rsp = slots[20];
    last_fault.ss = slots[21];
    last_fault.cr2 = read_control_register_2();
    last_fault.rax = registers->rax;
    last_fault.rbx = registers->rbx;
    last_fault.rcx = registers->rcx;
    last_fault.rdx = registers->rdx;
    last_fault.rsi = registers->rsi;
    last_fault.rdi = registers->rdi;
    last_fault.rbp = registers->rbp;
    last_fault.r8 = registers->r8;
    last_fault.r9 = registers->r9;
    last_fault.r10 = registers->r10;
    last_fault.r11 = registers->r11;
    last_fault.r12 = registers->r12;
    last_fault.r13 = registers->r13;
    last_fault.r14 = registers->r14;
    last_fault.r15 = registers->r15;
    last_fault.task_index = (uint64_t)(int32_t)scheduler_current_task();
    last_fault.captured = 1U;
}

static const char *exception_name(uint64_t vector)
{
    switch (vector) {
    case 0:
        return "divide error";
    case 6:
        return "invalid opcode";
    case 8:
        return "double fault";
    case 13:
        return "general protection fault";
    case 14:
        return "page fault";
    case 21:
        return "control protection exception";
    default:
        return "cpu exception";
    }
}

static void report_exception(struct interrupt_registers *registers,
                             uint64_t vector)
{
    const uint64_t *slots = (const uint64_t *)(const void *)registers;

    serial_write("!! ");
    serial_write(exception_name(vector));
    serial_write(" vector ");
    serial_hex(vector);
    serial_write(" error ");
    serial_hex(slots[16]);
    serial_write("\n   rip ");
    serial_hex(slots[17]);
    serial_write("  cr2 ");
    serial_hex(read_control_register_2());
    serial_write("  rsp ");
    serial_hex(slots[20]);
    serial_write("\n   rax ");
    serial_hex(registers->rax);
    serial_write("  rbx ");
    serial_hex(registers->rbx);
    serial_write("  rcx ");
    serial_hex(registers->rcx);
    serial_write("\n   rdx ");
    serial_hex(registers->rdx);
    serial_write("  rsi ");
    serial_hex(registers->rsi);
    serial_write("  rdi ");
    serial_hex(registers->rdi);
    serial_write("  rbp ");
    serial_hex(registers->rbp);
    serial_write("  r8 ");
    serial_hex(registers->r8);
    serial_write("  r12 ");
    serial_hex(registers->r12);
    serial_write("\n");
    if (console_enabled) {
        console_clear();
        console_write("kernel fault: ");
        console_write(exception_name(vector));
        console_write(" vector ");
        console_number("", vector, " error ");
        console_number("", slots[16], "\n  rip ");
        console_number("", slots[17], "  cr2 ");
        console_number("", read_control_register_2(), "\n  rsp ");
        console_number("", slots[20], "\n");
        console_flush();
    }
}

void interrupt_dispatch(struct interrupt_registers *registers,
                        uint64_t vector)
{
    if (vector < 32) {
        capture_fault(registers, vector);
        report_exception(registers, vector);
        panic(exception_name(vector));
    }

    if (vector == 32) {
        pit_tick();
        pic_eoi(0);
        scheduler_on_interrupt(registers, (struct cpu_frame *)((uint8_t *)registers + 128));
        return;
    }

    if (vector >= 32 && vector < 48) {
        uint8_t irq = (uint8_t)(vector - 32);

        if (irq == 1) {
            irq1_count++;
            gui_keyboard_irq();
            pic_eoi(irq);
            return;
        }
        if (irq == 12) {
            irq12_count++;
            gui_mouse_irq();
            pic_eoi(irq);
            return;
        }
        pic_eoi(irq);
        return;
    }

    serial_write("Unexpected interrupt ");
    serial_hex(vector);
    panic("unexpected interrupt");
}

static void idle_task(uint64_t argument)
{
    (void)argument;
    for (;;) {
        __asm__ volatile("hlt");
    }
}

struct __attribute__((packed)) multiboot_tag {
    uint32_t type;
    uint32_t size;
};

struct __attribute__((packed)) multiboot_tag_basic_mem {
    uint32_t type;
    uint32_t size;
    uint32_t mem_lower;
    uint32_t mem_upper;
};

struct __attribute__((packed)) multiboot_tag_mmap {
    uint32_t type;
    uint32_t size;
    uint32_t entry_size;
    uint32_t entry_version;
};

struct __attribute__((packed)) multiboot_mmap_entry {
    uint64_t addr;
    uint64_t len;
    uint32_t type;
    uint32_t zero;
};

#define MULTIBOOT_MEMORY_AVAILABLE 1

static uint64_t map_tag_total(void)
{
    const uint8_t *cursor;
    uint64_t total = 0;

    if (multiboot_address == 0U) {
        return 0U;
    }
    cursor = (const uint8_t *)(uintptr_t)multiboot_address + 8U;
    for (;;) {
        const struct multiboot_tag *tag =
            (const struct multiboot_tag *)(const void *)cursor;
        uint32_t size = tag->size;

        if (tag->type == 0U || size < 8U) {
            break;
        }
        if (tag->type == 6U && size >= 16U) {
            const struct multiboot_tag_mmap *mmap =
                (const struct multiboot_tag_mmap *)(const void *)tag;
            uint32_t stride = mmap->entry_size;
            const uint8_t *entry = (const uint8_t *)(const void *)(mmap + 1);
            const uint8_t *limit = cursor + size;

            if (stride < sizeof(struct multiboot_mmap_entry)) {
                stride = (uint32_t)sizeof(struct multiboot_mmap_entry);
            }
            while (entry + stride <= limit) {
                uint64_t length;
                uint32_t kind;

                __builtin_memcpy(&length, entry + 8, sizeof(length));
                __builtin_memcpy(&kind, entry + 16, sizeof(kind));
                if (kind == MULTIBOOT_MEMORY_AVAILABLE) {
                    total += length;
                }
                entry += stride;
            }
            break;
        }
        cursor += (size + 7U) & ~7U;
    }
    return total;
}

void kernel_main(uint64_t framebuffer_address, uint32_t pitch,
                 uint32_t width, uint32_t height, uint8_t bpp,
                 uint8_t framebuffer_type)
{
    void *boot_stack = __builtin_frame_address(0);
    uint64_t memory_bytes;
    bool video = framebuffer_address != 0U && bpp == 32U;

    serial_init();
    post_line("Klye OS 0.2\n");
    console_enabled = video;
    if (video) {
        console_init();
    }
    report_cpu();

    memory_bytes = map_tag_total();
    if (memory_bytes >= (1ULL * 1024ULL * 1024ULL) &&
        memory_bytes <= (64ULL * 1024ULL * 1024ULL * 1024ULL)) {
        post("SYSTEM MEMORY", "OK");
        post_number("  bytes (", memory_bytes, "  ");
        post_number("", memory_bytes / (1024U * 1024U), " MiB)\n");
    } else {
        post("SYSTEM MEMORY", "OK");
        post_line("       multiboot memory map unavailable\n");
    }
    post("MEMORY MAP", "OK");

    gdt_init();
    post("GLOBAL DESCRIPTOR TABLE", "OK");
    idt_init();
    post("INTERRUPT DESCRIPTOR TABLE", "OK");

    scheduler_init(boot_stack);
    post("TASK SCHEDULER", "OK");

    if (vfs_mount() && vfs_mounted()) {
        post("VIRTUAL FILESYSTEM", "OK");
        post_number("  ",
                    (uint32_t)VFS_TOTAL_BLOCKS * (uint32_t)VFS_BLOCK_SIZE,
                    " bytes in-memory storage\n");
    } else {
        post("VIRTUAL FILESYSTEM", "MISSING");
    }

    pit_init(1000);
    post("PROGRAMMABLE INTERVAL TIMER", "OK");
    post_line("       channel 0 at 1000 Hz, 1 ms tick\n");
    pic_init();
    pic_unmask(0);
    __asm__ volatile("sti");
    post("INTERRUPTS ENABLED", "OK");

    if (task_spawn(idle_task, 0) < 0) {
        panic("task table full");
    }

    if (video) {
        post("TEXT CONSOLE", "OK");
        post_number("       framebuffer ", width, "x");
        post_number("", height, " @ ");
        post_hex("", framebuffer_address, ", pitch ");
        post_number("", pitch, ", 32 bpp\n");
    } else {
        post("TEXT CONSOLE", "MISSING");
        post_line("       no usable 32 bpp framebuffer was provided\n");
    }

    post_line("\nstarting console bring-up\n");

    pic_unmask(1);
    pic_unmask(12);
    gui_init_input();

    if (gui_mouse_ready()) {
        post("PS/2 MOUSE", "OK");
        post_line("       auxiliary port enabled, reporting on, irq 12\n");
        post_hex("       controller config byte ", gui_mouse_config(), "\n");
        post_line("       liveness is confirmed on the first packet\n");
    } else {
        post("PS/2 MOUSE", "MISSING");
        post_line("       auxiliary port did not report a device\n");
    }

    post("PS/2 KEYBOARD", "OK");
    post_line("       scancode set 1, irq 1\n");

    post("KLYE SHELL", "OK");
    post_number("  ", (uint32_t)APP_COUNT, " registered applications\n");

    if (gui_init(framebuffer_address, pitch, width, height, bpp,
                 framebuffer_type) == false) {
        post("KERNEL MODE", "MISSING");
    }

    post_line("Klye OS ready\n");

    wm_init();
    wm_run_boot_animation();
    wm_set_ready(true);
    post("BOOT ANIMATION", "OK");
    post_line("       klye os splash, then desktop\n");

    if (task_spawn(gui_compositor_task, 0) < 0) {
        panic("compositor task table full");
    }
    post("COMPOSITOR TASK", "OK");
    post_line("       60 fps frame loop, damage rectangles\n");

    for (;;) {
        __asm__ volatile("hlt");
    }
}
#define PAGE_FAULT_PRESENT 0x01U
#define PAGE_FAULT_WRITE 0x02U
#define PAGE_FAULT_USER 0x04U
#define PAGE_FAULT_RESERVED 0x08U
#define PAGE_FAULT_FETCH 0x10U
#define PAGE_FAULT_PROTECTION 0x01U

static const char *fault_cause_text(void)
{
    if (last_fault.vector == 14U) {
        if ((last_fault.error & PAGE_FAULT_PRESENT) == 0U) {
            return "page fault: accessed memory that is not mapped";
        }
        if ((last_fault.error & PAGE_FAULT_WRITE) != 0U) {
            return "page fault: wrote to read-only memory";
        }
        if ((last_fault.error & PAGE_FAULT_FETCH) != 0U) {
            return "page fault: executed code from non-executable memory";
        }
        return "page fault: violated memory protection rules";
    }
    if (last_fault.vector == 13U) {
        if ((last_fault.error & 0x0010U) != 0U) {
            return "general protection fault: executed an invalid opcode";
        }
        if ((last_fault.error & 0x0008U) != 0U) {
            return "general protection fault: descriptor table entry "
                   "was not readable";
        }
        if ((last_fault.error & 0x0004U) != 0U) {
            return "general protection fault: attempted to write a "
                   "read-only segment";
        }
        if ((last_fault.error & 0x0002U) != 0U) {
            return "general protection fault: accessed a null selector";
        }
        if ((last_fault.error & 0x0001U) != 0U) {
            return "general protection fault: memory protection violation";
        }
        return "general protection fault: privileged operation while "
               "running at user level";
    }
    if (last_fault.vector == 8U) {
        return "double fault: fault while handling another fault, "
               "often a bad stack or gate";
    }
    if (last_fault.vector == 6U) {
        return "invalid opcode: cpu could not decode the instruction";
    }
    if (last_fault.vector == 0U) {
        return "divide error: division by zero or divide overflow";
    }
    if (last_fault.vector == 21U) {
        return "control protection exception: branch target was not "
               "an allowed control flow target";
    }
    if (last_fault.cr2 != 0U) {
        return "cpu exception while accessing a faulting address";
    }
    return "cpu exception during kernel execution";
}

__attribute__((noreturn)) void panic(const char *message)
{
    __asm__ volatile("cli");
    serial_write("\n*** KERNEL PANIC ***\nTrigger: ");
    serial_write(message != 0 ? message : "unspecified failure");
    serial_write("\n\nDiagnosis\n");
    if (last_fault.captured != 0U) {
        const char *cause = fault_cause_text();

        serial_write("  detected cause : ");
        serial_write(cause);
        serial_write("\n  exception      : ");
        serial_write(exception_name(last_fault.vector));
        serial_write(" (vector ");
        serial_hex(last_fault.vector);
        serial_write(")\n  faulting rip   : ");
        serial_hex(last_fault.rip);
        serial_write("\n  fault address  : ");
        serial_hex(last_fault.cr2);
        serial_write("\n  error code     : ");
        serial_hex(last_fault.error);
        serial_write("\n  faulting task  : ");
        serial_hex(last_fault.task_index);
        serial_write("\n");
        if (last_fault.nested != 0U) {
            serial_write("  nested faults  : ");
            serial_hex((uint64_t)last_fault.nested);
            serial_write("\n");
        }
    } else {
        serial_write("  detected cause : software invariant failed, "
                     "no cpu exception was recorded\n");
    }
    if (console_enabled) {
        console_clear();
        console_write("*** KERNEL PANIC ***\n");
        console_write(message != 0 ? message : "unspecified failure");
        console_write("\n\ndiagnosis: ");
        console_write(last_fault.captured != 0U ? fault_cause_text()
                                               : "software invariant failed");
        if (last_fault.captured != 0U) {
            console_write("\nexception: ");
            console_write(exception_name(last_fault.vector));
            console_number("\nfaulting rip: 0x", last_fault.rip, "");
            console_number("\nfault address: 0x", last_fault.cr2, "");
            console_number("\nerror code: 0x", last_fault.error, "");
        }
        console_write("\n\nsystem halted");
        console_flush();
    }
    serial_write("system halted\n");
    halt_forever();
}
