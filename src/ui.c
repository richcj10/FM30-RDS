/*
 * Front-panel screens (2x8). Only fills the lcd.c shadow buffer at ~5 Hz;
 * lcd_task() pushes the changed chars to the display.
 *
 *  Page 1 (freq/state)  Page 2 (RDS PS)  Page 3 (power/Vtune)  Page 4 (RDS status)
 *   " 98.1MHz"           "RDS  PS:"       "PWR  88%"            "PLT LOCK"
 *   "ON AIR L"           "MY STATN"       "Vt 3.80V"            "RDS  ON "
 *
 *  Pages 5..8: message slots set over serial (LCD MSG n top|bottom), e.g.
 *   "WiFi OK "
 *   "192.168.1.23"  <- bottom row scrolls when longer than 8 chars
 *
 * The pages in the rotation mask rotate every 3 s (longer if the bottom row
 * is still scrolling); empty message slots are skipped. LCD PAGE n pins one.
 * Boot, TUNING and FAULT screens override the pages.
 */
#include "config.h"
#include "ui.h"
#include "lcd.h"
#include "fm30.h"
#include "dsp.h"
#include "rds.h"
#include "cli.h"
#include <stdio.h>
#include <string.h>
#include <ctype.h>

static uint32_t t_boot, t_draw, t_page, t_ovm, ovm_seen;
static bool     ovm_recent;
static uint8_t  page = 1, pinned, dots, rot_mask = UI_ROT_ALL;

static struct { char top[LCD_COLS + 1], bot[UI_MSG_LEN + 1]; } msg[UI_MSG_SLOTS];

/* bottom-row marquee */
static char     scr_txt[UI_MSG_LEN + 1];
static uint16_t scr_pos, scr_hold;
static bool     scr_done = true;

/* fault_msg -> 8-char text; anything else: first 8 chars, upper case */
static const struct { const char *msg, *lcd; } fault_txt[] = {
    { "lock timeout", "LOCK TMO" },
    { "vtune railed", "RAILED"   },
    { "lock lost",    "LOCK LST" },
};

static void put_line(uint8_t line, const char *s)       /* pad / clip to 8 */
{
    char b[LCD_COLS + 1];
    size_t n = strlen(s);
    if (n > LCD_COLS) n = LCD_COLS;
    memset(b, ' ', LCD_COLS);
    memcpy(b, s, n);
    b[LCD_COLS] = 0;
    lcd_print(line, 0, b);
}

static void scroll_reset(void) { scr_txt[0] = 0; scr_done = true; }

/* bottom row: static if it fits, else scrolls one char per redraw with a
 * pause at the start; scr_done is set after each full pass */
static void bottom_line(const char *s)
{
    size_t n = strnlen(s, UI_MSG_LEN);
    if (strncmp(s, scr_txt, UI_MSG_LEN)) {               /* new text: restart */
        memcpy(scr_txt, s, n);
        scr_txt[n] = 0;
        scr_pos = 0;
        scr_hold = UI_SCROLL_HOLD;
        scr_done = n <= LCD_COLS;
    }
    if (n <= LCD_COLS) { put_line(1, s); return; }

    size_t period = n + UI_SCROLL_GAP;
    char b[LCD_COLS + 1];
    for (size_t i = 0; i < LCD_COLS; i++) {
        size_t k = (scr_pos + i) % period;
        b[i] = k < n ? s[k] : ' ';
    }
    b[LCD_COLS] = 0;
    lcd_print(1, 0, b);

    if (scr_hold) scr_hold--;
    else if (++scr_pos >= period) { scr_pos = 0; scr_hold = UI_SCROLL_HOLD; scr_done = true; }
}

static void freq_line(void)
{
    char t[16];
    snprintf(t, sizeof t, "%3u.%uMHz", (unsigned)(fm30.f10 / 10u), (unsigned)(fm30.f10 % 10u));
    put_line(0, t);
}

static void page_freq(void)
{
    bool on_air = fm30.state == FM30_ON_AIR || fm30.state == FM30_RAMP;
    char flag = ovm_recent ? '*' : (on_air ? 'L' : ' ');
    char t[16];
    snprintf(t, sizeof t, "%-6s %c", on_air ? "ON AIR" : "TX OFF", flag);
    freq_line();
    bottom_line(t);
}

static void page_ps(void)
{
    put_line(0, "RDS  PS:");
    bottom_line(rds.ps);
}

static void page_power(void)
{
    char t[16];
    unsigned pct = (unsigned)((uint32_t)fm30.power_now * 100u / POWER_MAX);
    snprintf(t, sizeof t, "PWR %3u%%", pct);
    put_line(0, t);
    float v = fm30.vtune;
    unsigned cv = v > 0 ? (unsigned)(v * 100.0f + 0.5f) : 0;       /* centivolts */
    if (cv < 1000) snprintf(t, sizeof t, "Vt %u.%02uV", cv / 100u, cv % 100u);
    else           snprintf(t, sizeof t, "Vt%3u.%uV", cv / 100u, (cv / 10u) % 10u);
    bottom_line(t);
}

