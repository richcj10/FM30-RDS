# FM30 headless controller + RDS encoder (NUCLEO-G431KB)

This firmware replaces the MC68HC908JL3 (U1) in a Ramsey FM30. The STM32 drives the FM30 control bus directly and also generates pilot-locked RDS. It also drives the front-panel 2×8 LCD for status and messages. The front-panel buttons and the buzzer are not used. An ESP8266 controls everything over serial (USART2, 115200 8N1). For bench work, the same commands work from a PC over the Nucleo's ST-Link USB serial port.

## Build / flash (PlatformIO)

```
pio run -t upload          # ST-Link on the Nucleo
pio device monitor         # 115200, type HELP
```

Before you build, check two things on the Nucleo:

- **SB9/SB10 must be closed** so the 24 MHz crystal (X2) is connected. If they're open, the firmware falls back to the HSI oscillator and prints `clock: HSI` at boot.

## Wiring (U1 socket → Nucleo)

Remove the JL3 from its socket. Wire from the socket pins:

| Nucleo | STM32 | → U1 pin | Signal | Notes |
|---|---|---|---|---|
| D1 | PA9 | 1 | IRQ: overmod in (U5:A) | **measure first**, must be ≤ 5 V |
| D0 | PA10 | 2 | LDAC (U6 + U7) | open-drain, 4.7k to +5 V |
| D2 | PA12 | 6 | U2 CS (volume/balance pot) | open-drain, 4.7k to +5 V; **remove the HW1 jumper** |
| D4 | PB7 | 8 | U3 CE (BH1415F, active high) | open-drain, 4.7k to +5 V; SB2 open |
| D6 | PB6 | 9 | U7 CS (RF power DAC) | open-drain, 4.7k to +5 V |
| D11 | PB5 | 13 | Bus DATA | open-drain, 4.7k to +5 V |
| D12 | PB4 | 14 | Bus CLK | open-drain, 4.7k to +5 V |
| A6 | PA7 | 25 | LOCK LED | push-pull 3.3 V |
| D10 | PA11 | 27 | U6 CS (deviation comp DAC) | open-drain, 4.7k to +5 V |
| A0 | PA0 | — | Pilot / composite in | from RDS front end |
| A1 | PA1 | 20 | Vtune/2 node | add **47k PA1 → GND** (Vadc = Vtune/3) |
| A3 | PA4 | — | RDS DAC out | to RDS front end |
| D13 | PB3 | 15 | LCD D4 (LCD pin 11) | push-pull 3.3 V |
| D3 | PB0 | 12 | LCD D5 (LCD pin 12) | push-pull 3.3 V |
| D5 | PA15 | 11 | LCD D6 (LCD pin 13) | push-pull 3.3 V |
| D9 | PA8 | 10 | LCD D7 (LCD pin 14) | push-pull 3.3 V |
| A4 | PA5 | 18 | LCD RS (LCD pin 4) | push-pull 3.3 V; **SB2 open** |
| A5 | PA6 | 17 | LCD E (LCD pin 6) | push-pull 3.3 V; **SB3 open**, no pull-down on U1-17 |
| GND | GND | 3 | GND | |
| — | — | 7 | +5 V | feeds the pull-up network only |

Every header pin is now in use. These are reserved:

| Pins | Why |
|---|---|
| D7, D8 | 24 MHz crystal (SB9/SB10 closed) |
| A2, A7 | USART2 (PA3/PA2): ESP8266 link / ST-Link VCP |

Not connected: the front-panel buttons and the buzzer (U1-26).

**LCD:** the stock HD44780-compatible 2×8 module, powered at 5 V on the FM30. It runs in 4-bit write-only mode; R/W is tied to GND on the FM30, so the LCD never drives the bus, and 3.3 V logic meets its V<sub>IH</sub>. PB0, PA5 and PA6 are not 5 V tolerant, so make sure the FM30 has no 5 V pull-ups on the LCD lines.

