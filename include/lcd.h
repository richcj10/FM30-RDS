#pragma once
#include <stdint.h>
#include <stdbool.h>

#define LCD_COLS 8
#define LCD_ROWS 2

void lcd_init(void);                                        /* blocking ~60 ms, once at boot */
void lcd_task(void);                                        /* main loop: <= 1 op per call, ~6 us */
void lcd_print(uint8_t line, uint8_t col, const char *s);   /* into the shadow buffer, clipped */
void lcd_enable(bool on);                                   /* display on/off, contents kept */
bool lcd_is_enabled(void);
