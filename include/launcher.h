#ifndef KLYE_LAUNCHER_H
#define KLYE_LAUNCHER_H

#include <stdbool.h>
#include <stdint.h>

#define LAUNCHER_MAX 24
#define LAUNCHER_NAME_MAX 48
#define LAUNCHER_TITLE_MAX 32
#define LAUNCHER_ENTRY_MAX 32
#define LAUNCHER_KIND_MAX 16
#define LAUNCHER_PATH_MAX 160

struct launcher {
    char file[LAUNCHER_NAME_MAX];
    char title[LAUNCHER_TITLE_MAX];
    char kind[LAUNCHER_KIND_MAX];
    char entry[LAUNCHER_ENTRY_MAX];
    char path[LAUNCHER_PATH_MAX];
    int icon;
    bool used;
    bool is_record;
};

void launcher_scan(void);
void launcher_reset(void);
int launcher_count(void);
const struct launcher *launcher_at(int index);
int launcher_find(const char *name);
int launcher_resolve(const char *name);
bool launcher_is_builtin(int index, char *entry, int entry_max);
bool launcher_is_bytecode(int index);

#endif
