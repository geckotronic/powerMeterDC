# powerMeterDC

DC power meter for a 4S LiFePO4 battery pack, built around an ESP32-C3
SuperMini, a Waveshare 2.13" e-paper display and an INA226 current/voltage
monitor. It shows live power with charge/discharge direction arrows,
voltage, current, uptime, an estimated state of charge and the chip
temperature — on a display that keeps its image with zero power.

![powerMeterDC live on the e-paper display](docs/meter-live.jpg)

## Hardware

| Part | Notes |
|---|---|
| ESP32-C3 SuperMini | esp32 Arduino core 3.x, native USB CDC |
| Waveshare Pico-ePaper-2.13 | 250x122, black/white, SSD1680 class, SPI |
| INA226 module | I2C, onboard R100 shunt pads |
| Battery pack | 4S LiFePO4, EVE IFR40135 cells (3.2 V, 20 Ah, 64 Wh each) |

### Display wiring (SPI + control)

| EPD pin | ESP32-C3 |
|---|---|
| DIN (MOSI) | GPIO4 |
| CLK (SCK) | GPIO3 |
| CS | GPIO2 |
| DC | GPIO1 |
| RST | GPIO5 |
| BUSY | GPIO6 |

### INA226 wiring (I2C)

| INA226 pin | ESP32-C3 |
|---|---|
| SDA | GPIO10 |
| SCL | GPIO7 |

GPIO10 was chosen because it is one of the few fully free pins on the
SuperMini (no strapping role, no onboard LED). The I2C address does not
need configuring: init scans the bus and identifies the chip by its ID
registers, so any strap combination (0x40..0x4F) works.

### Shunt

The INA226 shunt ADC has a fixed +/-81.92 mV full scale, so the shunt value
sets the measurable range. This build stacks **3 identical R100 resistors in
parallel** on the module pads (33.3 mOhm):

```
I_max = 81.92 mV / 33.3 mOhm = +/-2.46 A
```

Each additional stacked resistor widens the range by 0.82 A. The count
lives in one constant (`SHUNT_PARALLEL_N`); current math and the overload
threshold derive from it. Currents beyond the range do not damage the
chip — the reading saturates and the display reports `OVLD`.

For the pack's full 10 A standard charge/discharge a lower shunt is
required; 4x R025 in parallel (6.25 mOhm, +/-13.1 A) is the planned
upgrade, and swapping it is a one-constant change.

## Firmware

Single sketch (`powerMeterDC.ino`), Arduino framework, no external sensor
library — the INA226 is driven at register level; the display uses GxEPD2.

### Display pipeline (e-paper)

- **GxEPD2** with the `GxEPD2_213_BN` (SSD1680) driver class; full-frame
  buffer is ~4 KB of RAM.
- **Refresh only on change**: readings are sampled every second, but the
  panel redraws only when a displayed string actually changed. E-paper
  keeps its image for free, so an idle meter performs no refreshes at all.
- **Fast partial updates** (~0.3 s, no black flash) carry all normal
  value changes. A plain **full refresh** runs at boot and every 10 minutes
  of uptime to clear accumulated ghosting.
- **The clock colon blinks on every refresh actually performed** — it is
  display-activity feedback, not a timebase. It is excluded from change
  detection, so the blink never causes a refresh by itself; a frozen colon
  means the panel is at rest, not dead.
- **Temperature is rounded to 0.5 °C** before display so sensor jitter
  does not force refreshes.
- **Custom fonts**: JetBrains Mono Bold converted with Adafruit's
  `fontconvert` (sources and generated headers in `fonts/`). Monospace
  keeps digit columns stable as values change; the footer font uses
  charset 32..176 to carry the degree sign.

### Measurement

- Register-level INA226 driver: bus voltage (1.25 mV/LSB) and shunt voltage
  (2.5 uV/LSB) are read directly; current is computed in firmware as
  `I = Vshunt / SHUNT_OHMS`. The calibration register is unused, which keeps
  shunt changes a one-constant edit.
- Config `0x4527`: 16-sample averaging, 1.1 ms conversions, continuous
  shunt+bus — a fresh averaged reading every ~35 ms.
- **Auto-detection**: init scans every I2C address in both SDA/SCL pin
  orientations and verifies the manufacturer (0x5449 "TI") and die (0x2260)
  IDs, then adopts whatever address the module straps selected. The periodic
  serial line reports the detection (or the full scan result on failure).
- **Fault states**, in priority order on the main value: `ERROR` when any
  I2C transaction fails (checked every cycle — covers a wire falling off
  mid-run), `OVLD` while the shunt ADC is saturated. While the sensor is
  absent the firmware retries detection every 2 s, so plugging it back
  recovers the meter without a reboot. Rationale: a clipped or stale number
  looks plausible and misleads; an explicit fault state does not.
- Positive current = discharge (flow IN+ → IN-); a 10 mA deadband keeps
  offset noise from flickering the direction arrows.
- `SIM_WHEN_ABSENT` replaces missing-sensor `ERROR` with a simulated sweep,
  for UI work on a bare bench (currently enabled while the measurement
  front-end is being rebuilt; set to `false` for deployment).

### State of charge

Voltage-based estimate for the 4S pack: per-cell voltage is interpolated
over two lookup tables (charge and discharge) built from the EVE IFR40135
datasheet curves; the table is selected by current direction to compensate
hysteresis and IR shift. LiFePO4 sits on a very flat plateau (3.2–3.3 V per
cell from ~20% to ~90%), so mid-range values are coarse estimates — accurate
near the knees and at rest. Coulomb counting with voltage re-anchoring is
the planned refinement.

### Dashboard layout (250x122, black on white)

- **Hero row**: instantaneous |P| in watts, with two stacked arrows beside
  it — up = charging, down = discharging. Both are always drawn; only the
  active direction is filled, and below 0.1 A (idle) both stay outlined.
- **Middle row**: pack voltage left, |I| right.
- **Footer**: uptime HHH:MM (blinking colon) and SoC left, chip
  temperature right.

## Building and flashing

Requires `arduino-cli` with the esp32 core (3.x) and the libraries
`GxEPD2`, `Adafruit GFX` and `Adafruit BusIO`.

```sh
./arduinocli-esp32c3.sh            # compile and flash (port auto-detected)
./arduinocli-esp32c3.sh compile    # compile only
./arduinocli-esp32c3.sh monitor    # serial monitor (115200)
./arduinocli-esp32c3.sh reset      # reset the board without flashing
```

FQBN: `esp32:esp32:esp32c3:UploadSpeed=921600,CDCOnBoot=cdc` — serial goes
to the native USB port, no UART bridge needed.

To regenerate fonts, compile `fontconvert` from the Adafruit GFX library
(needs freetype) and run it over the TTFs in `fonts/`.

## Roadmap

- Solder the 4x R025 parallel shunt (+/-13.1 A) to cover the pack's full
  10 A standard charge and discharge.
- Always-on battery-side power through a 12 V → 3.3 V regulator: deep sleep
  between samples — the e-paper keeps its image at zero power, so only the
  MCU and INA226 budgets matter.
- Coulomb counting (20 Ah capacity) with voltage re-anchoring at the curve
  knees for a drift-free SoC.
