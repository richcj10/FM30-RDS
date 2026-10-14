/*
 * FM30 control: bit-banged bus to U3 (BH1415F), U6/U7 (MCP4921), U2 (MCP42010),
 * Vtune lock detection and the power-up / retune sequence reverse-engineered
 * from the stock MC68HC908JL3 firmware.
 */
#include "config.h"
#include "fm30.h"
#include <math.h>

fm30_t fm30;
static ADC_HandleTypeDef hadc2;

/* ------------------------------------------------------------------ */
/* Low level                                                           */
/* ------------------------------------------------------------------ */
static inline void pin(GPIO_TypeDef *p, uint16_t n, int hi)
{
    p->BSRR = hi ? n : ((uint32_t)n << 16);   /* open-drain: 1 = release (pull-up) */
}

void delay_us(uint32_t us)
{
    uint32_t start = DWT->CYCCNT;
    uint32_t ticks = us * (SystemCoreClock / 1000000U);
    while ((DWT->CYCCNT - start) < ticks) { }
}

static void clk_pulse(void)
{
    delay_us(BUS_HALF_BIT_US);
    pin(BUS_CLK_PORT, BUS_CLK_PIN, 1);
    delay_us(BUS_HALF_BIT_US);
    pin(BUS_CLK_PORT, BUS_CLK_PIN, 0);
}

static void shift_msb16(uint16_t w)
{
    for (int i = 15; i >= 0; i--) {
        pin(BUS_DATA_PORT, BUS_DATA_PIN, (w >> i) & 1);
        clk_pulse();
    }
    pin(BUS_DATA_PORT, BUS_DATA_PIN, 0);
}

static void shift_lsb16(uint16_t w)
{
    for (int i = 0; i < 16; i++) {
        pin(BUS_DATA_PORT, BUS_DATA_PIN, (w >> i) & 1);
        clk_pulse();
    }
    pin(BUS_DATA_PORT, BUS_DATA_PIN, 0);
}

static void ldac_pulse(void)
{
    pin(LDAC_PORT, LDAC_PIN, 0);
    delay_us(2);
    pin(LDAC_PORT, LDAC_PIN, 1);
}

/* BH1415F: 16 bits LSB first, CE active high */
static void bh1415_write(uint16_t w)
{
    pin(U3_CE_PORT, U3_CE_PIN, 1);
    delay_us(BUS_HALF_BIT_US);
    shift_lsb16(w);
    delay_us(BUS_HALF_BIT_US);
    pin(U3_CE_PORT, U3_CE_PIN, 0);
    delay_us(20);
}

/* MCP4921: 0x7000 | code (DAC A, buffered, 1x, active), MSB first, CS low, then LDAC */
static void mcp4921_write(GPIO_TypeDef *cs_port, uint16_t cs_pin, uint16_t code)
{
    if (code > 4095) code = 4095;
    pin(cs_port, cs_pin, 0);
    delay_us(BUS_HALF_BIT_US);
    shift_msb16(0x7000u | code);
    delay_us(BUS_HALF_BIT_US);
    pin(cs_port, cs_pin, 1);
    delay_us(2);
    ldac_pulse();
}

/* MCP42010: cmd 0x11 = pot0 (LEFT), 0x12 = pot1 (RIGHT) */
static void mcp42010_write(uint8_t cmd, uint8_t val)
{
    pin(U2_CS_PORT, U2_CS_PIN, 0);
    delay_us(BUS_HALF_BIT_US);
    shift_msb16(((uint16_t)cmd << 8) | val);
    delay_us(BUS_HALF_BIT_US);
    pin(U2_CS_PORT, U2_CS_PIN, 1);
}

static void u7_write(uint16_t code) { fm30.power_now = code; mcp4921_write(U7_CS_PORT, U7_CS_PIN, code); }
static void u6_write(uint16_t code) { fm30.u6 = code;        mcp4921_write(U6_CS_PORT, U6_CS_PIN, code); }

static void lock_led(int on) { pin(LOCK_LED_PORT, LOCK_LED_PIN, on); }

/* ------------------------------------------------------------------ */
/* Calibration data (captured from the stock firmware / measured)      */
/* ------------------------------------------------------------------ */
static const uint16_t u6_f10[]  = { 881,  931,  981, 1031, 1071 };
static const uint16_t u6_cal[]  = { 948, 1194, 1412, 1676, 1914 };
#define U6_N (sizeof(u6_f10) / sizeof(u6_f10[0]))

