/* Settings: a store, a load, a save, and a way to ask.  See settings.c. */
#ifndef INCLUDE_SETTINGS_H
#define INCLUDE_SETTINGS_H

#include <stdbool.h>
#include <stdint.h>

void settings_load(void);
void settings_save(void);

uint32_t settings_get(const char *key);
bool settings_set(const char *key, uint32_t value);
bool settings_valid(const char *key);

uint32_t settings_count_entries(void);
const char *settings_key_at(uint32_t index);
uint32_t settings_value_at(uint32_t index);
uint32_t settings_low_at(uint32_t index);
uint32_t settings_high_at(uint32_t index);

#endif
