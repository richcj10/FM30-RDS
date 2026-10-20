/*
 * RDS modulator, pilot-locked.
 *
 *  TIM6 (170 MHz / 746 = 227.882 kS/s) triggers ADC1 (pilot/composite tap on PA0)
 *  and DAC1 CH1 (RDS out on PA4). Both run circular DMA over 2 x DSP_BLOCK samples.
 *  On each half-buffer interrupt:
 *    1. a software PLL locks a 19 kHz NCO to the pilot in the captured block,
 *    2. the RDS subcarrier is generated as exactly 3x the NCO phase (57 kHz),
 *    3. the bit clock is 57 kHz / 48 = 1187.5 Hz, counted off the subcarrier,
 *    4. biphase symbols are shaped with the IEC 62106 cos() spectrum.
 *  The output block is written one buffer ahead, so its phase is advanced by
 *  2*DSP_BLOCK samples; residual group delay is trimmed with RDSPHASE.
 */
#ifdef DSP_HOST_TEST
#include "host_stub.h"          /* tools/: lets the DSP core run on a PC */
#else
#include "config.h"
#endif
#include "dsp.h"
#include "rds.h"
#include <math.h>
#include <string.h>

dsp_t dsp;

#ifndef DSP_HOST_TEST
static ADC_HandleTypeDef hadc1;
static DAC_HandleTypeDef hdac1;
static TIM_HandleTypeDef htim6;
static DMA_HandleTypeDef hdma_adc1, hdma_dac1;

static uint16_t adc_buf[2 * DSP_BLOCK];
static uint16_t dac_buf[2 * DSP_BLOCK];
#define STATIC static
#else
#define STATIC
#endif

/* ---------------- tables ---------------- */
#define SIN_N      1024
static float sin_lut[SIN_N + SIN_N / 4];          /* + quarter for cos */
#define COS(i)     sin_lut[(i) + SIN_N / 4]

#define SH_RES     128                            /* LUT points per bit */
#define SH_T0      (-2.0f)                        /* LUT covers t in [-2, 2.5) bits */
#define SH_LEN     (9 * SH_RES / 2)               /* 4.5 bits */
static float shape[SH_LEN];

/* ---------------- state ---------------- */
static float    fs;
static uint32_t inc_nom, nco_inc, nco_ph;
static float    freq_acc;                         /* rad per block */
static float    If, Qf, err_f, dc = 2048.0f;
static uint32_t prev57;
static uint32_t cyc57;
static float    sym[5];                           /* bits n-2 .. n+2 */
static uint8_t  prev_diff;
static int      lock_cnt;
static volatile int busy;
static uint32_t test_ph, test_inc;                /* DACTEST 2: 1 kHz sine */

#define TWO_PI      6.283185307f
#define RAD2PH      (4294967296.0f / TWO_PI)

static float h_pulse(float t)          /* inverse FT of cos(pi f Td / 4), |f| < 2/Td */
{
    const float a = 3.14159265f / 4.0f;
    float b = TWO_PI * t;
    float c1 = a - b, c2 = a + b;
    float f1 = (fabsf(c1) < 1e-6f) ? 2.0f : sinf(2.0f * c1) / c1;
    float f2 = (fabsf(c2) < 1e-6f) ? 2.0f : sinf(2.0f * c2) / c2;
    return f1 + f2;
}

static inline float shape_at(float t)
{
    int i = (int)((t - SH_T0) * SH_RES);
    return (i >= 0 && i < SH_LEN) ? shape[i] : 0.0f;
}

static inline float envelope(float tau)
{
    return sym[0] * shape_at(tau + 2.0f) + sym[1] * shape_at(tau + 1.0f) +
           sym[2] * shape_at(tau)        + sym[3] * shape_at(tau - 1.0f) +
           sym[4] * shape_at(tau - 2.0f);
}

