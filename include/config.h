/*
 * FM30 headless controller + RDS encoder — NUCLEO-G431KB
 * All board/pin/tuning constants live here.
 */
#pragma once
#include "stm32g4xx_hal.h"
#include <stdint.h>
#include <stdbool.h>

#define FW_VERSION "fm30ctl 0.3"

/* ------------------------------------------------------------------ */
/* FM30 control bus -> U1 socket.  (Richard's IO map)                  */
/* Bit-banged (no SPI peripheral). All 7 bus lines are OPEN-DRAIN with */
/* 4.7k pull-ups to FM30 +5 V (U1-7) - one 8-pin bussed SIP network.   */
/* All bus pins below are 5 V tolerant (FT).                           */
/*                                                                     */
/* Nucleo  STM32  U1 pin  signal                                       */
/*  D1     PA9    1   <-  IRQ: overmod comparator U5:A (falling edge)  */
/*  D0     PA10   2   ->  LDAC (U6 + U7)                               */
/*  D2     PA12   6   ->  U2 CS (MCP42010 vol/bal)  (remove HW1 jumper)*/
/*  D4     PB7    8   ->  U3 CE (BH1415F, active high)  (SB2 open)     */
/*  D6     PB6    9   ->  U7 CS (MCP4921 RF power)                     */
/*  D11    PB5    13  ->  BUS DATA                                     */
/*  D12    PB4    14  ->  BUS CLK                                      */
/*  A6     PA7    25  ->  LOCK LED (push-pull 3.3 V)                   */
/*  D10    PA11   27  ->  U6 CS (MCP4921 deviation comp)               */
/* ------------------------------------------------------------------ */
#define BUS_CLK_PORT    GPIOB
#define BUS_CLK_PIN     GPIO_PIN_4
#define BUS_DATA_PORT   GPIOB
#define BUS_DATA_PIN    GPIO_PIN_5
#define LDAC_PORT       GPIOA
#define LDAC_PIN        GPIO_PIN_10
#define U6_CS_PORT      GPIOA
#define U6_CS_PIN       GPIO_PIN_11
#define U7_CS_PORT      GPIOB
#define U7_CS_PIN       GPIO_PIN_6
#define U2_CS_PORT      GPIOA
#define U2_CS_PIN       GPIO_PIN_12
#define U3_CE_PORT      GPIOB
#define U3_CE_PIN       GPIO_PIN_7

#define LOCK_LED_PORT   GPIOA
#define LOCK_LED_PIN    GPIO_PIN_7
#define OVM_PORT        GPIOA
#define OVM_PIN         GPIO_PIN_9
#define OVM_EXTI_IRQn   EXTI9_5_IRQn

/* Vtune sense: PA1 (A1) -> U1-20 node, with an extra 47k from PA1 to GND.
 * C38 --47k--+--47k--GND (on FM30)  +  47k to GND (added)  => Vadc = Vtune/3 */
#define VTUNE_PORT      GPIOA
#define VTUNE_PIN       GPIO_PIN_1
#define VTUNE_ADC_CH    ADC_CHANNEL_2       /* PA1 = ADC12_IN2 */
#define VTUNE_SCALE_DEF 3.0f                /* Vtune = Vadc * scale (trim with VTSCALE) */
#define VDDA_VOLTS      3.30f

/* RDS front end (see fm30_rds_frontend schematic) */
#define PILOT_ADC_CH    ADC_CHANNEL_1       /* PA0 (A0) <- composite tap, gain 3, Vmid bias */
#define RDS_DAC_CH      DAC_CHANNEL_1       /* PA4 (A3) -> reconstruction filter -> 220k -> R10/R5 */

/* Nucleo user LED (LD2) */
#define BOARD_LED_PORT  GPIOB
#define BOARD_LED_PIN   GPIO_PIN_8

