#pragma once
#include <stdint.h>

#define UI_BUILTIN_PAGES 4                  /* 1 freq/state, 2 PS, 3 power/Vtune, 4 RDS */
#define UI_MSG_SLOTS     4                  /* pages 5..8 = LCD MSG 1..4 */
#define UI_PAGES         (UI_BUILTIN_PAGES + UI_MSG_SLOTS)
#define UI_ROT_ALL       0xFFu              /* rotation mask, bit n-1 = page n */

void    ui_init(void);                      /* starts the boot screen */
void    ui_task(void);                      /* main loop; fills the LCD shadow buffer at ~5 Hz */
void    ui_set_page(uint8_t page);          /* 0 = rotate, 1..UI_PAGES = pinned */
uint8_t ui_get_page(void);
void    ui_set_rotation(uint8_t mask);      /* pages in the rotation; 0 = all */
uint8_t ui_get_rotation(void);
/* message page (slot 1..UI_MSG_SLOTS): top clipped to 8 chars, bottom up to
 * UI_MSG_LEN chars and scrolls if longer than 8. Both empty = slot cleared. */
void    ui_set_msg(uint8_t slot, const char *top, const char *bottom);
