# Task: add the FM30 front-panel LCD (STM32G431, fm30ctl)

Read CLAUDE.md first. This adds the stock FM30 2×8 character LCD back, driven by the STM32.
Propose the diff before applying. Keep `include/config.h` the single source of truth for pins.

## Final IO map (ALL header pins used — no spares)

| U1 pin | Signal | STM32 | Nucleo | Mode |
|---|---|---|---|---|
| 1 | IRQ ← overmod comparator U5:A | PA9 | D1 | input, EXTI falling (EXTI9_5) |
| 2 | LDAC (U6+U7) | PA10 | D0 | OD + 4.7k to 5 V |
| 6 | U2 CS (MCP42010) | PA12 | D2 | OD + 4.7k to 5 V (HW1 jumper removed) |
| 8 | U3 CE (BH1415F, active high) | PB7 | D4 | OD + 4.7k to 5 V |
| 9 | U7 CS (MCP4921 RF power) | PB6 | D6 | OD + 4.7k to 5 V |
| 13 | Bus DATA | PB5 | D11 | OD + 4.7k to 5 V |
| 14 | Bus CLK | PB4 | D12 | OD + 4.7k to 5 V |
| 25 | LOCK LED | PA7 | A6 | push-pull 3.3 V |
| 27 | U6 CS (MCP4921 dev comp) | PA11 | D10 | OD + 4.7k to 5 V |
| 20 | Vtune/2 (no extra 47k fitted → VTSCALE 2.0) | PA1 | A1 | analog, ADC2 |
| — | Pilot/composite in | PA0 | A0 | analog, ADC1 (TIM6 228 kS/s DMA) |
| — | RDS out | PA4 | A3 | DAC1_OUT1 (TIM6 DMA) |
| **15** | **LCD D4** (LCD pin 11) | **PB3** | D13 | **push-pull 3.3 V** |
| **12** | **LCD D5** (LCD pin 12) | **PB0** | D3 | push-pull 3.3 V |
| **11** | **LCD D6** (LCD pin 13) | **PA15** | D5 | push-pull 3.3 V |
| **10** | **LCD D7** (LCD pin 14) | **PA8** | D9 | push-pull 3.3 V |
| **18** | **LCD RS** (LCD pin 4) | **PA5** | A4 | push-pull 3.3 V (SB2 must be OPEN) |
| **17** | **LCD E** (LCD pin 6) | **PA6** | A5 | push-pull 3.3 V (SB3 must be OPEN; remove 10k pull-down on U1-17) |
| 3 / 7 | GND / +5 V | | | |

Reserved / unusable: PA2/PA3 (VCP now, ESP32 link later), PF0/PF1 (24 MHz crystal), PB8 (LD2), PA13/PA14 (SWD).
Optional future buzzer (U1-26): only by freeing a pin — e.g. tie LDAC (U1-2) to GND, drop the LDAC pulse in code
(MCP4921 then updates on CS↑), and use PA10 for the buzzer (TIM-capable). Not part of this task.

## LCD hardware facts
- HD44780-compatible 2 lines × 8 chars, module powered at **5 V**, 14-pin header, no backlight pins.
- **4-bit mode, write-only**: R/W (LCD 5) is hard-tied to GND on the FM30 → cannot read busy flag → use delays.
- 3.3 V logic is fine (HD44780 V_IH ≈ 2.2 V at 5 V). Push-pull outputs, no pull-ups.
- Data nibble pins span two ports (PB3, PB0, PA15, PA8) → set bits individually via BSRR, not a port write.
- Line 2 DDRAM address 0x40. Contrast (V0) is on the FM30 board.

## Driver requirements (`src/lcd.c`, `include/lcd.h`)
- Init per HD44780 4-bit sequence: wait ≥ 40 ms after power, send 0x3 nibble ×3 (4.1 ms, 100 µs, 100 µs),
  then 0x2; then 0x28 (4-bit, 2 lines, 5×8), 0x08, 0x01 (clear, ≥ 1.6 ms), 0x06, 0x0C.
- E pulse ≥ 450 ns high; ≥ 40 µs per command/char; ≥ 1.6 ms for Clear/Home. Use the existing DWT `delay_us`.
- **Non-blocking redraw**: keep a 16-char shadow buffer, and in the main loop write only changed chars,
  a few per pass (never block the loop > ~200 µs). Never write the LCD from an ISR.
- API: `lcd_init()`, `lcd_task()`, `lcd_print(line, col, const char*)` into the shadow buffer.
- The DSP ISR (priority 1) may preempt LCD writes — fine, HD44780 timing is minimum-only.

## Screens (2 × 8). `src/ui.c`, called from main loop at ~5 Hz.
Rotate pages every 3 s while ON AIR; fault/tuning states override rotation.

```
Page 1 (freq/state)     Page 2 (RDS PS)        Page 3 (power/Vtune)    Page 4 (RDS status)
+--------+              +--------+             +--------+              +--------+
| 98.1MHz|              |RDS  PS:|             |PWR  88%|              |PLT LOCK|
|ON AIR L|              |MY STATN|             |Vt 3.80V|              |RDS  ON |
+--------+              +--------+             +--------+              +--------+
```
- Line 2 of page 1 status: `ON AIR` / `TUNING` / `TX OFF` / `FAULT`; last char `L` = PLL locked, `*` = overmod
  seen in last 1 s (g_ovm_count changed).
- Page 3 power % = power_now * 100 / POWER_MAX; Vtune from fm30.vtune (2 decimals).
- Page 4: `PLT LOCK` / `PLT ----` (dsp.pilot_lock); `RDS  ON`/`RDS OFF`/`RDS MUT` (enabled but no pilot).
- Override screens:
  - TUNING: `98.1MHz` / `TUNING..` (animate dots).
  - FAULT: `FAULT   ` / first 8 chars of fm30.fault_msg (e.g. `LOCK TMO`, `RAILED`).
  - Boot: `FM30 RDS` / `fw 0.3  ` for 1.5 s.
- CLI additions: `LCD ON|OFF` (blank/restore), `LCD PAGE n` (pin a page, 0 = rotate). Persist in settings.

## Done criteria
- Builds with zero warnings (`pio run`); host DSP test still `RESULT: PASS`.
- No change to bus timing or the tune/lock state machine.
- Measure: main-loop worst-case blocking with LCD active < 250 µs (log via a `STATUS` field `loop_max_us`).
