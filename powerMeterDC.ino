// powerMeterDC — ESP32-C3 + ST7789V3 (1.69" 240x280 IPS, SPI)
//
// Dashboard UI: header with status, three metric cards (voltage, current,
// power) with icon, big value and a scrolling sparkline, and a footer with
// load, chip temperature and uptime.
//
// Hardware: ESP32-C3 module wired directly to the TFT plus an INA226
// current/voltage monitor on I2C. Voltage/current come from the INA226 when
// present (simulated sweep otherwise, so the UI works on a bare bench).
// Temperature is the ESP32-C3 internal sensor; time is uptime (no RTC).
//
// Display wiring:
//   TFT SCL/SCK  -> GPIO4
//   TFT SDA/MOSI -> GPIO3
//   TFT RES/RST  -> GPIO2
//   TFT DC       -> GPIO5
//   TFT CS       -> GPIO6
//   TFT BLK      -> 3V3 (always on)
//   VCC 3V3, GND common.
//
// INA226 wiring (I2C, orientation auto-probed at boot):
//   INA226 SDA -> GPIO10
//   INA226 SCL -> GPIO7
//   Address auto-detected at boot (0x40..0x4F depending on module straps).
//   Shunt: SHUNT_PARALLEL_N identical R100 resistors stacked in parallel.
//
// Uses Adafruit ST7789 (standard SPI API) instead of TFT_eSPI: TFT_eSPI
// 2.5.43 writes raw SPI registers and crashes on ESP32-C3 with esp32 core
// 3.x (REG_SPI_BASE(SPI2_HOST) resolves to address 0 -> store access fault).

#include <Adafruit_GFX.h>
#include <Adafruit_ST7789.h>
#include <SPI.h>
#include <Wire.h>

// JetBrains Mono Bold — modern monospace — converted to GFXfont with
// Adafruit's fontconvert (see fonts/JetBrainsMono-Bold.ttf). Monospace keeps
// digits from shifting as values change. Custom fonts render from the
// BASELINE and ignore the background color; the classic 5x7 font stays for
// chart axis micro-labels.
#include "fonts/JetBrains32pt.h"
#include "fonts/JetBrains18pt.h"
#include "fonts/JetBrains11pt.h"  // charset 32..176 so it carries the degree sign
#include "icons.h"
#define FONT_HERO JetBrainsMono_Bold32pt7b
#define FONT_VALUE JetBrainsMono_Bold18pt7b
#define FONT_FOOT JetBrainsMono_Bold11pt8b

// Pin assignments
constexpr int8_t PIN_SCK = 4;
constexpr int8_t PIN_MOSI = 3;
constexpr int8_t PIN_RST = 2;
constexpr int8_t PIN_DC = 5;
constexpr int8_t PIN_CS = 6;

// INA226 on I2C. The ESP32-C3 GPIO matrix allows any pin pair, and init
// probes both orientations, so a swapped SDA/SCL self-corrects.
constexpr int8_t PIN_SDA = 10;
constexpr int8_t PIN_SCL = 7;
// Shunt: N identical R100 (0.100 ohm) resistors stacked in parallel on the
// module pads. The INA226 shunt ADC saturates at +/-81.92 mV, so every
// resistor in the stack adds 0.82 A of measurable range.
constexpr int SHUNT_PARALLEL_N = 3;
constexpr float SHUNT_OHMS = 0.100f / SHUNT_PARALLEL_N;
constexpr float SHUNT_FS_VOLTS = 0.08192f;
constexpr float I_FS = SHUNT_FS_VOLTS / SHUNT_OHMS;  // ~2.46 A with N=3

// Panel geometry (1.69" ST7789V3; the library centers the 240x280 window
// in the controller's 240x320 RAM automatically)
constexpr uint16_t TFT_W = 240;
constexpr uint16_t TFT_H = 280;

Adafruit_ST7789 tft = Adafruit_ST7789(&SPI, PIN_CS, PIN_DC, PIN_RST);

// Full-screen canvas: the whole frame is rendered in RAM and pushed in one
// blit, so periodic refreshes never flicker (240x280x2 = ~131 KB heap).
GFXcanvas16 frame(TFT_W, TFT_H);

// Palette (RGB565)
constexpr uint16_t COL_BG = 0x0021;        // near-black navy
constexpr uint16_t COL_CARD = 0x0861;      // card fill
constexpr uint16_t COL_TEXT = 0xFFFF;
constexpr uint16_t COL_MUTED = 0x8C51;     // gray labels
constexpr uint16_t COL_GRID = 0x2124;      // chart grid / dividers
constexpr uint16_t COL_RED = 0xF800;
constexpr uint16_t COL_CYAN = 0x07FF, COL_CYAN_DIM = 0x0208;
constexpr uint16_t COL_YELLOW = 0xFFE0, COL_YELLOW_DIM = 0x4200;
constexpr uint16_t COL_GREEN = 0x07E0, COL_GREEN_DIM = 0x0200;

