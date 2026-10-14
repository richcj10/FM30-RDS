/*
 * Line-based ASCII command interface on the ST-Link USB VCP (115200 8N1).
 * Every command answers "OK ..." or "ERR ...". STATUS answers one JSON line.
 */
#include "config.h"
#include "cli.h"
#include "fm30.h"
#include "rds.h"
#include "dsp.h"
#include "settings.h"
#include "lcd.h"
#include "ui.h"
#include <string.h>
#include <stdlib.h>
#include <stdio.h>
#include <ctype.h>

#if VCP_USE_LPUART1
#define VCP           LPUART1
#define VCP_IRQn      LPUART1_IRQn
#define VCP_IRQHandler LPUART1_IRQHandler
#define VCP_AF        GPIO_AF12_LPUART1
#else
#define VCP           USART2
#define VCP_IRQn      USART2_IRQn
#define VCP_IRQHandler USART2_IRQHandler
#define VCP_AF        GPIO_AF7_USART2
#endif

volatile uint32_t g_ovm_count;
static uint32_t status_period_ms, t_status;

/* ---------------- UART ---------------- */
static UART_HandleTypeDef huart;
static volatile char rx_ring[256];
static volatile uint8_t rx_head, rx_tail;

void VCP_IRQHandler(void)
{
    if (VCP->ISR & USART_ISR_ORE) VCP->ICR = USART_ICR_ORECF;
    while (VCP->ISR & USART_ISR_RXNE_RXFNE) {
        char c = (char)VCP->RDR;
        uint8_t nh = rx_head + 1;
        if (nh != rx_tail) { rx_ring[rx_head] = c; rx_head = nh; }
    }
}

void cli_print(const char *s)
{
    HAL_UART_Transmit(&huart, (const uint8_t *)s, (uint16_t)strlen(s), 100);
}

static void printfln(const char *fmt, ...) __attribute__((format(printf, 1, 2)));
#include <stdarg.h>
static void printfln(const char *fmt, ...)
{
    char buf[768];        /* STATUS JSON ~450 B, ~620 B worst case with a full escaped RT */
    va_list ap;
    va_start(ap, fmt);
    int n = vsnprintf(buf, sizeof buf - 2, fmt, ap);
    va_end(ap);
    if (n < 0) return;
    if (n > (int)sizeof buf - 3) n = sizeof buf - 3;
    buf[n++] = '\r'; buf[n++] = '\n'; buf[n] = 0;
    cli_print(buf);
}

void cli_init(void)
{
    GPIO_InitTypeDef g = {0};
    __HAL_RCC_GPIOA_CLK_ENABLE();
#if VCP_USE_LPUART1
    __HAL_RCC_LPUART1_CLK_ENABLE();
#else
    __HAL_RCC_USART2_CLK_ENABLE();
#endif
    g.Pin = GPIO_PIN_2 | GPIO_PIN_3;
    g.Mode = GPIO_MODE_AF_PP;
    g.Pull = GPIO_PULLUP;
    g.Speed = GPIO_SPEED_FREQ_LOW;
    g.Alternate = VCP_AF;
    HAL_GPIO_Init(GPIOA, &g);

    huart.Instance = VCP;
    huart.Init.BaudRate = VCP_BAUD;
    huart.Init.WordLength = UART_WORDLENGTH_8B;
    huart.Init.StopBits = UART_STOPBITS_1;
    huart.Init.Parity = UART_PARITY_NONE;
    huart.Init.Mode = UART_MODE_TX_RX;
    huart.Init.HwFlowCtl = UART_HWCONTROL_NONE;
    huart.Init.OverSampling = UART_OVERSAMPLING_16;
    huart.Init.OneBitSampling = UART_ONE_BIT_SAMPLE_DISABLE;
    huart.Init.ClockPrescaler = UART_PRESCALER_DIV1;
    if (HAL_UART_Init(&huart) != HAL_OK) Error_Handler();

    VCP->CR1 |= USART_CR1_RXNEIE_RXFNEIE;
    HAL_NVIC_SetPriority(VCP_IRQn, 0, 0);   /* above the DSP DMA ISR (prio 1): RDR has no FIFO, a byte arrives every 87 us */
    HAL_NVIC_EnableIRQ(VCP_IRQn);
}

/* ---------------- helpers ---------------- */
static void fmt_fixed(char *out, size_t n, float v, int decimals)
{
    int scale = decimals == 1 ? 10 : decimals == 2 ? 100 : 1000;
    long x = (long)(v * scale + (v >= 0 ? 0.5f : -0.5f));
    const char *sign = (x < 0) ? "-" : "";
    if (x < 0) x = -x;
    snprintf(out, n, "%s%ld.%0*ld", sign, x / scale, decimals, x % scale);
}

