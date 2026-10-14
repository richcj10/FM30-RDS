/*
 * FM30 front-panel LCD: HD44780 2x8, 4-bit, write-only (R/W tied to GND on
 * the FM30), so there is no busy flag and every operation is followed by a
 * fixed minimum gap.
 *
 * lcd_print() only writes a 16-char shadow buffer. lcd_task() compares it
 * with what the display holds and sends at most ONE operation (a DDRAM
 * address or one char, ~6 us of pin wiggling) per call, then lets
 * LCD_OP_GAP_US pass before the next one without blocking. A full redraw
 * takes ~1 ms of main-loop passes. Never call from an ISR.
 */
#include "config.h"
#include "lcd.h"
#include "fm30.h"                 /* delay_us() */
#include <string.h>

#define LCD_CHARS (LCD_ROWS * LCD_COLS)

static char want[LCD_CHARS];      /* what the UI wants shown */
static char shown[LCD_CHARS];     /* what the display holds  */
static int  cursor = -1;          /* index the DDRAM address points at, -1 = unknown */
static bool enabled = true, enabled_shown;
static uint32_t t_next;           /* DWT time the display accepts the next op */

static inline void pin(GPIO_TypeDef *p, uint16_t n, int hi)
{
    p->BSRR = hi ? n : ((uint32_t)n << 16);
}

/* data pins span two ports -> set bit by bit */
static void put_nibble(uint8_t v)
{
    pin(LCD_D4_PORT, LCD_D4_PIN, v & 1);
    pin(LCD_D5_PORT, LCD_D5_PIN, v & 2);
    pin(LCD_D6_PORT, LCD_D6_PIN, v & 4);
    pin(LCD_D7_PORT, LCD_D7_PIN, v & 8);
    delay_us(1);                  /* RS/data setup before E (t_AS >= 140 ns) */
    pin(LCD_E_PORT, LCD_E_PIN, 1);
    delay_us(1);                  /* PW_EH >= 450 ns */
    pin(LCD_E_PORT, LCD_E_PIN, 0);
    delay_us(1);                  /* t_cycE >= 1 us */
}

static void put_byte(bool rs, uint8_t b)
{
    pin(LCD_RS_PORT, LCD_RS_PIN, rs);
    put_nibble(b >> 4);
    put_nibble(b & 0x0F);
}

static void cmd_wait(uint8_t c, uint32_t us)    /* init only (blocking) */
{
    put_byte(false, c);
    delay_us(us);
}

void lcd_init(void)
{
    __HAL_RCC_GPIOA_CLK_ENABLE();
    __HAL_RCC_GPIOB_CLK_ENABLE();

    static GPIO_TypeDef *const port[] = { LCD_E_PORT, LCD_RS_PORT, LCD_D4_PORT,
                                          LCD_D5_PORT, LCD_D6_PORT, LCD_D7_PORT };
    static const uint16_t pins[] = { LCD_E_PIN, LCD_RS_PIN, LCD_D4_PIN,
                                     LCD_D5_PIN, LCD_D6_PIN, LCD_D7_PIN };
    GPIO_InitTypeDef g = {0};
    g.Mode  = GPIO_MODE_OUTPUT_PP;
    g.Pull  = GPIO_NOPULL;
    g.Speed = GPIO_SPEED_FREQ_LOW;
    for (unsigned i = 0; i < sizeof pins / sizeof pins[0]; i++) {
        pin(port[i], pins[i], 0);                  /* E low first, all idle low */
        g.Pin = pins[i];
        HAL_GPIO_Init(port[i], &g);
    }

    HAL_Delay(LCD_POWERUP_MS);
    put_nibble(0x3); delay_us(4100);               /* RS = 0 from above */
    put_nibble(0x3); delay_us(100);
    put_nibble(0x3); delay_us(100);
    put_nibble(0x2); delay_us(100);                /* -> 4-bit */
    cmd_wait(0x28, LCD_OP_GAP_US);                 /* 4-bit, 2 lines, 5x8 */
    cmd_wait(0x08, LCD_OP_GAP_US);                 /* display off */
    cmd_wait(0x01, LCD_CLEAR_US);                  /* clear, DDRAM addr 0 */
    cmd_wait(0x06, LCD_OP_GAP_US);                 /* increment, no shift */
    cmd_wait(enabled ? 0x0C : 0x08, LCD_OP_GAP_US); /* on, no cursor (or kept off) */

    memset(want, ' ', sizeof want);
    memset(shown, ' ', sizeof shown);
    cursor = 0;
    enabled_shown = enabled;
    t_next = DWT->CYCCNT;
}

void lcd_task(void)
{
    if ((int32_t)(DWT->CYCCNT - t_next) < 0) return;   /* last op still executing */

    if (enabled != enabled_shown) {
        put_byte(false, enabled ? 0x0C : 0x08);
        enabled_shown = enabled;
    } else {
        int start = cursor < 0 ? 0 : cursor, i = -1;
        for (int k = 0; k < LCD_CHARS; k++) {          /* prefer the char at the cursor */
            int j = (start + k) % LCD_CHARS;
            if (want[j] != shown[j]) { i = j; break; }
        }
        if (i < 0) return;
        if (i != cursor) {
            put_byte(false, (uint8_t)(0x80 | (i / LCD_COLS ? 0x40 : 0x00) | (i % LCD_COLS)));
            cursor = i;
        } else {
            put_byte(true, (uint8_t)want[i]);
            shown[i] = want[i];
            /* after col 7 the DDRAM address runs off the visible window */
            cursor = (i % LCD_COLS == LCD_COLS - 1) ? -1 : i + 1;
        }
    }
    t_next = DWT->CYCCNT + LCD_OP_GAP_US * (SystemCoreClock / 1000000U);
}

void lcd_print(uint8_t line, uint8_t col, const char *s)
{
    if (line >= LCD_ROWS) return;
    for (; col < LCD_COLS && *s; col++, s++) {
        char c = *s;
        want[line * LCD_COLS + col] = (c >= 0x20 && c < 0x7F) ? c : ' ';
    }
}

void lcd_enable(bool on) { enabled = on; }
bool lcd_is_enabled(void) { return enabled; }
