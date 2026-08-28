# 08 — EVK Pmod pinout (where the pins physically are)

The EVK ver1 exposes its user-accessible pins on four Pmod headers. All signals
are **3.3 V**. On every header, **pins 5/11 = GND** and **pins 6/12 = +3.3 V**.

Cross-references to the rest of this kit are marked inline. Source: board
schematic net names, cross-checked on the live board 2026-08-28.

## CN1 — Pmod Type1/1A (GPIO)

| Pin | Signal | Pin | Signal |
|---|---|---|---|
| 1 | P60 | 7 | P64 |
| 2 | P61 | 8 | P65 |
| 3 | P62 | 9 | P66 |
| 4 | P63 | 10 | P67 |

Eight plain GPIOs, no known Linux claims.

## CN2 — Pmod Type2/2A (SPI)

| Pin | Signal | Pin | Signal |
|---|---|---|---|
| 1 | PA7 / RSPI2_SSLA | 7 | P74 / IRQ6 |
| 2 | PB4 / RSPI2_MOSI | 8 | P75 |
| 3 | PB3 / RSPI2_MISO | 9 | PA2 / RSPI2_SSLB |
| 4 | PB5 / RSPI2_SCK | 10 | PA4 / RSPI2_SSLC |

⚠ PB3 is claimed by Linux (`12800800.spi`). PA7 doubles as **GTIOC7B** in the
PWM-demo firmware — usable in principle, but it shares the header with the SPI
nets; prefer GPT6 on CN3.

## CN3 — Pmod Type3/3A (UART)

| Pin | Signal | Pin | Signal |
|---|---|---|---|
| 1 | P52 / SCI0_CTS | 7 | P57 / IRQ13 |
| 2 | P50 / SCI0_TXD | 8 | P73 |
| 3 | P51 / SCI0_RXD | 9 | **P76** |
| 4 | P53 / SCI0_RTS | 10 | **P77** |

- **Pins 9/10 = P76/P77 = GTIOC6A/6B — the PWM web demo's outputs**
  (docs/07). Scope ground is right there on pin 5 or 11.
- ⚠ The SCI0 pins (1–4) are labeled UART but the factory device tree binds
  them to a Linux **SPI** driver (`renesas,rsci-spi`, no spidev child). They
  are not free, and P50 doubles as GTIOC8A — commands to GPT8 succeed but the
  pin belongs to Linux.
- P73 (pin 8) used to be the GPS UART (RSCI5 RXD) in the sensors firmware.

## CN6 — Pmod Type6/6A (I2C)

| Pin | Signal | Pin | Signal |
|---|---|---|---|
| 1 | P55 / IRQ9 | 7 | P70 / IRQ0 |
| 2 | P54 | 8 | P72 / IRQ4 |
| 3 | P21 / I2C2_SCL | 9 | P90 |
| 4 | P20 / I2C2_SDA | 10 | P91 |

- ⚠ **P70 (pin 7) = GTIOC0A = the jitter-probe's timebase** in the PWM-demo
  firmware. You *can* watch it, but sending GPT0 commands wrecks the jitter
  statistics (docs/07).
- P72 (pin 8) used to be the GPS UART TXD (RSCI5) in the sensors firmware.

## Not on any Pmod header

P84/P85 (GPT5 — also Linux's ttySC1 UART), P95 (GPT4), P96/P97 (GPT9),
PA6 (GTIOC7A). Treat them as unreachable on the EVK unless you find them on
another connector.
