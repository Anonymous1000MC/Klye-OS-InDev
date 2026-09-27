#ifndef KLYE_KERNEL_H
#define KLYE_KERNEL_H

#include <stdint.h>

struct cpu_frame {
    uint64_t error_code;
    uint64_t rip;
    uint64_t cs;
    uint64_t rflags;
    uint64_t rsp;
    uint64_t ss;
};

struct interrupt_registers {
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
};

void kernel_main(uint64_t framebuffer_address, uint32_t pitch,
                 uint32_t width, uint32_t height, uint8_t bpp,
                 uint8_t framebuffer_type);

extern uint32_t multiboot_address;
void gdt_init(void);
void idt_init(void);
void pic_init(void);
void pic_unmask(uint8_t irq);
void pic_eoi(uint8_t irq);
void pit_init(uint32_t frequency);
void pit_tick(void);
uint64_t pit_ticks(void);
void interrupt_dispatch(struct interrupt_registers *registers,
                        uint64_t vector);
void serial_init(void);
void serial_write(const char *text);
void serial_write_decimal(uint64_t value);
void panic(const char *message);
__attribute__((noreturn)) void halt_forever(void);

#endif