constexpr uint32_t REFRESH_MS = 200;
uint32_t lastRefresh = 0;

// Sparkline history: ring buffer per metric, one sample per refresh,
// so the chart spans roughly HIST_N * REFRESH_MS of history.
constexpr uint8_t HIST_N = 27;
struct Metric {
  float hist[HIST_N] = {};
  uint8_t head = 0;
  void push(float v) {
    hist[head] = v;
    head = (head + 1) % HIST_N;
  }
};
Metric histVolts, histAmps, histWatts;

// Panel glass: the rounded corners intrude ~GLASS_CORNER_R px into the
// active area, so edge content is inset — header text starts at (16, 8),
// the hero chart and strip stay in mid-screen rows, and the footer band
// lives in y 250..274 with x 8..226.
constexpr int16_t GLASS_CORNER_R = 28;

// Chart axis labels: top, middle, bottom. The power chart is symmetric
// around zero (charge above, discharge below) with unsigned labels; the
// full-scale label is formatted at boot from the sensor's real ceiling.
char axisWLabel[8] = "";
const char *AXIS_W[3] = {axisWLabel, "0", axisWLabel};

// --- Battery model: 4S LiFePO4 pack (EVE IFR40135, 3.2 V 20 Ah 64 Wh/cell) ---
// Datasheet: charge CC/CV to 3.65 V/cell, end of discharge 2.5 V/cell.
// LiFePO4 sits on a very flat plateau between ~20% and ~90% (3.2-3.3 V/cell),
// so a voltage-based estimate is coarse mid-range. Separate charge/discharge
// tables compensate hysteresis and typical IR shift; values are per cell,
// linearly interpolated. Coulomb counting (INA226) will refine this later.
constexpr float PACK_CELLS = 4.0f;
constexpr float PACK_V_MAX = PACK_CELLS * 3.65f;  // CC/CV charge limit

// Power chart full scale: the highest power the sensor can actually report
// (shunt full-scale current at the pack's top voltage), so the trace clips
// exactly where the measurement does.
constexpr float PCHART_MAX = I_FS * PACK_V_MAX;  // ~36 W with N=3

struct SocPoint {
  float v;    // cell terminal voltage
  float soc;  // percent
};

const SocPoint SOC_DISCHARGE[] = {
    {2.50f, 0}, {2.80f, 2},  {3.00f, 5},  {3.13f, 10}, {3.20f, 20},
    {3.23f, 30}, {3.25f, 40}, {3.27f, 50}, {3.28f, 60}, {3.30f, 80},
    {3.32f, 90}, {3.33f, 95}, {3.40f, 100}};

const SocPoint SOC_CHARGE[] = {
    {2.60f, 0},  {3.00f, 2},  {3.20f, 5},  {3.30f, 10}, {3.33f, 20},
    {3.35f, 30}, {3.37f, 40}, {3.38f, 50}, {3.40f, 60}, {3.42f, 70},
    {3.44f, 80}, {3.47f, 90}, {3.55f, 95}, {3.65f, 100}};

float batterySoc(float packVolts, bool charging) {
  float v = packVolts / PACK_CELLS;
  const SocPoint *t = charging ? SOC_CHARGE : SOC_DISCHARGE;
  size_t n = charging ? sizeof(SOC_CHARGE) / sizeof(SocPoint)
                      : sizeof(SOC_DISCHARGE) / sizeof(SocPoint);
  if (v <= t[0].v) return 0.0f;
  if (v >= t[n - 1].v) return 100.0f;
  for (size_t i = 1; i < n; i++)
    if (v < t[i].v)
      return t[i - 1].soc + (t[i].soc - t[i - 1].soc) * (v - t[i - 1].v) /
                                (t[i].v - t[i - 1].v);
  return 100.0f;
}

// --- INA226 acquisition ---
// Minimal register-level driver: bus and shunt voltages are read directly
// and current is computed in firmware (I = Vshunt / Rshunt), so the
// calibration register is not needed and swapping the shunt only changes
// SHUNT_OHMS. Positive current = flow from IN+ to IN- (wire the shunt so
// discharge reads positive; charging then reads negative).
constexpr uint8_t INA_REG_CONFIG = 0x00;
constexpr uint8_t INA_REG_SHUNT = 0x01;   // 2.5 uV/LSB, signed
constexpr uint8_t INA_REG_BUS = 0x02;     // 1.25 mV/LSB
constexpr uint8_t INA_REG_MFG_ID = 0xFE;  // reads 0x5449 ("TI")
constexpr uint8_t INA_REG_DIE_ID = 0xFF;  // reads 0x2260 on an INA226

