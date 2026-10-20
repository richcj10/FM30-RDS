/*
 * Settings persisted in the last 2 KB flash page (STM32G431KB, 128 KB).
 * Note: a SAVE stalls the CPU ~25 ms, causing a brief RDS glitch.
 */
#include "config.h"
#include "settings.h"
#include "fm30.h"
#include "rds.h"
#include "dsp.h"
#include "lcd.h"
#include "ui.h"
#include <string.h>
#include <stddef.h>

#define SETTINGS_ADDR   0x0801F800u
#define SETTINGS_MAGIC  0x46333031u   /* "F301" */
#define SETTINGS_VER    1

typedef struct {
    uint32_t magic, version;
    uint16_t f10, power;
    uint8_t  tx, volume;
    int8_t   balance, pad0;
    int16_t  u6_trim, pad1;
    float    vt_scale;
    uint16_t pi;
    uint8_t  pty, tp, ta, ms, stereo, rds_en;
    uint16_t level;
    int16_t  phase_deg;
    char     ps[9];
    char     rt[65];
    uint8_t  lcd_off, lcd_page;      /* in former padding: old saves read 0 = LCD on, rotate */
    uint8_t  lcd_rot_off;            /* ~rotation mask: old saves read 0 = all pages */
    uint8_t  pad2[3];
    uint32_t sum;
} store_t;

_Static_assert(sizeof(store_t) % 8 == 0, "store_t must be a multiple of 8 bytes");

static uint32_t checksum(const store_t *s)
{
    const uint8_t *p = (const uint8_t *)s;
    uint32_t h = 2166136261u;                       /* FNV-1a */
    for (size_t i = 0; i < offsetof(store_t, sum); i++) { h ^= p[i]; h *= 16777619u; }
    return h;
}

void settings_defaults(void)
{
    fm30.f10 = FREQ_DEF_X10;
    fm30.power_target = POWER_DEF;
    fm30.tx_enable = false;          /* RF stays off until a TX ON command */
    fm30.volume = VOLUME_DEF;
    fm30.balance = 0;
    fm30.u6_trim = 0;
    fm30.vt_scale = VTUNE_SCALE_DEF;
    rds.pi = 0x1234; rds.pty = 0; rds.tp = 0; rds.ta = 0; rds.ms = 1; rds.stereo = 1;
    rds_set_ps("FM30");
    rds_set_rt("");
    dsp.rds_enable = true;
    dsp.level = RDS_LEVEL_DEF;
    dsp.phase_deg = RDS_PHASE_DEF;
    lcd_enable(true);
    ui_set_page(0);
    ui_set_rotation(UI_ROT_ALL);
}

bool settings_load(void)
{
    const store_t *s = (const store_t *)SETTINGS_ADDR;
    settings_defaults();
    if (s->magic != SETTINGS_MAGIC || s->version != SETTINGS_VER || s->sum != checksum(s))
        return false;
    fm30.f10 = s->f10;
    fm30.power_target = s->power > POWER_MAX ? POWER_MAX : s->power;
    /* s->tx is still saved but not restored: TX always starts off. */
    fm30.volume = s->volume;
    fm30.balance = s->balance;
    fm30.u6_trim = s->u6_trim;
    fm30.vt_scale = s->vt_scale;
    rds.pi = s->pi; rds.pty = s->pty; rds.tp = s->tp; rds.ta = s->ta; rds.ms = s->ms; rds.stereo = s->stereo;
    char tmp[65];
    memcpy(tmp, s->ps, 9); tmp[8] = 0;  rds_set_ps(tmp);
    memcpy(tmp, s->rt, 65); tmp[64] = 0; rds_set_rt(tmp);
    dsp.rds_enable = s->rds_en;
    dsp.level = s->level;
    dsp.phase_deg = s->phase_deg;
    lcd_enable(!s->lcd_off);
    ui_set_page(s->lcd_page);
    ui_set_rotation((uint8_t)~s->lcd_rot_off);
    return true;
}

bool settings_save(void)
{
    store_t s;
    memset(&s, 0, sizeof s);
    s.magic = SETTINGS_MAGIC; s.version = SETTINGS_VER;
    s.f10 = fm30.f10; s.power = fm30.power_target; s.tx = fm30.tx_enable;
    s.volume = fm30.volume; s.balance = fm30.balance; s.u6_trim = fm30.u6_trim;
    s.vt_scale = fm30.vt_scale;
    s.pi = rds.pi; s.pty = rds.pty; s.tp = rds.tp; s.ta = rds.ta; s.ms = rds.ms; s.stereo = rds.stereo;
    s.rds_en = dsp.rds_enable; s.level = dsp.level; s.phase_deg = dsp.phase_deg;
    s.lcd_off = !lcd_is_enabled(); s.lcd_page = ui_get_page();
    s.lcd_rot_off = (uint8_t)~ui_get_rotation();
    memcpy(s.ps, rds.ps, 9);
    memcpy(s.rt, rds.rt, 65);
    s.sum = checksum(&s);

    HAL_FLASH_Unlock();
    __HAL_FLASH_CLEAR_FLAG(FLASH_FLAG_ALL_ERRORS);
    FLASH_EraseInitTypeDef e = {0};
    uint32_t perr;
    e.TypeErase = FLASH_TYPEERASE_PAGES;
    e.Banks = FLASH_BANK_1;
    e.Page = (SETTINGS_ADDR - FLASH_BASE) / FLASH_PAGE_SIZE;
    e.NbPages = 1;
    bool ok = HAL_FLASHEx_Erase(&e, &perr) == HAL_OK;
    const uint64_t *src = (const uint64_t *)&s;
    for (size_t i = 0; ok && i < sizeof s / 8; i++)
        ok = HAL_FLASH_Program(FLASH_TYPEPROGRAM_DOUBLEWORD, SETTINGS_ADDR + i * 8, src[i]) == HAL_OK;
    HAL_FLASH_Lock();
    return ok;
}