static void build_tables(void)
{
    for (int i = 0; i < SIN_N + SIN_N / 4; i++)
        sin_lut[i] = sinf(TWO_PI * (float)i / SIN_N);

    /* biphase symbol: +h(t) - h(t - 1/2), h truncated to |t| <= 2 bits */
    for (int i = 0; i < SH_LEN; i++) {
        float t = SH_T0 + (i + 0.5f) / SH_RES;
        float v = 0;
        if (fabsf(t) <= 2.0f)        v += h_pulse(t);
        if (fabsf(t - 0.5f) <= 2.0f) v -= h_pulse(t - 0.5f);
        shape[i] = v;
    }
    /* normalise so the worst-case envelope peak is 1.0 */
    float peak = 0;
    for (int pat = 0; pat < 32; pat++) {
        for (int k = 0; k < 5; k++) sym[k] = (pat >> k) & 1 ? 1.0f : -1.0f;
        for (int j = 0; j < SH_RES; j++) {
            float e = fabsf(envelope((float)j / SH_RES));
            if (e > peak) peak = e;
        }
    }
    for (int i = 0; i < SH_LEN; i++) shape[i] /= peak;
    for (int k = 0; k < 5; k++) sym[k] = 1.0f;
}

static void next_symbol(void)
{
    sym[0] = sym[1]; sym[1] = sym[2]; sym[2] = sym[3]; sym[3] = sym[4];
    uint8_t d = rds_next_bit() ^ prev_diff;        /* differential encoding */
    prev_diff = d;
    sym[4] = d ? 1.0f : -1.0f;
}

/* ---------------- block processing ---------------- */
STATIC void process(const uint16_t *in, uint16_t *out)
{
    if (busy) { dsp.overruns++; return; }
    busy = 1;
    uint32_t t0 = DWT->CYCCNT;

    const int n = DSP_BLOCK;
    uint32_t ph  = nco_ph, inc = nco_inc;
    uint32_t adv = inc * (uint32_t)(2 * DSP_BLOCK);
    uint32_t ofs = (uint32_t)(int32_t)((float)dsp.phase_deg * (4294967296.0f / 360.0f));
    bool on = dsp.rds_enable && (dsp.pilot_lock || dsp.free_run);
    float amp = on ? (float)dsp.level : 0.0f;
    bool tone = dsp.dac_test == 2;
    float I = 0, Q = 0, sum = 0, d0 = dc;

    for (int i = 0; i < n; i++) {
        float x = (float)in[i];
        sum += x;
        x -= d0;
        uint32_t k = ph >> 22;
        I += x * COS(k);
        Q += x * sin_lut[k];

        uint32_t p57 = (ph + adv) * 3u + ofs;
        if (p57 < prev57) {
            if (++cyc57 >= 48) { cyc57 = 0; next_symbol(); }
        }
        prev57 = p57;
        float tau = ((float)cyc57 + (float)p57 * 2.3283064e-10f) * (1.0f / 48.0f);
        int v = 2048 + (int)(amp * envelope(tau) * sin_lut[p57 >> 22]);
        if (tone) { v = 2048 + (int)(2047.0f * sin_lut[test_ph >> 22]); test_ph += test_inc; }
        if (v < 0) v = 0;
        if (v > 4095) v = 4095;
        out[i] = (uint16_t)v;
        ph += inc;
    }
    dc += 0.05f * (sum / n - dc);

    /* ---- pilot PLL (runs at block rate, ~1 kHz) ---- */
    float i_ = I * (2.0f / n), q_ = Q * (2.0f / n);
    If += 0.3f * (i_ - If);
    Qf += 0.3f * (q_ - Qf);
    float ampl = sqrtf(If * If + Qf * Qf);
    float err  = atan2f(-Qf, If);                  /* pilot phase - NCO phase */
    err_f += 0.05f * (err - err_f);

    bool have = ampl > PILOT_MIN_AMPL;
    float kp = dsp.pilot_lock ? 0.045f : 0.15f;
    float ki = dsp.pilot_lock ? 0.001f : 0.004f;
    if (have) {
        freq_acc += ki * err;
        const float lim = TWO_PI * 50.0f * n / fs;  /* +-50 Hz */
        if (freq_acc >  lim) freq_acc =  lim;
        if (freq_acc < -lim) freq_acc = -lim;
        ph += (uint32_t)(int32_t)(kp * err * RAD2PH);
    } else {
        freq_acc *= 0.995f;
    }
    nco_inc = inc_nom + (uint32_t)(int32_t)(freq_acc / n * RAD2PH);
    nco_ph  = ph;

    if (have && fabsf(err_f) < 0.15f) { if (lock_cnt < 300) lock_cnt++; }
    else if (!have || fabsf(err_f) > 0.5f) { if (lock_cnt > 0) lock_cnt -= 6; if (lock_cnt < 0) lock_cnt = 0; }
    dsp.pilot_lock = lock_cnt >= 300 ? true : (lock_cnt == 0 ? false : dsp.pilot_lock);

    dsp.pilot_ampl = ampl;
    dsp.pilot_err  = err_f;
    dsp.pilot_hz   = (float)nco_inc * fs / 4294967296.0f;
    dsp.blocks++;
    uint32_t cyc = DWT->CYCCNT - t0;
    dsp.load_pct = (uint16_t)(cyc * 100u / ((uint32_t)n * (DSP_TIM_ARR + 1)));
    dsp.isr_cycles += cyc;
    busy = 0;
}

