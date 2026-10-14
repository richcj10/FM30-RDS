/* Minimal stand-ins so src/dsp.c and src/rds.c compile on a PC (tools/test_dsp.c). */
#pragma once
#include <stdint.h>
#include <stdbool.h>
#define DSP_TIM_ARR     745
#define DSP_BLOCK       228
#define RDS_LEVEL_DEF   400
#define PILOT_MIN_AMPL  15.0f
typedef struct { uint32_t CYCCNT; } DWT_stub_t;
static DWT_stub_t dwt_stub;
#define DWT (&dwt_stub)