/* ------------------------------------------------------------------ */
/* Front-panel LCD: HD44780 2x8, 5 V module, 4-bit, write-only (R/W    */
/* tied to GND on the FM30 -> no busy flag, fixed delays).             */
/* PUSH-PULL 3.3 V, no pull-ups (HD44780 V_IH ~2.2 V at 5 V).          */
/*                                                                     */
/* Nucleo  STM32  U1 pin  signal (LCD pin)                             */
/*  D13    PB3    15  ->  D4 (11)                                      */
/*  D3     PB0    12  ->  D5 (12)                                      */
/*  D5     PA15   11  ->  D6 (13)                                      */
/*  D9     PA8    10  ->  D7 (14)                                      */
/*  A4     PA5    18  ->  RS (4)   SB2 must be OPEN                    */
/*  A5     PA6    17  ->  E  (6)   SB3 must be OPEN; remove the 10k    */
/*                                 pull-down on U1-17                  */
/* ------------------------------------------------------------------ */
#define LCD_D4_PORT     GPIOB
#define LCD_D4_PIN      GPIO_PIN_3
#define LCD_D5_PORT     GPIOB
#define LCD_D5_PIN      GPIO_PIN_0
#define LCD_D6_PORT     GPIOA
#define LCD_D6_PIN      GPIO_PIN_15
#define LCD_D7_PORT     GPIOA
#define LCD_D7_PIN      GPIO_PIN_8
#define LCD_RS_PORT     GPIOA
#define LCD_RS_PIN      GPIO_PIN_5
#define LCD_E_PORT      GPIOA
#define LCD_E_PIN       GPIO_PIN_6

#define LCD_POWERUP_MS  50      /* >= 40 ms after LCD Vcc before init      */
#define LCD_OP_GAP_US   50      /* >= 40 us execution time per cmd/char    */
#define LCD_CLEAR_US    2000    /* >= 1.6 ms for Clear/Home                */

/* Front-panel UI */
#define UI_PERIOD_MS    200     /* redraw rate (~5 Hz)                     */
#define UI_PAGE_MS      3000    /* min time per page in the rotation       */
#define UI_BOOT_MS      1500    /* boot screen                             */
#define UI_OVM_HOLD_MS  1000    /* '*' shown this long after an overmod    */
#define UI_MSG_LEN      48      /* max bottom-row length of a message page */
#define UI_SCROLL_HOLD  5       /* redraws (x200 ms) paused at scroll start */
#define UI_SCROLL_GAP   3       /* blanks between repeats of scrolled text */

/* USB virtual COM port: ST-Link VCP = USART2 on PA2/PA3 (A7/A2 on the header - leave them unused). */
#define VCP_USE_LPUART1 0
#define VCP_BAUD        115200

/* ------------------------------------------------------------------ */
/* Bus timing                                                          */
/* ------------------------------------------------------------------ */
#define BUS_HALF_BIT_US 4       /* ~125 kHz, stock firmware ran ~115 kHz */

/* ------------------------------------------------------------------ */
/* Radio defaults / limits (from the logic-analyzer captures)          */
/* ------------------------------------------------------------------ */
#define FREQ_MIN_X10    875
#define FREQ_MAX_X10    1080
#define FREQ_DEF_X10    881
#define POWER_DEF       0xE1F   /* stock stored power setting */
#define POWER_MAX       0xF80   /* highest code seen from stock firmware */
#define POWER_STEP      8       /* soft-start step (stock: +8)          */
#define POWER_STEP_MS   10      /* ms between steps (stock: ~11.6 ms)   */
#define VOLUME_DEF      128

/* Lock detection on Vtune */
#define VT_SAMPLE_MS    100     /* Vtune sample period while locking    */
#define VT_STABLE_V     0.04f   /* max change per sample to count as stable */
#define VT_STABLE_MS    1000    /* must be stable this long              */
#define VT_RAIL_LO      0.40f   /* below => loop railed / unlocked       */
#define VT_RAIL_HI      9.00f   /* above => loop railed / unlocked       */
#define VT_LOCK_TIMEOUT_MS 8000
#define VT_JUMP_V       0.50f   /* on-air: step away from tracked ref => unlock */
#define RETUNE_DELAY_MS 2000

/* ------------------------------------------------------------------ */
/* RDS / DSP                                                           */
/* ------------------------------------------------------------------ */
#define DSP_TIM_ARR     745     /* 170 MHz / 746 = 227.882 kS/s           */
#define DSP_BLOCK       228     /* samples per half-buffer (~1 ms)        */
#define RDS_LEVEL_DEF   400     /* DAC counts peak (trim for ~3-5% injection) */
#define RDS_PHASE_DEF   0       /* degrees, 57k phase trim                 */
#define PILOT_MIN_AMPL  15.0f   /* ADC counts; below => "no pilot"         */

extern uint32_t g_sysclk_from_hse;
extern uint32_t g_loop_max_us;          /* worst-case ui_task()+lcd_task() time, DSP ISR excluded */
void Error_Handler(void);