#ifndef DSP_HOST_TEST
void HAL_ADC_ConvHalfCpltCallback(ADC_HandleTypeDef *h)
{
    if (h == &hadc1) process(&adc_buf[0], &dac_buf[0]);
}

void HAL_ADC_ConvCpltCallback(ADC_HandleTypeDef *h)
{
    if (h == &hadc1) process(&adc_buf[DSP_BLOCK], &dac_buf[DSP_BLOCK]);
}

void DMA1_Channel1_IRQHandler(void) { HAL_DMA_IRQHandler(&hdma_adc1); }
void DMA1_Channel2_IRQHandler(void) { HAL_DMA_IRQHandler(&hdma_dac1); }
#endif

/* ---------------- init ---------------- */
void dsp_core_init(float sample_rate)
{
    build_tables();
    fs = sample_rate;
    inc_nom = (uint32_t)(19000.0 / (double)fs * 4294967296.0 + 0.5);
    nco_inc = inc_nom;
    test_inc = (uint32_t)(1000.0 / (double)fs * 4294967296.0 + 0.5);
    if (dsp.level == 0) dsp.level = RDS_LEVEL_DEF;
}

#ifndef DSP_HOST_TEST
static HAL_StatusTypeDef dac_config(uint32_t trigger)
{
    DAC_ChannelConfTypeDef c = {0};
    c.DAC_HighFrequency = DAC_HIGH_FREQUENCY_INTERFACE_MODE_AUTOMATIC;   /* HCLK 170 MHz -> HFSEL >160 MHz */
    c.DAC_DMADoubleDataMode = DISABLE;
    c.DAC_SignedFormat = DISABLE;
    c.DAC_SampleAndHold = DAC_SAMPLEANDHOLD_DISABLE;
    c.DAC_Trigger = trigger;
    c.DAC_Trigger2 = DAC_TRIGGER_NONE;
    c.DAC_OutputBuffer = DAC_OUTPUTBUFFER_ENABLE;
    c.DAC_ConnectOnChipPeripheral = DAC_CHIPCONNECT_EXTERNAL;
    c.DAC_UserTrimming = DAC_TRIMMING_FACTORY;
    return HAL_DAC_ConfigChannel(&hdac1, &c, RDS_DAC_CH);
}

/* ADC and DAC DMA both start at sample 0, then TIM6 clocks them together:
 * process() relies on the DAC reading the half the ADC is not filling. */
static HAL_StatusTypeDef stream_start(void)
{
    for (int i = 0; i < 2 * DSP_BLOCK; i++) dac_buf[i] = 2048;
    DAC1->SR = DAC_SR_DMAUDR1;                     /* clear a latched underrun (rc_w1) */
    __HAL_TIM_SET_COUNTER(&htim6, 0);
    HAL_StatusTypeDef st = HAL_ADC_Start_DMA(&hadc1, (uint32_t *)adc_buf, 2 * DSP_BLOCK);
    if (st == HAL_OK) st = HAL_DAC_Start_DMA(&hdac1, RDS_DAC_CH, (uint32_t *)dac_buf, 2 * DSP_BLOCK, DAC_ALIGN_12B_R);
    if (st == HAL_OK) st = HAL_TIM_Base_Start(&htim6);
    return st;
}

