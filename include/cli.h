#pragma once
#include <stdint.h>

void cli_init(void);
void cli_task(void);                 /* parse any complete lines */
void cli_print(const char *s);
void cli_status_json(void);
extern volatile uint32_t g_ovm_count;