static const uint16_t vt_f10[]  = { 881,  981, 1074 };
static const float    vt_volt[] = { 1.72f, 3.80f, 6.46f };
#define VT_N (sizeof(vt_f10) / sizeof(vt_f10[0]))

static float interp(uint16_t x, const uint16_t *xs, const float *ys, int n)
{
    if (x <= xs[0])     return ys[0] + (ys[1] - ys[0]) * ((float)x - xs[0]) / (xs[1] - xs[0]);
    if (x >= xs[n - 1]) return ys[n-2] + (ys[n-1] - ys[n-2]) * ((float)x - xs[n-2]) / (xs[n-1] - xs[n-2]);
    for (int i = 0; i < n - 1; i++)
        if (x <= xs[i + 1])
            return ys[i] + (ys[i + 1] - ys[i]) * ((float)x - xs[i]) / (xs[i + 1] - xs[i]);
    return ys[n - 1];
}

uint16_t fm30_u6_code(uint16_t f10)
{
    float y[U6_N];
    for (unsigned i = 0; i < U6_N; i++) y[i] = u6_cal[i];
    float v = interp(f10, u6_f10, y, U6_N) + fm30.u6_trim;   /* extrapolates beyond 88.1/107.1 */
    if (v < 0) v = 0;
    if (v > 4095) v = 4095;
    return (uint16_t)(v + 0.5f);
}

float fm30_vtune_expected(uint16_t f10) { return interp(f10, vt_f10, vt_volt, VT_N); }

/* ------------------------------------------------------------------ */
/* Vtune ADC (ADC2, software triggered)                                */
/* ------------------------------------------------------------------ */
static void vtune_adc_init(void)
{
    __HAL_RCC_ADC12_CLK_ENABLE();
    hadc2.Instance = ADC2;
    hadc2.Init.ClockPrescaler        = ADC_CLOCK_SYNC_PCLK_DIV4;   /* must match ADC1 */
    hadc2.Init.Resolution            = ADC_RESOLUTION_12B;
    hadc2.Init.DataAlign             = ADC_DATAALIGN_RIGHT;
    hadc2.Init.GainCompensation      = 0;
    hadc2.Init.ScanConvMode          = ADC_SCAN_DISABLE;
    hadc2.Init.EOCSelection          = ADC_EOC_SINGLE_CONV;
    hadc2.Init.LowPowerAutoWait      = DISABLE;
    hadc2.Init.ContinuousConvMode    = DISABLE;
    hadc2.Init.NbrOfConversion       = 1;
    hadc2.Init.DiscontinuousConvMode = DISABLE;
    hadc2.Init.ExternalTrigConv      = ADC_SOFTWARE_START;
    hadc2.Init.ExternalTrigConvEdge  = ADC_EXTERNALTRIGCONVEDGE_NONE;
    hadc2.Init.DMAContinuousRequests = DISABLE;
    hadc2.Init.Overrun               = ADC_OVR_DATA_OVERWRITTEN;
    hadc2.Init.OversamplingMode      = DISABLE;
    if (HAL_ADC_Init(&hadc2) != HAL_OK) Error_Handler();

    ADC_ChannelConfTypeDef ch = {0};
    ch.Channel      = VTUNE_ADC_CH;
    ch.Rank         = ADC_REGULAR_RANK_1;
    ch.SamplingTime = ADC_SAMPLETIME_640CYCLES_5;   /* ~16k source impedance */
    ch.SingleDiff   = ADC_SINGLE_ENDED;
    ch.OffsetNumber = ADC_OFFSET_NONE;
    if (HAL_ADC_ConfigChannel(&hadc2, &ch) != HAL_OK) Error_Handler();
    HAL_ADCEx_Calibration_Start(&hadc2, ADC_SINGLE_ENDED);
}

float fm30_read_vtune(void)
{
    uint32_t acc = 0;
    for (int i = 0; i < 16; i++) {
        HAL_ADC_Start(&hadc2);
        HAL_ADC_PollForConversion(&hadc2, 2);
        acc += HAL_ADC_GetValue(&hadc2);
    }
    float v = (acc / 16.0f) * (VDDA_VOLTS / 4095.0f) * fm30.vt_scale;
    fm30.vtune = v;
    return v;
}

/* ------------------------------------------------------------------ */
/* State machine                                                       */
/* ------------------------------------------------------------------ */
static uint32_t t_state, t_step, stable_ms;
static float    vt_prev;

static void enter(fm30_state_t s)
{
    fm30.state = s;
    t_state = t_step = HAL_GetTick();
}