static void stream_stop(void)
{
    HAL_TIM_Base_Stop(&htim6);
    HAL_ADC_Stop_DMA(&hadc1);
    HAL_DAC_Stop_DMA(&hdac1, RDS_DAC_CH);          /* also clears EN1 */
}

void dsp_init(void)
{
    dsp_core_init((float)SystemCoreClock / (DSP_TIM_ARR + 1));

    __HAL_RCC_GPIOA_CLK_ENABLE();
    __HAL_RCC_ADC12_CLK_ENABLE();
    __HAL_RCC_DAC1_CLK_ENABLE();
    __HAL_RCC_TIM6_CLK_ENABLE();
    __HAL_RCC_DMAMUX1_CLK_ENABLE();
    __HAL_RCC_DMA1_CLK_ENABLE();

    GPIO_InitTypeDef g = {0};
    g.Mode = GPIO_MODE_ANALOG;
    g.Pull = GPIO_NOPULL;
    g.Pin  = GPIO_PIN_0 | GPIO_PIN_4;   /* PA0 ADC in, PA4 DAC out */
    HAL_GPIO_Init(GPIOA, &g);

    /* TIM6: sample clock */
    htim6.Instance = TIM6;
    htim6.Init.Prescaler = 0;
    htim6.Init.CounterMode = TIM_COUNTERMODE_UP;
    htim6.Init.Period = DSP_TIM_ARR;
    htim6.Init.AutoReloadPreload = TIM_AUTORELOAD_PRELOAD_ENABLE;
    if (HAL_TIM_Base_Init(&htim6) != HAL_OK) Error_Handler();
    TIM_MasterConfigTypeDef m = {0};
    m.MasterOutputTrigger = TIM_TRGO_UPDATE;
    m.MasterSlaveMode = TIM_MASTERSLAVEMODE_DISABLE;
    if (HAL_TIMEx_MasterConfigSynchronization(&htim6, &m) != HAL_OK) Error_Handler();

    /* ADC1 DMA */
    hdma_adc1.Instance = DMA1_Channel1;
    hdma_adc1.Init.Request = DMA_REQUEST_ADC1;
    hdma_adc1.Init.Direction = DMA_PERIPH_TO_MEMORY;
    hdma_adc1.Init.PeriphInc = DMA_PINC_DISABLE;
    hdma_adc1.Init.MemInc = DMA_MINC_ENABLE;
    hdma_adc1.Init.PeriphDataAlignment = DMA_PDATAALIGN_HALFWORD;
    hdma_adc1.Init.MemDataAlignment = DMA_MDATAALIGN_HALFWORD;
    hdma_adc1.Init.Mode = DMA_CIRCULAR;
    hdma_adc1.Init.Priority = DMA_PRIORITY_HIGH;
    if (HAL_DMA_Init(&hdma_adc1) != HAL_OK) Error_Handler();
    __HAL_LINKDMA(&hadc1, DMA_Handle, hdma_adc1);

    /* ADC1: pilot/composite on PA0, triggered by TIM6 */
    hadc1.Instance = ADC1;
    hadc1.Init.ClockPrescaler = ADC_CLOCK_SYNC_PCLK_DIV4;
    hadc1.Init.Resolution = ADC_RESOLUTION_12B;
    hadc1.Init.DataAlign = ADC_DATAALIGN_RIGHT;
    hadc1.Init.GainCompensation = 0;
    hadc1.Init.ScanConvMode = ADC_SCAN_DISABLE;
    hadc1.Init.EOCSelection = ADC_EOC_SINGLE_CONV;
    hadc1.Init.LowPowerAutoWait = DISABLE;
    hadc1.Init.ContinuousConvMode = DISABLE;
    hadc1.Init.NbrOfConversion = 1;
    hadc1.Init.DiscontinuousConvMode = DISABLE;
    hadc1.Init.ExternalTrigConv = ADC_EXTERNALTRIG_T6_TRGO;
    hadc1.Init.ExternalTrigConvEdge = ADC_EXTERNALTRIGCONVEDGE_RISING;
    hadc1.Init.DMAContinuousRequests = ENABLE;
    hadc1.Init.Overrun = ADC_OVR_DATA_OVERWRITTEN;
    hadc1.Init.OversamplingMode = DISABLE;
    if (HAL_ADC_Init(&hadc1) != HAL_OK) Error_Handler();
    ADC_ChannelConfTypeDef ch = {0};
    ch.Channel = PILOT_ADC_CH;
    ch.Rank = ADC_REGULAR_RANK_1;
    ch.SamplingTime = ADC_SAMPLETIME_47CYCLES_5;
    ch.SingleDiff = ADC_SINGLE_ENDED;
    ch.OffsetNumber = ADC_OFFSET_NONE;
    if (HAL_ADC_ConfigChannel(&hadc1, &ch) != HAL_OK) Error_Handler();
    if (HAL_ADCEx_Calibration_Start(&hadc1, ADC_SINGLE_ENDED) != HAL_OK) Error_Handler();

    /* DAC1 DMA */
    hdma_dac1.Instance = DMA1_Channel2;
    hdma_dac1.Init.Request = DMA_REQUEST_DAC1_CHANNEL1;
    hdma_dac1.Init.Direction = DMA_MEMORY_TO_PERIPH;
    hdma_dac1.Init.PeriphInc = DMA_PINC_DISABLE;
    hdma_dac1.Init.MemInc = DMA_MINC_ENABLE;
    hdma_dac1.Init.PeriphDataAlignment = DMA_PDATAALIGN_WORD;   /* G4 DAC registers: 32-bit access only */
    hdma_dac1.Init.MemDataAlignment = DMA_MDATAALIGN_HALFWORD;
    hdma_dac1.Init.Mode = DMA_CIRCULAR;
    hdma_dac1.Init.Priority = DMA_PRIORITY_HIGH;
    if (HAL_DMA_Init(&hdma_dac1) != HAL_OK) Error_Handler();
    __HAL_LINKDMA(&hdac1, DMA_Handle1, hdma_dac1);

    /* DAC1 CH1 on PA4, triggered by TIM6 */
    hdac1.Instance = DAC1;
    if (HAL_DAC_Init(&hdac1) != HAL_OK) Error_Handler();
    if (dac_config(DAC_TRIGGER_T6_TRGO) != HAL_OK) Error_Handler();

    HAL_NVIC_SetPriority(DMA1_Channel1_IRQn, 1, 0);
    HAL_NVIC_EnableIRQ(DMA1_Channel1_IRQn);
    HAL_NVIC_SetPriority(DMA1_Channel2_IRQn, 2, 0);
    HAL_NVIC_EnableIRQ(DMA1_Channel2_IRQn);

    if (stream_start() != HAL_OK) Error_Handler();
}

/* ---------------- DACTEST (bench diagnostics) ---------------- */
uint32_t dsp_dac_error(void) { return hdac1.ErrorCode; }

int dsp_dac_test(int mode)
{
    if (mode < 0 || mode > 2) return -1;
    HAL_StatusTypeDef st;
    if (mode == 1) {
        /* TIM6/ADC keep running (pilot PLL stays up); only the DAC leaves the stream */
        dsp.dac_test = 1;
        HAL_DAC_Stop_DMA(&hdac1, RDS_DAC_CH);
        st = dac_config(DAC_TRIGGER_SOFTWARE);
        if (st == HAL_OK) st = HAL_DAC_SetValue(&hdac1, RDS_DAC_CH, DAC_ALIGN_12B_R, 2048);
        if (st == HAL_OK) st = HAL_DAC_Start(&hdac1, RDS_DAC_CH);   /* sets SWTRIG1 */
        return st;
    }
    /* 0 and 2: full restart of the stream (re-syncs the DAC to the ADC halves) */
    stream_stop();
    dsp.dac_test = (uint8_t)mode;
    st = dac_config(DAC_TRIGGER_T6_TRGO);
    if (st == HAL_OK) st = stream_start();
    return st;
}
#endif /* !DSP_HOST_TEST */
