#pragma once
#include <stdbool.h>

bool settings_load(void);    /* applies saved values to fm30/rds/dsp structs; false = defaults */
bool settings_save(void);
void settings_defaults(void);
