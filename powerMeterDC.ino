// powerMeterDC — ESP32-C3 + Waveshare Pico-ePaper-2.13 (250x122 BW) + INA226
//
// Dashboard UI on a 2-color e-paper: big power value with charge/discharge
// arrows, voltage and current row, and a footer with uptime, battery SoC
// and chip temperature. The panel only refreshes when the displayed text
// actually changes: fast partial updates normally, with a full refresh at
// boot and every FULL_REFRESH_MS to clear ghosting.
//
// Hardware: ESP32-C3 module + Waveshare Pico-ePaper-2.13 (SSD1680 class,
// 250x122) + INA226 current/voltage monitor on I2C. Temperature is the
// ESP32-C3 internal sensor.
//
// Display wiring (SPI + control):
//   EPD DIN  -> GPIO4   (MOSI)
//   EPD CLK  -> GPIO3   (SCK)
//   EPD CS   -> GPIO2
//   EPD DC   -> GPIO1
//   EPD RST  -> GPIO5
//   EPD BUSY -> GPIO6
//   VCC 3V3, GND common.
//
// INA226 wiring (I2C, orientation auto-probed at boot):
//   INA226 SDA -> GPIO10
//   INA226 SCL -> GPIO7
//   Address auto-detected at boot (0x40..0x4F depending on module straps).
//   Shunt: SHUNT_PARALLEL_N identical R100 resistors stacked in parallel.

#include <GxEPD2_BW.h>
#include <SPI.h>
#include <Wire.h>

// JetBrains Mono Bold — modern monospace — converted to GFXfont with
// Adafruit's fontconvert (see fonts/JetBrainsMono-Bold.ttf). Monospace keeps
// digits from shifting as values change. Custom fonts render from the
// BASELINE and ignore the background color.
#include "fonts/JetBrains32pt.h"
#include "fonts/JetBrains18pt.h"
#include "fonts/JetBrains11pt.h"  // charset 32..176 so it carries the degree sign
#define FONT_HERO JetBrainsMono_Bold32pt7b
#define FONT_VALUE JetBrainsMono_Bold18pt7b
#define FONT_FOOT JetBrainsMono_Bold11pt8b

// E-paper pin assignments
constexpr int8_t PIN_DIN = 4;   // MOSI
constexpr int8_t PIN_CLK = 3;   // SCK
constexpr int8_t PIN_CS = 2;
constexpr int8_t PIN_DC = 1;
constexpr int8_t PIN_RST = 5;
constexpr int8_t PIN_BUSY = 6;

// 2.13" 250x122 SSD1680-class panel with fast partial update. If a
// different panel revision stays blank here, try GxEPD2_213_B74 or
// GxEPD2_213_B73 as the driver class.
GxEPD2_BW<GxEPD2_213_BN, GxEPD2_213_BN::HEIGHT> epd(
    GxEPD2_213_BN(PIN_CS, PIN_DC, PIN_RST, PIN_BUSY));

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

// Refresh policy. E-paper updates are slow and visually busy, so readings
// are sampled every second and the panel redraws only when a displayed
// string changed, using fast partial updates. A full refresh runs at boot
// and again every FULL_REFRESH_MS of uptime (millis-based) to clear
// accumulated ghosting.
constexpr uint32_t SAMPLE_MS = 1000;
constexpr uint32_t FULL_REFRESH_MS = 10UL * 60UL * 1000UL;  // 10 minutes
uint32_t lastSample = 0;
uint32_t lastFullRefresh = 0;
bool firstDraw = true;
bool colonVisible = true;
char lastShown[64] = "";

// --- Battery model: 4S LiFePO4 pack (EVE IFR40135, 3.2 V 20 Ah 64 Wh/cell) ---
// Datasheet: charge CC/CV to 3.65 V/cell, end of discharge 2.5 V/cell.
// LiFePO4 sits on a very flat plateau between ~20% and ~90% (3.2-3.3 V/cell),
// so a voltage-based estimate is coarse mid-range. Separate charge/discharge
// tables compensate hysteresis and typical IR shift; values are per cell,
// linearly interpolated. Coulomb counting (INA226) will refine this later.
constexpr float PACK_CELLS = 4.0f;

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
// a fresh averaged reading every ~35 ms, well inside the sampling period.
constexpr uint16_t INA_CONFIG = 0x4527;

// UI development aid: when true and no INA226 answers, readings fall back
// to a simulated sweep instead of reporting ERROR. Enabled while the new
// display is being adjusted with the sensor disconnected — set back to
// false for deployment.
constexpr bool SIM_WHEN_ABSENT = true;

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

// Up or down triangle beside the hero value; filled when it is the active
// flow direction, outline otherwise.
constexpr int16_t ARROW_W = 16, ARROW_H = 14, ARROW_GAP = 12;
void drawArrow(int16_t x, int16_t y, bool up, bool filled) {
  int16_t yBase = up ? y + ARROW_H : y;
  int16_t yTip = up ? y : y + ARROW_H;
  if (filled)
    epd.fillTriangle(x + ARROW_W / 2, yTip, x, yBase, x + ARROW_W, yBase,
                     GxEPD_BLACK);
  else
    epd.drawTriangle(x + ARROW_W / 2, yTip, x, yBase, x + ARROW_W, yBase,
                     GxEPD_BLACK);
}