// 16-sample averaging, 1.1 ms conversions, continuous shunt+bus mode:
// a fresh averaged reading every ~35 ms, well inside the 200 ms refresh.
constexpr uint16_t INA_CONFIG = 0x4527;

// UI development aid: when true and no INA226 answers, readings fall back
// to a simulated sweep instead of reporting ERROR.
constexpr bool SIM_WHEN_ABSENT = false;

bool inaPresent = false;
bool inaError = false;       // an I2C transaction failed this refresh cycle
bool shuntOverload = false;  // shunt ADC at its rail: the reading is clipped
uint32_t lastInaRetry = 0;   // reconnect attempts while the sensor is absent

// Filled by inaInit(): detected address/pins, plus a scan report of every
// I2C device seen (address, SDA pin, manufacturer and die IDs) so a wrong
// module or wiring is diagnosable from the periodic serial line.
uint8_t inaAddr = 0;
int8_t inaSdaPin = -1, inaSclPin = -1;
char inaDiag[96] = "no scan yet";

uint16_t inaRead16(uint8_t reg) {
  Wire.beginTransmission(inaAddr);
  Wire.write(reg);
  if (Wire.endTransmission(false) != 0 ||
      Wire.requestFrom((int)inaAddr, 2) != 2) {
    inaError = true;
    return 0;
  }
  uint16_t v = (uint16_t)Wire.read() << 8;
  return v | Wire.read();
}

void inaWrite16(uint8_t reg, uint16_t val) {
  Wire.beginTransmission(inaAddr);
  Wire.write(reg);
  Wire.write((uint8_t)(val >> 8));
  Wire.write((uint8_t)(val & 0xFF));
  Wire.endTransmission();
}

// Full-bus scan in both pin orientations: every ACKing address gets its ID
// registers read, and the first device that identifies as an INA226 (TI
// manufacturer + 2260 die) is adopted regardless of its address straps, so
// swapping in a differently-strapped module keeps working.
bool inaInit() {
  const struct { int8_t sda, scl; } combos[2] = {{PIN_SDA, PIN_SCL},
                                                 {PIN_SCL, PIN_SDA}};
  size_t used = 0;
  inaDiag[0] = '\0';
  for (const auto &c : combos) {
    Wire.begin(c.sda, c.scl);
    delay(5);
    for (uint8_t addr = 1; addr < 127; addr++) {
      Wire.beginTransmission(addr);
      if (Wire.endTransmission() != 0) continue;
      inaAddr = addr;
      uint16_t mfg = inaRead16(INA_REG_MFG_ID);
      uint16_t die = inaRead16(INA_REG_DIE_ID);
      if (used < sizeof(inaDiag))
        used += snprintf(inaDiag + used, sizeof(inaDiag) - used,
                         "0x%02X(sda%d m%04X d%04X) ", addr, c.sda, mfg, die);
      if (mfg == 0x5449 && die == 0x2260) {
        inaSdaPin = c.sda;
        inaSclPin = c.scl;
        inaWrite16(INA_REG_CONFIG, INA_CONFIG);
        Serial.printf("INA226 at 0x%02X (SDA=%d SCL=%d)\n", inaAddr, c.sda,
                      c.scl);
        return true;
      }
    }
    Wire.end();
  }
  inaAddr = 0;
  if (used == 0) snprintf(inaDiag, sizeof(inaDiag), "no I2C devices found");
  return false;
}

// Readings come from the INA226; with the sensor absent they either flag
// ERROR or, in SIM_WHEN_ABSENT builds, sweep the 4S LiFePO4 range so the
// UI stays exercisable on a bare bench.
float readVoltage() {
  if (inaPresent) return (int16_t)inaRead16(INA_REG_BUS) * 1.25e-3f;
  if (SIM_WHEN_ABSENT) return 13.2f + 0.65f * sinf(millis() / 7000.0f);
  inaError = true;
  return 0.0f;
}

float readCurrent() {
  if (inaPresent) {
    int16_t raw = (int16_t)inaRead16(INA_REG_SHUNT);
    shuntOverload = raw >= 32700 || raw <= -32700;
    return raw * 2.5e-6f / SHUNT_OHMS;
  }
  if (SIM_WHEN_ABSENT) return 0.8f + 1.0f * sinf(millis() / 5000.0f);
  inaError = true;
  return 0.0f;
}