static void start_tune(void)
{
    u7_write(0);                    /* RF off before touching the PLL */
    lock_led(0);
    uint16_t w = 0x4800u | (fm30.f10 & 0x7FFu);
    bh1415_write(w);                /* same 3-word pattern as stock firmware */
    bh1415_write(w & ~0x0800u);
    bh1415_write(w);
    stable_ms = 0;
    vt_prev = -100.0f;
    fm30.fault_msg = "";
    enter(FM30_TUNING);
}

static void fault(const char *msg)
{
    u7_write(0);
    lock_led(0);
    fm30.fault_msg = msg;
    fm30.fault_count++;
    enter(FM30_FAULT);
}

static uint16_t u6_target(void)
{
    return (fm30.u6_manual >= 0) ? (uint16_t)fm30.u6_manual : fm30_u6_code(fm30.f10);
}

void fm30_task(void)
{
    uint32_t now = HAL_GetTick();

    switch (fm30.state) {
    case FM30_OFF:
        break;

    case FM30_TUNING:
        if (now - t_step < VT_SAMPLE_MS) break;
        t_step = now;
        {
            float v = fm30_read_vtune();
            if (fabsf(v - vt_prev) < VT_STABLE_V) stable_ms += VT_SAMPLE_MS;
            else stable_ms = 0;
            vt_prev = v;

            if (stable_ms >= VT_STABLE_MS) {
                if (v < VT_RAIL_LO || v > VT_RAIL_HI) { fault("vtune railed"); break; }
                fm30.vtune_ref = v;
                u6_write(u6_target());
                enter(FM30_RAMP);
            } else if (now - t_state > VT_LOCK_TIMEOUT_MS) {
                fault("lock timeout");
            }
        }
        break;

    case FM30_RAMP:
        if (!fm30.tx_enable) { u7_write(0); enter(FM30_OFF); break; }
        if (now - t_step < POWER_STEP_MS) break;
        t_step = now;
        if (fm30.power_now < fm30.power_target) {
            uint16_t p = fm30.power_now + POWER_STEP;
            if (p > fm30.power_target) p = fm30.power_target;
            u7_write(p);
        }
        if (fm30.power_now >= fm30.power_target) {
            if (fm30.power_now > fm30.power_target) u7_write(fm30.power_target);
            lock_led(1);
            enter(FM30_ON_AIR);
        }
        break;

    case FM30_ON_AIR:
        if (now - t_step < 200) break;
        t_step = now;
        {
            float v = fm30_read_vtune();
            if (v < VT_RAIL_LO || v > VT_RAIL_HI) { fault("vtune railed"); break; }
            if (fabsf(v - fm30.vtune_ref) > VT_JUMP_V) { fault("lock lost"); break; }
            fm30.vtune_ref += (v - fm30.vtune_ref) * (1.0f / 64.0f);   /* follow slow drift */
        }
        break;

    case FM30_FAULT:
        if (now - t_state > RETUNE_DELAY_MS && fm30.tx_enable) start_tune();
        break;
    }
}

/* ------------------------------------------------------------------ */
/* Public API                                                          */
/* ------------------------------------------------------------------ */
void fm30_set_freq(uint16_t f10)
{
    if (f10 < FREQ_MIN_X10) f10 = FREQ_MIN_X10;
    if (f10 > FREQ_MAX_X10) f10 = FREQ_MAX_X10;
    fm30.f10 = f10;
    /* Always program the PLL and U6; with TX off the RAMP state drops
     * straight to OFF, so RF stays at 0. */
    start_tune();
}

void fm30_set_power(uint16_t code)
{
    if (code > POWER_MAX) code = POWER_MAX;
    fm30.power_target = code;
    if (fm30.state == FM30_ON_AIR || fm30.state == FM30_RAMP) {
        if (code < fm30.power_now) u7_write(code);          /* decreases are immediate */
        else if (code > fm30.power_now) enter(FM30_RAMP);   /* increases soft-start   */
    }
}

void fm30_set_tx(bool on)
{
    fm30.tx_enable = on;
    if (!on) {
        u7_write(0);
        lock_led(0);
        if (fm30.state == FM30_RAMP || fm30.state == FM30_ON_AIR || fm30.state == FM30_FAULT)
            enter(FM30_OFF);
    } else if (fm30.state == FM30_OFF) {
        start_tune();
    }
}

