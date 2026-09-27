#ifndef KLYE_SETJMP_H
#define KLYE_SETJMP_H

/* Freestanding setjmp/longjmp, implemented in setjmp.S.  The layout matches
 * the SysV x86-64 jmp_buf: rbx, rbp, r12-r15, rsp, rip.
 *
 * Lua's pcall is built on longjmp, so a kernel hosted Lua needs these even
 * though nothing else in the kernel uses them. */

typedef unsigned long jmp_buf[8];

int klye_setjmp(jmp_buf env);
__attribute__((noreturn)) void klye_longjmp(jmp_buf env, int value);

#define setjmp klye_setjmp
#define longjmp klye_longjmp

#endif