static void json_str(char *out, size_t n, const char *s)
{
    size_t j = 0;
    for (size_t i = 0; s[i] && j + 3 < n; i++) {
        char c = s[i];
        if (c == '"' || c == '\\') { out[j++] = '\\'; out[j++] = c; }
        else if ((unsigned char)c >= 0x20) out[j++] = c;
    }
    out[j] = 0;
}

static bool parse_bool(const char *a, bool *v)
{
    if (!a) return false;
    if (!strcasecmp(a, "1") || !strcasecmp(a, "ON"))  { *v = true;  return true; }
    if (!strcasecmp(a, "0") || !strcasecmp(a, "OFF")) { *v = false; return true; }
    return false;
}

void cli_status_json(void)
{
    char vt[16], vte[16], phz[16], pa[16], perr[16], ps[24], rt[140], vsc[16];
    fmt_fixed(vt, sizeof vt, fm30.vtune, 2);
    fmt_fixed(vte, sizeof vte, fm30_vtune_expected(fm30.f10), 2);
    fmt_fixed(phz, sizeof phz, dsp.pilot_hz, 2);
    fmt_fixed(pa, sizeof pa, dsp.pilot_ampl, 1);
    fmt_fixed(perr, sizeof perr, dsp.pilot_err, 3);
    fmt_fixed(vsc, sizeof vsc, fm30.vt_scale, 3);
    json_str(ps, sizeof ps, rds.ps);
    json_str(rt, sizeof rt, rds.rt);
    printfln("{\"fw\":\"" FW_VERSION "\",\"state\":\"%s\",\"fault\":\"%s\",\"faults\":%lu,"
             "\"freq\":%u.%u,\"tx\":%d,\"power\":%u,\"power_target\":%u,\"u6\":%u,\"u6_trim\":%d,"
             "\"vtune\":%s,\"vtune_expected\":%s,\"vt_scale\":%s,\"vol\":%u,\"bal\":%d,\"ovm\":%lu,"
             "\"pilot_lock\":%d,\"pilot_hz\":%s,\"pilot_ampl\":%s,\"pilot_err\":%s,"
             "\"rds\":%d,\"rds_level\":%u,\"rds_phase\":%d,\"pi\":\"%04X\",\"pty\":%u,\"tp\":%d,\"ta\":%d,"
             "\"ms\":%d,\"stereo\":%d,\"ps\":\"%s\",\"rt\":\"%s\",\"groups\":%lu,"
             "\"dsp_load\":%u,\"dsp_overruns\":%lu,\"hse\":%lu,\"uptime\":%lu,\"loop_max_us\":%lu}",
             fm30_state_name(fm30.state), fm30.fault_msg ? fm30.fault_msg : "", (unsigned long)fm30.fault_count,
             fm30.f10 / 10, fm30.f10 % 10, fm30.tx_enable, fm30.power_now, fm30.power_target, fm30.u6, fm30.u6_trim,
             vt, vte, vsc, fm30.volume, fm30.balance, (unsigned long)g_ovm_count,
             dsp.pilot_lock, phz, pa, perr,
             dsp.rds_enable, dsp.level, dsp.phase_deg, rds.pi, rds.pty, rds.tp, rds.ta,
             rds.ms, rds.stereo, ps, rt, (unsigned long)rds_groups_sent(),
             dsp.load_pct, (unsigned long)dsp.overruns, (unsigned long)g_sysclk_from_hse,
             (unsigned long)(HAL_GetTick() / 1000), (unsigned long)g_loop_max_us);
}

static void help(void)
{
    static const char *const lines[] = {
        "FREQ <MHz>            e.g. FREQ 98.1 (87.5-108.0)",
        "POWER <0-3968|n%>     RF power code (U7) or percent of max",
        "TX ON|OFF             RF on/off",
        "VOL <0-255>  BAL <-127..127>",
        "U6TRIM <n>  U6 <code>|AUTO   deviation compensation trim/override",
        "VTSCALE <x>           Vtune = Vadc * x (default 3.0)",
        "RDS ON|OFF  RDSFREE ON|OFF  RDSLEVEL <0-2047>  RDSPHASE <deg>",
        "PI <hex>  PTY <0-31>  TP|TA|MS|STEREO <0|1>",
        "PS <text>  RT <text>  (RT with no text clears it)",
        "STATUS [period_s]     JSON status; period 0 = off",
        "LCD ON|OFF  LCD PAGE <0-8>   pin a page (5-8 = MSG 1-4); 0 = rotate",
        "LCD MSG <1-4> [top]|<bottom> message page; bottom scrolls if > 8; no text = clear",
        "LCD ROTATE <pages>|ALL       pages in rotation, e.g. LCD ROTATE 1 5 6",
        "SAVE  DEFAULTS  HELP",
    };
    for (unsigned i = 0; i < sizeof lines / sizeof lines[0]; i++) printfln("%s", lines[i]);
    printfln("OK");
}