void fm30_set_vol_bal(uint8_t vol, int8_t bal)
{
    if (bal < -127) bal = -127;
    fm30.volume = vol;
    fm30.balance = bal;
    int left = vol, right = vol;
    if (bal > 0) left  = vol - (vol * bal) / 128;   /* balance right cuts left  */
    if (bal < 0) right = vol - (vol * -bal) / 128;  /* balance left cuts right  */
    if (left < 1) left = 1;                          /* stock floors pot 0 at 1  */
    mcp42010_write(0x11, (uint8_t)left);
    mcp42010_write(0x12, (uint8_t)right);
}

void fm30_set_u6_trim(int16_t trim)
{
    fm30.u6_trim = trim;
    if (fm30.state == FM30_ON_AIR || fm30.state == FM30_RAMP) u6_write(u6_target());
}

void fm30_set_u6_manual(int16_t code)
{
    fm30.u6_manual = code;
    if (fm30.state == FM30_ON_AIR || fm30.state == FM30_RAMP) u6_write(u6_target());
}

const char *fm30_state_name(fm30_state_t s)
{
    switch (s) {
    case FM30_OFF:    return "off";
    case FM30_TUNING: return "tuning";
    case FM30_RAMP:   return "ramp";
    case FM30_ON_AIR: return "on_air";
    case FM30_FAULT:  return "fault";
    }
    return "?";
}

/* ------------------------------------------------------------------ */
/* Init: called BEFORE anything else touches the bus                   */
/* ------------------------------------------------------------------ */
void fm30_init(void)
{
    __HAL_RCC_GPIOA_CLK_ENABLE();
    __HAL_RCC_GPIOB_CLK_ENABLE();

    /* idle levels first, then switch pins to outputs (no glitches) */
    pin(BUS_CLK_PORT, BUS_CLK_PIN, 0);
    pin(BUS_DATA_PORT, BUS_DATA_PIN, 0);
    pin(U3_CE_PORT, U3_CE_PIN, 0);
    pin(U6_CS_PORT, U6_CS_PIN, 1);
    pin(U7_CS_PORT, U7_CS_PIN, 1);
    pin(U2_CS_PORT, U2_CS_PIN, 1);
    pin(LDAC_PORT, LDAC_PIN, 1);
    pin(LOCK_LED_PORT, LOCK_LED_PIN, 0);

    GPIO_InitTypeDef g = {0};
    g.Mode  = GPIO_MODE_OUTPUT_OD;     /* external 4.7k pull-ups to +5 V */
    g.Pull  = GPIO_NOPULL;
    g.Speed = GPIO_SPEED_FREQ_LOW;
    /* Order matters: at power-up the pull-ups hold every line high, which makes
     * U3 CE (active high) active. Take CE low first, while CLK is still high
     * and idle, then CLK and DATA, then the chip selects (which simply stay high). */
    static GPIO_TypeDef *const od_port[] = { U3_CE_PORT, BUS_CLK_PORT, BUS_DATA_PORT,
                                             LDAC_PORT, U6_CS_PORT, U7_CS_PORT, U2_CS_PORT };
    static const uint16_t od_pin[] = { U3_CE_PIN, BUS_CLK_PIN, BUS_DATA_PIN,
                                       LDAC_PIN, U6_CS_PIN, U7_CS_PIN, U2_CS_PIN };
    for (unsigned i = 0; i < sizeof od_pin / sizeof od_pin[0]; i++) {
        g.Pin = od_pin[i];
        HAL_GPIO_Init(od_port[i], &g);
    }

    g.Mode = GPIO_MODE_OUTPUT_PP;
    g.Pin  = LOCK_LED_PIN;
    HAL_GPIO_Init(LOCK_LED_PORT, &g);

    /* Overmod comparator -> EXTI */
    g.Mode = GPIO_MODE_IT_FALLING;
    g.Pull = GPIO_NOPULL;
    g.Pin  = OVM_PIN;
    HAL_GPIO_Init(OVM_PORT, &g);
    HAL_NVIC_SetPriority(OVM_EXTI_IRQn, 3, 0);
    HAL_NVIC_EnableIRQ(OVM_EXTI_IRQn);

    /* Vtune analog pin */
    g.Mode = GPIO_MODE_ANALOG;
    g.Pin  = VTUNE_PIN;
    HAL_GPIO_Init(VTUNE_PORT, &g);
    vtune_adc_init();

    fm30.state = FM30_OFF;
    fm30.u6_manual = -1;
    if (fm30.vt_scale <= 0.1f) fm30.vt_scale = VTUNE_SCALE_DEF;

    u7_write(0);                       /* RF off first, always */
}
