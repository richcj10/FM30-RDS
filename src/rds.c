/*
 * RDS group generator (IEC 62106): groups 0A (PS) and 2A (RadioText).
 * Pure C, no HAL — also compiled by the host test in tools/.
 */
#include "rds.h"
#include <string.h>

rds_params_t rds;

#define OFS_A  0x0FCu
#define OFS_B  0x198u
#define OFS_C  0x168u
#define OFS_D  0x1B4u

static uint32_t bits[4];        /* 4 x 26-bit blocks of the current group */
static int      bitpos = 104;   /* 104 => need a new group */
static uint32_t group_count;
static uint8_t  ps_seg, rt_seg;

uint16_t rds_checkword(uint16_t data, uint16_t offset)
{
    /* g(x) = x^10 + x^8 + x^7 + x^5 + x^4 + x^3 + 1 */
    uint32_t reg = (uint32_t)data << 10;
    for (int i = 25; i >= 10; i--)
        if (reg & (1u << i)) reg ^= 0x5B9u << (i - 10);
    return (uint16_t)((reg & 0x3FFu) ^ offset);
}

static uint8_t rt_segments(void)
{
    size_t n = strnlen(rds.rt, 64);
    if (n == 0) return 0;
    if (n < 64) n++;                    /* room for the 0x0D terminator */
    return (uint8_t)((n + 3) / 4);
}

static char rt_char(size_t i)
{
    size_t n = strnlen(rds.rt, 64);
    if (i < n)  return rds.rt[i];
    if (i == n) return 0x0D;
    return ' ';
}

void rds_build_group(uint16_t blk[4])
{
    uint8_t nrt = rt_segments();
    bool send_rt = nrt && (group_count & 1);   /* alternate 0A / 2A */
    uint16_t common = (uint16_t)((rds.tp ? 1u : 0u) << 10) | ((uint16_t)(rds.pty & 31) << 5);

    blk[0] = rds.pi;
    if (!send_rt) {
        /* 0A: PS, 2 chars per segment, DI bit d(3-seg), AF "none" */
        uint8_t s = ps_seg & 3;
        /* DI: seg0=d3 dynamic PTY, seg1=d2 compressed, seg2=d1 artificial head, seg3=d0 stereo */
        uint8_t di = (s == 3 && rds.stereo) ? 1 : 0;
        blk[1] = (0u << 12) | (0u << 11) | common | ((rds.ta ? 1u : 0u) << 4) |
                 ((rds.ms ? 1u : 0u) << 3) | ((uint16_t)di << 2) | s;
        blk[2] = 0xE0CD;                         /* 0 AFs + filler */
        blk[3] = ((uint16_t)(uint8_t)rds.ps[s * 2] << 8) | (uint8_t)rds.ps[s * 2 + 1];
        ps_seg = (ps_seg + 1) & 3;
    } else {
        /* 2A: RadioText, 4 chars per segment */
        if (rt_seg >= nrt) rt_seg = 0;
        uint8_t s = rt_seg;
        blk[1] = (2u << 12) | (0u << 11) | common | ((rds.rt_ab ? 1u : 0u) << 4) | s;
        blk[2] = ((uint16_t)(uint8_t)rt_char(s * 4) << 8)     | (uint8_t)rt_char(s * 4 + 1);
        blk[3] = ((uint16_t)(uint8_t)rt_char(s * 4 + 2) << 8) | (uint8_t)rt_char(s * 4 + 3);
        rt_seg++;
    }
    group_count++;
}

static void load_group(void)
{
    uint16_t b[4];
    static const uint16_t ofs[4] = { OFS_A, OFS_B, OFS_C, OFS_D };
    rds_build_group(b);
    for (int i = 0; i < 4; i++)
        bits[i] = ((uint32_t)b[i] << 10) | rds_checkword(b[i], ofs[i]);
    bitpos = 0;
}

uint8_t rds_next_bit(void)
{
    if (bitpos >= 104) load_group();
    int blk = bitpos / 26, k = 25 - (bitpos % 26);
    bitpos++;
    return (uint8_t)((bits[blk] >> k) & 1u);
}

uint32_t rds_groups_sent(void) { return group_count; }

void rds_set_ps(const char *s)
{
    char tmp[9];
    size_t i = 0;
    for (; i < 8 && s[i]; i++) tmp[i] = s[i];
    for (; i < 8; i++) tmp[i] = ' ';
    tmp[8] = 0;
    memcpy(rds.ps, tmp, 9);
}

void rds_set_rt(const char *s)
{
    char tmp[65];
    size_t n = strnlen(s, 64);
    memcpy(tmp, s, n);
    tmp[n] = 0;
    if (strcmp(tmp, rds.rt) != 0) {
        memcpy(rds.rt, tmp, n + 1);
        rds.rt_ab = !rds.rt_ab;
        rt_seg = 0;
    }
}

void rds_init(void)
{
    if (rds.ps[0] == 0) rds_set_ps("FM30");
    bitpos = 104;
}