/* ---------------- command dispatch ---------------- */
static void exec(char *line)
{
    while (*line == ' ') line++;
    if (!*line) return;
    char *cmd = line, *rest = strchr(line, ' ');
    if (rest) { *rest++ = 0; while (*rest == ' ') rest++; } else rest = "";
    for (char *p = cmd; *p; p++) *p = (char)toupper((unsigned char)*p);
    bool b;

    if (!strcmp(cmd, "FREQ")) {
        float mhz = strtof(rest, NULL);
        int f10 = (int)(mhz * 10.0f + 0.5f);
        if (f10 < FREQ_MIN_X10 || f10 > FREQ_MAX_X10) { printfln("ERR range 87.5-108.0"); return; }
        fm30_set_freq((uint16_t)f10);
        printfln("OK FREQ %d.%d", f10 / 10, f10 % 10);
    } else if (!strcmp(cmd, "POWER")) {
        char *end;
        long v = strtol(rest, &end, 0);
        if (end == rest) { printfln("ERR value"); return; }
        if (*end == '%') v = v * POWER_MAX / 100;
        if (v < 0) v = 0;
        if (v > POWER_MAX) v = POWER_MAX;
        fm30_set_power((uint16_t)v);
        printfln("OK POWER %ld", v);
    } else if (!strcmp(cmd, "TX")) {
        if (!parse_bool(rest, &b)) { printfln("ERR ON|OFF"); return; }
        fm30_set_tx(b);
        printfln("OK TX %d", b);
    } else if (!strcmp(cmd, "VOL")) {
        long v = strtol(rest, NULL, 0);
        if (v < 0 || v > 255) { printfln("ERR 0-255"); return; }
        fm30_set_vol_bal((uint8_t)v, fm30.balance);
        printfln("OK VOL %ld", v);
    } else if (!strcmp(cmd, "BAL")) {
        long v = strtol(rest, NULL, 0);
        if (v < -127 || v > 127) { printfln("ERR -127..127"); return; }
        fm30_set_vol_bal(fm30.volume, (int8_t)v);
        printfln("OK BAL %ld", v);
    } else if (!strcmp(cmd, "U6TRIM")) {
        long v = strtol(rest, NULL, 0);
        if (v < -1000 || v > 1000) { printfln("ERR -1000..1000"); return; }
        fm30_set_u6_trim((int16_t)v);
        printfln("OK U6TRIM %ld", v);
    } else if (!strcmp(cmd, "U6")) {
        if (!strcasecmp(rest, "AUTO")) { fm30_set_u6_manual(-1); printfln("OK U6 AUTO"); return; }
        long v = strtol(rest, NULL, 0);
        if (v < 0 || v > 4095) { printfln("ERR 0-4095|AUTO"); return; }
        fm30_set_u6_manual((int16_t)v);
        printfln("OK U6 %ld", v);
    } else if (!strcmp(cmd, "VTSCALE")) {
        float v = strtof(rest, NULL);
        if (v < 1.0f || v > 10.0f) { printfln("ERR 1.0-10.0"); return; }
        fm30.vt_scale = v;
        printfln("OK");
    } else if (!strcmp(cmd, "RDS")) {
        if (!parse_bool(rest, &b)) { printfln("ERR ON|OFF"); return; }
        dsp.rds_enable = b; printfln("OK RDS %d", b);
    } else if (!strcmp(cmd, "RDSFREE")) {
        if (!parse_bool(rest, &b)) { printfln("ERR ON|OFF"); return; }
        dsp.free_run = b; printfln("OK RDSFREE %d", b);
    } else if (!strcmp(cmd, "RDSLEVEL")) {
        long v = strtol(rest, NULL, 0);
        if (v < 0 || v > 2047) { printfln("ERR 0-2047"); return; }
        dsp.level = (uint16_t)v; printfln("OK RDSLEVEL %ld", v);
    } else if (!strcmp(cmd, "RDSPHASE")) {
        long v = strtol(rest, NULL, 0);
        if (v < -180 || v > 180) { printfln("ERR -180..180"); return; }
        dsp.phase_deg = (int16_t)v; printfln("OK RDSPHASE %ld", v);
    } else if (!strcmp(cmd, "PI")) {
        char *end;
        unsigned long v = strtoul(rest, &end, 16);
        if (end == rest || v > 0xFFFF) { printfln("ERR hex 0000-FFFF"); return; }
        rds.pi = (uint16_t)v; printfln("OK PI %04lX", v);
    } else if (!strcmp(cmd, "PTY")) {
        long v = strtol(rest, NULL, 0);
        if (v < 0 || v > 31) { printfln("ERR 0-31"); return; }
        rds.pty = (uint8_t)v; printfln("OK PTY %ld", v);
    } else if (!strcmp(cmd, "TP") || !strcmp(cmd, "TA") || !strcmp(cmd, "MS") || !strcmp(cmd, "STEREO")) {
        if (!parse_bool(rest, &b)) { printfln("ERR 0|1"); return; }
        if (cmd[0] == 'T' && cmd[1] == 'P') rds.tp = b;
        else if (cmd[0] == 'T') rds.ta = b;
        else if (cmd[0] == 'M') rds.ms = b;
        else rds.stereo = b;
        printfln("OK %s %d", cmd, b);
    } else if (!strcmp(cmd, "PS")) {
        rds_set_ps(rest); printfln("OK PS \"%s\"", rds.ps);
    } else if (!strcmp(cmd, "RT")) {
        rds_set_rt(rest); printfln("OK RT");
    } else if (!strcmp(cmd, "STATUS")) {
        if (*rest) status_period_ms = (uint32_t)strtoul(rest, NULL, 0) * 1000u;
        cli_status_json();
    } else if (!strcmp(cmd, "LCD")) {
        char *val = strchr(rest, ' ');
        if (val) { *val++ = 0; while (*val == ' ') val++; } else val = "";
        if (!strcasecmp(rest, "PAGE")) {
            char *end;
            long v = strtol(val, &end, 0);
            if (end == val || v < 0 || v > UI_PAGES) { printfln("ERR LCD PAGE 0-%d", UI_PAGES); return; }
            ui_set_page((uint8_t)v); printfln("OK LCD PAGE %ld", v);
        } else if (!strcasecmp(rest, "MSG")) {
            char *text;
            long slot = strtol(val, &text, 0);
            if (text == val || slot < 1 || slot > UI_MSG_SLOTS) { printfln("ERR LCD MSG 1-%d [top]|<bottom>", UI_MSG_SLOTS); return; }
            while (*text == ' ') text++;
            char *bar = strchr(text, '|');
            const char *top = "", *bot = text;          /* no '|': whole text on the bottom row */
            if (bar) { *bar = 0; top = text; bot = bar + 1; }
            ui_set_msg((uint8_t)slot, top, bot);
            printfln(*top || *bot ? "OK LCD MSG %ld" : "OK LCD MSG %ld cleared", slot);
        } else if (!strcasecmp(rest, "ROTATE")) {
            unsigned mask = 0;
            if (!strcasecmp(val, "ALL")) mask = UI_ROT_ALL;
            else for (const char *p = val; *p; p++) {
                if (*p >= '1' && *p <= '0' + UI_PAGES) mask |= 1u << (*p - '1');
                else if (*p != ' ' && *p != ',') { mask = 0; break; }
            }
            if (!mask) { printfln("ERR LCD ROTATE 1-%d ... | ALL", UI_PAGES); return; }
            ui_set_rotation((uint8_t)mask);
            char list[2 * UI_PAGES + 1], *q = list;
            for (int p = 1; p <= UI_PAGES; p++) if (mask & (1u << (p - 1))) { *q++ = (char)('0' + p); *q++ = ' '; }
            q[-1] = 0;
            printfln("OK LCD ROTATE %s", list);
        } else if (parse_bool(rest, &b)) {
            lcd_enable(b); printfln("OK LCD %d", b);
        } else {
            printfln("ERR LCD ON|OFF|PAGE n|MSG n text|ROTATE list");
        }
    } else if (!strcmp(cmd, "SAVE")) {
        printfln(settings_save() ? "OK SAVED" : "ERR flash");
    } else if (!strcmp(cmd, "DEFAULTS")) {
        settings_defaults();
        fm30_set_vol_bal(fm30.volume, fm30.balance);
        fm30_set_freq(fm30.f10);
        printfln("OK DEFAULTS (not saved)");
    } else if (!strcmp(cmd, "HELP") || !strcmp(cmd, "?")) {
        help();
    } else {
        printfln("ERR unknown command (HELP)");
    }
}

void cli_task(void)
{
    static char line[128];
    static uint8_t len;
    while (rx_tail != rx_head) {
        char c = rx_ring[rx_tail];
        rx_tail++;
        if (c == '\r' || c == '\n') {
            line[len] = 0;
            if (len) exec(line);
            len = 0;
        } else if (len < sizeof line - 1 && (unsigned char)c >= 0x20) {
            line[len++] = c;
        }
    }
    if (status_period_ms && HAL_GetTick() - t_status >= status_period_ms) {
        t_status = HAL_GetTick();
        cli_status_json();
    }
}