// Layout (landscape 250x122, black on white):
//   hero watts + charge (up) / discharge (down) arrows, baseline 48
//   voltage left / current right, baseline 92
//   uptime + SoC left, temperature right, baseline 119
void updateDisplay(float volts, float amps, float watts) {
  char hero[8], vTxt[8], aTxt[8], foot[24], tempTxt[12];

  // ERROR when the sensor stopped answering, OVLD when the shunt ADC
  // clips: in both cases the number it would show is not a measurement.
  bool fault = inaError || shuntOverload;
  if (inaError)
    strlcpy(hero, "ERROR", sizeof(hero));
  else if (shuntOverload)
    strlcpy(hero, "OVLD", sizeof(hero));
  else
    snprintf(hero, sizeof(hero), "%04.1fW", fabsf(watts));

  snprintf(vTxt, sizeof(vTxt), "%04.1fV", volts);
  snprintf(aTxt, sizeof(aTxt), "%04.1fA", fabsf(amps));

  // below 0.1 A the pack is effectively at rest: neither arrow is filled;
  // charging still selects the SoC table
  bool charging = amps < -0.01f;
  bool idle = fabsf(amps) < 0.1f;

  uint32_t secs = millis() / 1000;
  snprintf(foot, sizeof(foot), "%03lu:%02lu %3.0f%%",
           (unsigned long)(secs / 3600), (unsigned long)(secs / 60 % 60),
           batterySoc(volts, charging));

  // temperature rounded to 0.5 C so sensor jitter does not force a panel
  // refresh every sample
  snprintf(tempTxt, sizeof(tempTxt), "%04.1f\xB0" "C",
           roundf(temperatureRead() * 2.0f) / 2.0f);

  // the arrows are part of the drawn state too
  char dir = fault ? 'F' : (idle ? 'I' : (charging ? 'C' : 'D'));

  // e-paper is slow: skip the refresh entirely when nothing visible
  // changed, unless the periodic cleanse is due
  char shown[64];
  snprintf(shown, sizeof(shown), "%s|%s|%s|%s|%s|%c", hero, vTxt, aTxt,
           foot, tempTxt, dir);
  bool full = firstDraw || millis() - lastFullRefresh >= FULL_REFRESH_MS;
  if (!full && strcmp(shown, lastShown) == 0) return;
  strlcpy(lastShown, shown, sizeof(lastShown));
  firstDraw = false;

  if (full) {
    lastFullRefresh = millis();
    epd.setFullWindow();
  } else {
    epd.setPartialWindow(0, 0, epd.width(), epd.height());
  }

  // the clock colon blinks on every refresh actually performed — display
  // activity feedback, not a timebase — so it is toggled only after the
  // redraw decision and stays out of the change-detection string
  colonVisible = !colonVisible;
  if (!colonVisible) {
    char *colon = strchr(foot, ':');
    if (colon) *colon = ' ';
  }

  int16_t w = epd.width();
  epd.firstPage();
  do {
    epd.fillScreen(GxEPD_WHITE);
    epd.setTextColor(GxEPD_BLACK);
    int16_t bx, by;
    uint16_t bw, bh;

    // hero group (value + arrow column) centered as a unit
    epd.setFont(&FONT_HERO);
    epd.getTextBounds(hero, 0, 0, &bx, &by, &bw, &bh);
    int16_t group = (int16_t)bw + (fault ? 0 : ARROW_GAP + ARROW_W);
    int16_t hx = (w - group) / 2 - bx;
    epd.setCursor(hx, 48);
    epd.print(hero);
    if (!fault) {
      int16_t ax = hx + bx + (int16_t)bw + ARROW_GAP;
      drawArrow(ax, 9, true, !idle && charging);      // up = charge
      drawArrow(ax, 29, false, !idle && !charging);   // down = discharge
    }

    epd.setFont(&FONT_VALUE);
    epd.setCursor(8, 92);
    epd.print(vTxt);
    epd.getTextBounds(aTxt, 0, 0, &bx, &by, &bw, &bh);
    epd.setCursor(w - 8 - (int16_t)bw - bx, 92);
    epd.print(aTxt);

    epd.setFont(&FONT_FOOT);
    epd.setCursor(8, 119);
    epd.print(foot);
    epd.getTextBounds(tempTxt, 0, 0, &bx, &by, &bw, &bh);
    epd.setCursor(w - 8 - (int16_t)bw - bx, 119);
    epd.print(tempTxt);
  } while (epd.nextPage());
}

void setup() {
  Serial.begin(115200);

  SPI.begin(PIN_CLK, -1 /* no MISO */, PIN_DIN, PIN_CS);
  epd.init(115200);
  epd.setRotation(3);  // landscape, 250x122, flex cable on the right

  inaPresent = inaInit();
  if (!inaPresent) Serial.println("INA226 not found");

  Serial.println("powerMeterDC: display initialized");
}

void loop() {
  if (millis() - lastSample < SAMPLE_MS) return;
  lastSample = millis();

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

  updateDisplay(volts, amps, watts);

  if (inaPresent)
    Serial.printf("V=%.2f  I=%.2f  P=%.2f  [INA 0x%02X SDA=%d SCL=%d]\n",
                  volts, amps, watts, inaAddr, inaSdaPin, inaSclPin);
  else
    Serial.printf("V=%.2f  I=%.2f  P=%.2f  [scan: %s]\n", volts, amps, watts,
                  inaDiag);
}