void drawTextRightAligned(const char *s, int16_t xRight, int16_t y) {
  frame.setCursor(xRight - (int16_t)strlen(s) * 6, y);
  frame.print(s);
}

// Sparkline with grid, highlighted zero baseline (wherever v=0 maps in the
// vmin..vmax range) and area filled from that baseline.
void drawChart(int16_t px, int16_t py, int16_t w, int16_t h,
               const Metric &m, float vmin, float vmax, const char *axis[3],
               uint16_t accent, uint16_t dim) {
  int16_t y0 = py + (int16_t)(vmax / (vmax - vmin) * h);  // zero baseline
  for (uint8_t i = 0; i < 5; i++) {
    int16_t gy = py + (h * i) / 4;
    frame.drawFastHLine(px, gy, w, COL_GRID);
  }
  frame.drawFastHLine(px, y0, w, COL_MUTED);
  frame.setTextSize(1);
  frame.setTextColor(COL_MUTED);
  drawTextRightAligned(axis[0], px - 5, py - 3);
  drawTextRightAligned(axis[1], px - 5, py + h / 2 - 3);
  drawTextRightAligned(axis[2], px - 5, py + h - 3);

  float step = (float)w / (HIST_N - 1);
  int16_t prevX = 0, prevY = 0;
  for (uint8_t i = 0; i < HIST_N; i++) {
    float v = m.hist[(m.head + i) % HIST_N];  // oldest to newest
    v = constrain(v, vmin, vmax);
    int16_t sx = px + (int16_t)(i * step);
    int16_t sy = py + (int16_t)((vmax - v) / (vmax - vmin) * h);
    if (i > 0) {
      for (int16_t x = prevX; x <= sx; x++) {  // fill toward the baseline
        int16_t yy = prevY + (sy - prevY) * (x - prevX) / (sx - prevX);
        if (yy <= y0)
          frame.drawFastVLine(x, yy, y0 - yy + 1, dim);
        else
          frame.drawFastVLine(x, y0, yy - y0 + 1, dim);
      }
      frame.drawLine(prevX, prevY, sx, sy, accent);
    }
    prevX = sx;
    prevY = sy;
  }
}

// Lightning bolt glyph for the power card icon
void drawBolt(int16_t cx, int16_t cy, uint16_t color) {
  frame.fillTriangle(cx + 5, cy - 11, cx - 6, cy + 3, cx + 1, cy + 3, color);
  frame.fillTriangle(cx - 5, cy + 11, cx + 6, cy - 3, cx - 1, cy - 3, color);
}

// Centered text row inside one of the two bottom panels (114 px wide)
void drawPanelText(int16_t x, int16_t baseline, const char *text,
                   uint16_t color) {
  int16_t bx, by;
  uint16_t bw, bh;
  frame.setFont(&FONT_VALUE);
  frame.setTextColor(color);
  frame.getTextBounds(text, 0, 0, &bx, &by, &bw, &bh);
  frame.setCursor(x + (114 - (int16_t)bw) / 2, baseline);
  frame.print(text);
  frame.setFont();
}

// Temperature and uptime only, no icons; the time colon blinks each second.
// Raised and inset so the panel's rounded corners don't clip it.
void drawFooter() {
  constexpr int16_t y = 250;
  frame.setFont(&FONT_FOOT);
  frame.setTextSize(1);
  frame.setTextColor(COL_TEXT);

  // centered row: temp value, thermo icon, small gap, hourglass icon, time
  char tempText[12];
  snprintf(tempText, sizeof(tempText), "%04.1f\xB0" "C", temperatureRead());

  uint32_t secs = millis() / 1000;
  unsigned long hh = secs / 3600, mm = secs / 60 % 60;
  char timeText[12];
  snprintf(timeText, sizeof(timeText), "%03lu:%02lu", hh, mm);

  int16_t bx, by;
  uint16_t tw, th, uw, uh;
  frame.getTextBounds(tempText, 0, 0, &bx, &by, &tw, &th);
  frame.getTextBounds(timeText, 0, 0, &bx, &by, &uw, &uh);
  int16_t x = (TFT_W - ((int16_t)tw + 60 + (int16_t)uw)) / 2;

  frame.setCursor(x, y + 20);
  frame.print(tempText);
  frame.drawRGBBitmap(x + tw + 4, y + 1, ICON_TEMP, 22, 22);
  frame.drawRGBBitmap(x + tw + 34, y + 1, ICON_TIME, 22, 22);

  if (secs % 2) {  // blink the colon
    char *colon = strchr(timeText, ':');
    if (colon) *colon = ' ';
  }
  frame.setCursor(x + tw + 60, y + 20);
  frame.print(timeText);
  frame.setFont();
}

