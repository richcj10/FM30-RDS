#pragma once
#include <stdint.h>
#include <stdbool.h>

typedef enum {
    FM30_OFF = 0,      /* RF disabled (U7 = 0)                 */
    FM30_TUNING,       /* U3 written, waiting for Vtune settle */
    FM30_RAMP,         /* U6 set, U7 soft-starting             */
    FM30_ON_AIR,       /* locked, at target power              */
    FM30_FAULT         /* no lock / lost lock, U7 = 0, retrying */
} fm30_state_t;

typedef struct {
    fm30_state_t state;
    uint16_t f10;          /* MHz * 10                     */
    uint16_t power_target; /* 0..POWER_MAX                 */
    uint16_t power_now;    /* last value written to U7     */
    uint16_t u6;           /* last value written to U6     */
    int16_t  u6_trim;      /* added to table value         */
    int16_t  u6_manual;    /* -1 = auto (table)            */
    uint8_t  volume;
    int8_t   balance;      /* -127..127, + = right         */
    bool     tx_enable;
    float    vtune;        /* volts, last reading          */
    float    vtune_ref;    /* tracked locked value         */
    float    vt_scale;
    uint32_t fault_count;
    const char *fault_msg;
} fm30_t;

extern fm30_t fm30;

void  fm30_init(void);
void  fm30_task(void);                 /* call often from main loop */
void  fm30_set_freq(uint16_t f10);     /* starts full retune sequence */
void  fm30_set_power(uint16_t code);
void  fm30_set_tx(bool on);
void  fm30_set_vol_bal(uint8_t vol, int8_t bal);
void  fm30_set_u6_trim(int16_t trim);
void  fm30_set_u6_manual(int16_t code); /* -1 = back to auto */
float fm30_read_vtune(void);
float fm30_vtune_expected(uint16_t f10);
uint16_t fm30_u6_code(uint16_t f10);
const char *fm30_state_name(fm30_state_t s);
void  delay_us(uint32_t us);           /* DWT busy-wait (bus + LCD timing) */
