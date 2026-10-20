#pragma once
#include <stdint.h>
#include <stdbool.h>

typedef struct {
    bool     rds_enable;
    bool     free_run;      /* allow RDS without pilot (out of spec, for bench tests) */
    uint16_t level;         /* DAC counts peak */
    int16_t  phase_deg;     /* 57 kHz phase trim */
    /* status (read-only) */
    volatile bool  pilot_lock;
    volatile float pilot_ampl;   /* ADC counts peak */
    volatile float pilot_hz;
    volatile float pilot_err;    /* rad, filtered */
    volatile uint32_t blocks;
    volatile uint32_t overruns;
    volatile uint16_t load_pct;  /* ISR CPU load */
    volatile uint32_t isr_cycles; /* cumulative ISR cycles (main-loop timing) */
    volatile uint8_t  dac_test;  /* DACTEST: 0 normal, 1 DC 2048 (DMA off), 2 1 kHz full-scale sine */
} dsp_t;

extern dsp_t dsp;

void dsp_init(void);              /* ADC1 + DAC1 + TIM6 + DMA, starts sampling */
void dsp_core_init(float fs_hz);  /* tables + NCO only (used by dsp_init and host test) */
int  dsp_dac_test(int mode);      /* DACTEST 0|1|2; returns HAL status (0 = ok), -1 = bad mode */
uint32_t dsp_dac_error(void);     /* hdac1.ErrorCode */
