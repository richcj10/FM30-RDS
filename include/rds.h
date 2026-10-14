#pragma once
#include <stdint.h>
#include <stdbool.h>

typedef struct {
    uint16_t pi;
    uint8_t  pty;        /* 0..31 */
    bool     tp, ta, ms; /* ms: 1 = music */
    bool     stereo;     /* DI d0 */
    char     ps[9];      /* 8 chars, space padded */
    char     rt[65];     /* up to 64 chars */
    bool     rt_ab;      /* toggles when RT text changes */
} rds_params_t;

extern rds_params_t rds;

void     rds_init(void);
void     rds_set_ps(const char *s);
void     rds_set_rt(const char *s);
/* Next raw (not yet differentially encoded) bit of the RDS stream, 1187.5 bit/s.
 * Safe to call from the DSP interrupt. */
uint8_t  rds_next_bit(void);

/* exposed for host tests */
uint16_t rds_checkword(uint16_t data, uint16_t offset);
void     rds_build_group(uint16_t blocks[4]);
uint32_t rds_groups_sent(void);
