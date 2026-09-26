#ifndef KLYE_SHELL_H
#define KLYE_SHELL_H

#include <stdbool.h>
#include <stdint.h>

#define SHELL_MAX_TOKENS 16
#define SHELL_TOKEN_MAX 96
#define SHELL_HISTORY_MAX 32
#define SHELL_CWD_MAX 64

struct shell_state {
    char cwd[SHELL_CWD_MAX];
    char user[16];
    char host[16];
    int history_count;
    char history[SHELL_HISTORY_MAX][SHELL_MAX_TOKENS * SHELL_TOKEN_MAX / 4];
};

void shell_init(void);
void shell_execute(const char *line);
const struct shell_state *shell_get_state(void);
uint32_t shell_command_count(void);
const char *shell_last_command(void);
bool shell_run_app(const char *name);
void shell_machine_reboot(void);
void shell_machine_halt(void);
bool shell_reboot_pending(void);
bool shell_halt_pending(void);

#endif