// Focus layout: power is the hero metric — big centered value with flow
// arrows, full-width trend — and voltage/current in a strip below.
void drawDashboard(float volts, float amps, float watts) {
  frame.fillScreen(COL_BG);

  // hero value, centered, magnitude only. ERROR when the sensor stopped
  // answering, OVLD when the shunt ADC clips: in both cases the number it
  // would show is not the real measurement.
  char text[8];
  bool fault = inaError || shuntOverload;
  if (inaError)
    snprintf(text, sizeof(text), "ERROR");
  else if (shuntOverload)
    snprintf(text, sizeof(text), "OVLD");
  else
    snprintf(text, sizeof(text), "%04.1f%c", fabsf(watts), 'W');
  frame.setFont(&FONT_HERO);
  frame.setTextColor(fault ? COL_RED : COL_GREEN);
  int16_t bx, by;
  uint16_t bw, bh;
  frame.getTextBounds(text, 0, 0, &bx, &by, &bw, &bh);
  frame.setCursor((TFT_W - (int16_t)bw) / 2, 56);
  frame.print(text);
  frame.setFont();

  // full-width power trend
  drawChart(28, 68, 204, 88, histWatts, -PCHART_MAX, PCHART_MAX, AXIS_W,
            COL_GREEN, COL_GREEN_DIM);

  // left column: voltage and current
  frame.fillRoundRect(4, 164, 114, 80, 8, COL_CARD);
  snprintf(text, sizeof(text), "%04.1fV", volts);
  drawPanelText(4, 196, text, COL_CYAN);
  snprintf(text, sizeof(text), "%04.1fA", fabsf(amps));
  drawPanelText(4, 236, text, COL_YELLOW);

  // right column: charge state and battery level (voltage-based SoC)
  frame.fillRoundRect(122, 164, 114, 80, 8, COL_CARD);
  // below 0.1 A the pack is effectively at rest: show IDLE instead of a
  // flow direction; charging still selects the SoC table
  bool charging = amps < -0.01f;
  bool idle = fabsf(amps) < 0.1f;
  drawPanelText(122, 196, idle ? "IDLE" : (charging ? "CHARG" : "DISCH"),
                idle ? COL_TEXT : (charging ? COL_GREEN : COL_RED));
  snprintf(text, sizeof(text), "%3.0f%%", batterySoc(volts, charging));
  drawPanelText(122, 236, text, COL_TEXT);

  drawFooter();
  tft.drawRGBBitmap(0, 0, frame.getBuffer(), TFT_W, TFT_H);
}

void setup() {
  Serial.begin(115200);

  SPI.begin(PIN_SCK, -1 /* no MISO */, PIN_MOSI, PIN_CS);
  tft.init(TFT_W, TFT_H);
  tft.setSPISpeed(40000000);
  tft.setRotation(2);  // portrait, connector at the top

  if (!frame.getBuffer()) {
    Serial.println("ERROR: frame canvas allocation failed");
    while (true) delay(1000);
  }

  inaPresent = inaInit();
  if (!inaPresent) Serial.println("INA226 not found");
  snprintf(axisWLabel, sizeof(axisWLabel), "%.0f", PCHART_MAX);

  Serial.println("powerMeterDC: display initialized");
}

void loop() {
  if (millis() - lastRefresh >= REFRESH_MS) {
    lastRefresh = millis();

    // silent reconnect attempt so plugging the sensor back recovers the
    // meter without a reboot
    if (!inaPresent && millis() - lastInaRetry >= 2000) {
      lastInaRetry = millis();
      inaPresent = inaInit();
    }

    inaError = false;  // re-evaluated by this cycle's reads
    float volts = readVoltage();
    float amps = readCurrent();
    float watts = volts * amps;
    histVolts.push(volts);
    histAmps.push(amps);
    // chart convention: charge rises above zero, discharge dips below,
    // same color either way (positive current = discharge)
    histWatts.push(-watts);

    drawDashboard(volts, amps, watts);
    if (inaPresent)
      Serial.printf("V=%.2f  I=%.2f  P=%.2f  [INA 0x%02X SDA=%d SCL=%d]\n",
                    volts, amps, watts, inaAddr, inaSdaPin, inaSclPin);
    else
      Serial.printf("V=%.2f  I=%.2f  P=%.2f  [scan: %s]\n", volts, amps,
                    watts, inaDiag);
  }
}