static void page_rds(void)
{
    put_line(0, dsp.pilot_lock ? "PLT LOCK" : "PLT ----");
    if (!dsp.rds_enable)                       bottom_line("RDS OFF");
    else if (dsp.pilot_lock || dsp.free_run)   bottom_line("RDS  ON");
    else                                       bottom_line("RDS MUT");   /* enabled, no pilot */
}

static void page_msg(uint8_t slot)
{
    put_line(0, msg[slot].top);
    bottom_line(msg[slot].bot);
}

static void screen_fault(void)
{
    const char *m = fm30.fault_msg ? fm30.fault_msg : "";
    char t[LCD_COLS + 1];
    unsigned i;
    for (i = 0; i < sizeof fault_txt / sizeof fault_txt[0]; i++)
        if (!strcmp(m, fault_txt[i].msg)) { put_line(1, fault_txt[i].lcd); break; }
    if (i == sizeof fault_txt / sizeof fault_txt[0]) {
        for (i = 0; i < LCD_COLS && m[i]; i++) t[i] = (char)toupper((unsigned char)m[i]);
        t[i] = 0;
        put_line(1, t);
    }
    put_line(0, "FAULT");
}

static bool page_ok(uint8_t p)              /* in the rotation and has content */
{
    if (!(rot_mask & (1u << (p - 1)))) return false;
    if (p > UI_BUILTIN_PAGES) {
        uint8_t s = (uint8_t)(p - UI_BUILTIN_PAGES - 1);
        return msg[s].top[0] || msg[s].bot[0];
    }
    return true;
}

static uint8_t next_page(uint8_t p)
{
    for (uint8_t k = 1; k <= UI_PAGES; k++) {
        uint8_t q = (uint8_t)((p - 1 + k) % UI_PAGES + 1);
        if (page_ok(q)) return q;
    }
    return 1;
}

void ui_init(void)
{
    t_boot = t_page = HAL_GetTick();
    ovm_seen = g_ovm_count;
}

void ui_task(void)
{
    uint32_t now = HAL_GetTick();
    if (now - t_draw < UI_PERIOD_MS) return;
    t_draw = now;

    if (g_ovm_count != ovm_seen) { ovm_seen = g_ovm_count; t_ovm = now; ovm_recent = true; }
    if (now - t_ovm >= UI_OVM_HOLD_MS) ovm_recent = false;

    if (now - t_boot < UI_BOOT_MS) {
        const char *ver = strrchr(FW_VERSION, ' ');
        char t[16];
        snprintf(t, sizeof t, "fw %s", ver ? ver + 1 : FW_VERSION);
        put_line(0, "FM30 RDS");
        put_line(1, t);
        return;
    }
    if (fm30.state == FM30_TUNING) {
        static const char *const anim[] = { "TUNING", "TUNING.", "TUNING.." };
        dots = (uint8_t)((dots + 1) % 3);
        freq_line();
        put_line(1, anim[dots]);
        return;
    }
    if (fm30.state == FM30_FAULT) { screen_fault(); return; }

    if (pinned) {
        page = pinned;
    } else if (!page_ok(page)) {
        page = next_page(page);
        t_page = now;
        scroll_reset();
    } else if (now - t_page >= UI_PAGE_MS && scr_done) {
        uint8_t np = next_page(page);
        if (np != page) { page = np; scroll_reset(); }
        t_page = now;
    }
    switch (page) {
    case 1:  page_freq();  break;
    case 2:  page_ps();    break;
    case 3:  page_power(); break;
    case 4:  page_rds();   break;
    default: page_msg((uint8_t)(page - UI_BUILTIN_PAGES - 1)); break;
    }
}

void ui_set_page(uint8_t p)
{
    pinned = p <= UI_PAGES ? p : 0;
    page = pinned ? pinned : 1;
    t_page = HAL_GetTick();
    scroll_reset();
}

uint8_t ui_get_page(void) { return pinned; }

void ui_set_rotation(uint8_t mask) { rot_mask = mask ? mask : UI_ROT_ALL; }
uint8_t ui_get_rotation(void) { return rot_mask; }

void ui_set_msg(uint8_t slot, const char *top, const char *bottom)
{
    if (slot < 1 || slot > UI_MSG_SLOTS) return;
    slot--;
    size_t n = strnlen(top, LCD_COLS);
    memcpy(msg[slot].top, top, n);
    msg[slot].top[n] = 0;
    n = strnlen(bottom, UI_MSG_LEN);
    memcpy(msg[slot].bot, bottom, n);
    msg[slot].bot[n] = 0;
}
