#ifndef KLYE_KAS_H
#define KLYE_KAS_H

#include <stdbool.h>
#include <stdint.h>

/* In-kernel assembler for .kby sources.  Produces the same bytecode as the
 * host tool, so a program assembled inside the OS is bit-identical to one
 * built by tools/kbasm.
 *
 * On failure returns false and points *error at a short static reason.
 */
bool kas_assemble(const char *source, int source_length, uint8_t *out,
                  int out_max, int *out_length, const char **error);

#endif
