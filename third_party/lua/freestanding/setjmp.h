#ifndef KLYE_FREESTANDING_SETJMP_H
#define KLYE_FREESTANDING_SETJMP_H

/* Shadows the hosted <setjmp.h> for the vendored Lua build.  lua_pcall is
 * implemented with longjmp, so a kernel hosted Lua cannot work without these.
 * See setjmp.S for the x86-64 implementation. */

typedef unsigned long jmp_buf[8];

int klye_setjmp(jmp_buf env);
__attribute__((noreturn)) void klye_longjmp(jmp_buf env, int value);

#define setjmp(env) klye_setjmp(env)
#define longjmp(env, value) klye_longjmp((env), (value))

#endif
