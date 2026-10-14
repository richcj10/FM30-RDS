/*
 * FM30 headless controller + RDS encoder
 * NUCLEO-G431KB in the U1 socket of a Ramsey FM30.
 */
#include "config.h"
#include "fm30.h"
#include "dsp.h"
#include "rds.h"
#include "cli.h"
#include "settings.h"
#include "lcd.h"
#include "ui.h"

uint32_t g_sysclk_from_hse;
uint32_t g_loop_max_us;

static void clock_init(void)
{
    RCC_OscInitTypeDef o = {0};
    RCC_ClkInitTypeDef c = {0};

    HAL_PWREx_ControlVoltageScaling(PWR_REGULATOR_VOLTAGE_SCALE1_BOOST);

    /* 24 MHz crystal (X2, needs SB9/SB10 closed): /6 x85 /2 = 170 MHz */
    o.OscillatorType = RCC_OSCILLATORTYPE_HSE;
    o.HSEState = RCC_HSE_ON;
    o.PLL.PLLState = RCC_PLL_ON;
    o.PLL.PLLSource = RCC_PLLSOURCE_HSE;
    o.PLL.PLLM = RCC_PLLM_DIV6;
    o.PLL.PLLN = 85;
    o.PLL.PLLP = RCC_PLLP_DIV2;
    o.PLL.PLLQ = RCC_PLLQ_DIV2;
    o.PLL.PLLR = RCC_PLLR_DIV2;
    g_sysclk_from_hse = (HAL_RCC_OscConfig(&o) == HAL_OK);

    if (!g_sysclk_from_hse) {
        /* fallback: HSI16 /4 x85 /2 = 170 MHz (1% clock — pilot PLL still pulls in) */
        o.OscillatorType = RCC_OSCILLATORTYPE_HSI;
        o.HSEState = RCC_HSE_OFF;
        o.HSIState = RCC_HSI_ON;
        o.HSICalibrationValue = RCC_HSICALIBRATION_DEFAULT;
        o.PLL.PLLSource = RCC_PLLSOURCE_HSI;
        o.PLL.PLLM = RCC_PLLM_DIV4;
        if (HAL_RCC_OscConfig(&o) != HAL_OK) Error_Handler();
    }

    c.ClockType = RCC_CLOCKTYPE_HCLK | RCC_CLOCKTYPE_SYSCLK | RCC_CLOCKTYPE_PCLK1 | RCC_CLOCKTYPE_PCLK2;
    c.SYSCLKSource = RCC_SYSCLKSOURCE_PLLCLK;
    c.AHBCLKDivider = RCC_SYSCLK_DIV1;
    c.APB1CLKDivider = RCC_HCLK_DIV1;
    c.APB2CLKDivider = RCC_HCLK_DIV1;
    if (HAL_RCC_ClockConfig(&c, FLASH_LATENCY_4) != HAL_OK) Error_Handler();
}

static void board_led_init(void)
{
    __HAL_RCC_GPIOB_CLK_ENABLE();
    GPIO_InitTypeDef g = {0};
    g.Pin = BOARD_LED_PIN;
    g.Mode = GPIO_MODE_OUTPUT_PP;
    g.Speed = GPIO_SPEED_FREQ_LOW;
    HAL_GPIO_Init(BOARD_LED_PORT, &g);
}

int main(void)
{
    HAL_Init();

    /* G4: UCPD dead-battery Rd (~5.1k to GND) is on after reset on PB4/PB6
     * (UCPD1_CC2/CC1) = bus CLK and U7 CS, fighting the 4.7k pull-ups. */
    __HAL_RCC_PWR_CLK_ENABLE();
    HAL_PWREx_DisableUCPDDeadBattery();

    /* cycle counter for the bus bit timing */
    CoreDebug->DEMCR |= CoreDebug_DEMCR_TRCENA_Msk;
    DWT->CYCCNT = 0;
    DWT->CTRL |= DWT_CTRL_CYCCNTENA_Msk;

    clock_init();
    board_led_init();

    bool loaded = settings_load();      /* fills fm30/rds/dsp config */
    fm30_init();                         /* bus idle, U7 = 0, Vtune ADC */
    cli_init();
    lcd_init();                          /* blocking ~60 ms (HD44780 power-up), before the DSP starts */
    ui_init();
    rds_init();
    dsp_init();                          /* starts 228 kS/s ADC/DAC */

    cli_print("\r\n" FW_VERSION " ready\r\n");
    cli_print(loaded ? "settings: flash\r\n" : "settings: defaults\r\n");
    cli_print(g_sysclk_from_hse ? "clock: HSE 24 MHz\r\n" : "clock: HSI (no crystal?)\r\n");

    fm30_set_vol_bal(fm30.volume, fm30.balance);
    fm30_set_freq(fm30.f10);             /* full tune -> lock -> U6 -> ramp */

    uint32_t t_led = 0;
    for (;;) {
        cli_task();
        fm30_task();

        /* LCD cost per pass; DSP ISR time that preempts it is subtracted */
        uint32_t c0 = DWT->CYCCNT, i0 = dsp.isr_cycles;
        ui_task();
        lcd_task();
        uint32_t us = ((DWT->CYCCNT - c0) - (dsp.isr_cycles - i0)) / (SystemCoreClock / 1000000U);
        if (us > g_loop_max_us) g_loop_max_us = us;

        /* LD2: solid = on air + pilot locked, slow blink = on air no pilot,
         * fast blink = tuning/fault, off = TX off */
        uint32_t now = HAL_GetTick();
        if (now - t_led >= 100) {
            t_led = now;
            static uint8_t ph;
            ph++;
            int on;
            switch (fm30.state) {
            case FM30_ON_AIR: on = dsp.pilot_lock ? 1 : ((ph / 5) & 1); break;
            case FM30_OFF:    on = 0; break;
            default:          on = ph & 1; break;
            }
            HAL_GPIO_WritePin(BOARD_LED_PORT, BOARD_LED_PIN, on ? GPIO_PIN_SET : GPIO_PIN_RESET);
        }
    }
}

void HAL_GPIO_EXTI_Callback(uint16_t pin)
{
    if (pin == OVM_PIN) g_ovm_count++;
}

void Error_Handler(void)
{
    __disable_irq();
    for (;;) {                       /* fast-blink LD2 forever */
        BOARD_LED_PORT->ODR ^= BOARD_LED_PIN;
        for (volatile uint32_t i = 0; i < 2000000; i++) { }
    }
}