**Pull-ups:** one 8-pin bussed 4.7k SIP network handles all seven bus lines. Its common pin goes to U1-7 (+5 V).

RDS front end (from the earlier schematic): composite tap → **PA0 / A0**, and **PA4 / A3** → reconstruction filter → 220k → the R10/R5 junction.

Notes:

- **The bus is bit-banged in the main loop.** The RDS DSP (DMA plus a priority-1 interrupt) and the USB-serial receive (interrupt into a ring buffer) both preempt it. A bus write just stretches by a few hundred µs, which the SPI-style chips don't care about.
- **Pull-ups are required.** All the bus pins are 5 V-tolerant. They only pull low, and the 4.7k pull-ups take them up to 5 V. 3.3 V push-pull would not meet the MCP4921/MCP42010 V<sub>IH</sub> of 3.5 V.
- **Power and ground.** Keep the Nucleo powered whenever the FM30 is on, because the bus pull-ups sit on the FM30's 5 V rail. Tie the Nucleo GND and the ESP8266 GND to the FM30 GND.

## Behavior

1. **Boot:** U7 = 0 (RF off). U2 gets the saved volume/balance, then a full tune runs.
2. **Tune:**
   - Write U7 = 0.
   - Write U3 (the same 3-word pattern as the stock firmware).
   - Sample Vtune every 100 ms until it holds within 40 mV for 1 s, with an 8 s timeout. A railed Vtune counts as a fault.
3. **Locked:**
   - Write U6 from the captured frequency table (plus `U6TRIM`).
   - Soft-start U7 at +8 every 10 ms up to the target.
   - Turn on the LOCK LED.
4. **On air:** Vtune is checked every 200 ms. If it jumps more than 0.5 V or rails, RF goes to 0, `state` becomes `fault`, and the firmware retries after 2 s.
5. **Power changes:** decreases apply immediately; increases soft-start.
6. **RDS:**
   - A software PLL locks a 19 kHz NCO to the pilot (about 0.3–0.4 s to lock).
   - The 57 kHz subcarrier is exactly 3× the NCO phase, and the bit clock is 57 kHz / 48.
   - RDS is muted until the pilot locks, unless `RDSFREE ON` is set.
   - Groups sent: 0A (PS) alternating with 2A (RT).

Board LED (LD2):

| Pattern | Meaning |
|---|---|
| Solid | On air, pilot locked |
| Slow blink | On air, no pilot |
| Fast blink | Tuning or fault |
| Off | TX off |

### Front-panel LCD (2×8)

Boot shows `FM30 RDS` / `fw 0.3` for 1.5 s. After that, the pages rotate every 3 s:

| Page | Line 1 | Line 2 |
|---|---|---|
| 1 | ` 98.1MHz` | `ON AIR L` / `TX OFF` (`L` = locked, `*` = overmod in the last 1 s) |
| 2 | `RDS  PS:` | the PS name |
| 3 | `PWR  88%` | `Vt 3.80V` |
| 4 | `PLT LOCK` / `PLT ----` | `RDS  ON` / `RDS OFF` / `RDS MUT` (enabled, no pilot) |
| 5–8 | message slots 1–4, set over serial with `LCD MSG` | |

- **Overrides:** while tuning the screen shows the frequency and `TUNING..`. A fault shows `FAULT` and a short reason (`LOCK TMO`, `RAILED`, `LOCK LST`).
- **Scrolling:** a bottom row longer than 8 characters scrolls. A page that scrolls stays up until it has finished one full pass.
- **Empty slots:** message slots with no text are skipped in the rotation.
- **Saved vs not saved:** LCD on/off, the pinned page and the rotation list are saved with `SAVE`. Message text isn't saved, so the host should send it again after a reboot.

## Commands (115200 8N1, one per line, replies `OK…`/`ERR…`)

| Command | |
|---|---|
| `FREQ 98.1` | retune (87.5–108.0) |
| `POWER 3615` / `POWER 80%` | U7 code (max 3968) |
| `TX ON` / `TX OFF` | RF on/off |
| `VOL 0-255`, `BAL -127..127` | U2 pots |
| `U6TRIM n`, `U6 code`, `U6 AUTO` | deviation-compensation trim / override |
| `VTSCALE 3.0` | Vtune scaling; match it to a DMM on C38 |
| `RDS ON/OFF`, `RDSFREE ON/OFF` | RDS enable; allow RDS without pilot |
| `RDSLEVEL n` | DAC peak counts (default 400); set injection to ~3–5 % (2–4 kHz dev) |
| `RDSPHASE deg` | 57 kHz phase trim vs pilot |
| `PI C0DE`, `PTY n`, `TP/TA/MS/STEREO 0/1` | RDS flags |
| `PS text`, `RT text` | station name (8), radiotext (64) |
| `STATUS`, `STATUS 5` | JSON status now / every 5 s |
| `LCD ON` / `LCD OFF` | blank or restore the LCD |
| `LCD MSG 1 WiFi OK\|192.168.1.23` | message slot 1–4 (pages 5–8): top \| bottom, bottom up to 48 chars; no `\|` = all on the bottom row; `LCD MSG 1` alone clears it |
| `LCD ROTATE 1 5 6`, `LCD ROTATE ALL` | pages included in the rotation |
| `LCD PAGE 5`, `LCD PAGE 0` | pin one page (1–8); 0 = rotate |
| `SAVE`, `DEFAULTS`, `HELP` | |

Example status:

```
{"fw":"fm30ctl 0.1","state":"on_air","fault":"","faults":0,"freq":98.1,"tx":1,"power":3615,
 "power_target":3615,"u6":1412,"u6_trim":0,"vtune":3.80,"vtune_expected":3.80,"vol":128,"bal":0,
 "ovm":0,"pilot_lock":1,"pilot_hz":19000.01,"pilot_ampl":88.0,"pilot_err":0.002,"rds":1,...}
```

Host side: the ESP8266 sends these commands as plain text lines, for example `LCD MSG 1 WiFi OK|192.168.1.23` once it joins the network. For bench tests from a PC, `tools/fm30.py` (pyserial) sends the same commands over the ST-Link port, e.g. `fm30.py rt "Now playing: $TITLE"`.

## Tests

`tools/test_dsp.c` runs the real `dsp.c` and `rds.c` on a PC. It feeds them a synthetic composite (pilot with an offset, audio, L−R, noise), demodulates the output, and checks that PS/RT decode with every CRC passing:

```
cd tools && cc -O2 -DDSP_HOST_TEST -I../include -I. test_dsp.c ../src/dsp.c ../src/rds.c -lm -o test_dsp && ./test_dsp 3
```

Results: 45/45 groups decoded, and the carrier is 0.4° from 3× pilot at pilot offsets of 0, +3, −8 and +40 Hz.

## Bring-up order

1. **Bus first, no RF.** Flash the board and wire everything. Run `TX OFF`, then `FREQ 98.1`, then `STATUS`. Vtune should read about 3.8 V, which tells you the bus and the PLL work. Compare it with a DMM on C38 and adjust `VTSCALE` if needed.
2. **RF on.** Run `TX ON` and check that the LOCK LED comes on and you hear the carrier on a receiver.
3. **Saleae check.** With the Saleae on the bus, the traffic should match the stock captures: U3 3-word pattern, U6, then the U7 ramp.
4. **LCD.** At boot you should see `FM30 RDS` / `fw 0.3`, then the pages. If the display stays blank, check SB2/SB3 are open, the LCD contrast pot and the 5 V supply to the module.
5. **RDS.** Feed in audio and check that `pilot_lock` goes to 1. Use an RDS receiver or an SDR (RDS Spy, gr-rds) to adjust `RDSLEVEL`, then `RDSPHASE`. Then `SAVE`.
