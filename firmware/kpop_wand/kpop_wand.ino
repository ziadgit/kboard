// K-pop Wand firmware for the NUCODE NU40 DK (nRF52840)
// Spec: ../../KPOP_WAND_SPEC.md
//
// Hardware:
//   - NU40 DK, Arduino core nucode:nrf52 1.0.2, FQBN nucode:nrf52:nu40dk
//   - NU-IMU (ST LSM6DS3TR-C 6-axis IMU) on the Qwiic connector (I2C: SDA=P0.26, SCL=P0.27)
//   - 4 single-color onboard LEDs on P0.13..P0.16 (no RGB LED on this board, see "LED rendering")
//   - Optional LiPo on the BATT connector; voltage read on P0.02 through a 470K/1M divider
//
// BLE: Nordic UART Service via Bluefruit BLEUart, device name "KpopWand",
// newline-terminated ASCII lines (see spec for the full protocol).
//
// USB Serial is deliberately not used: the NUCODE core has known USB Serial link problems.

#include <Arduino.h>
#include <Wire.h>
#include <bluefruit.h>
#include <math.h>

// ---------------------------------------------------------------------------
// Configuration (tune on the real device)
// ---------------------------------------------------------------------------

// LED polarity. The variant says LED_STATE_ON = 1 (active high), but the board
// silkscreen/schematic say "negative logic / active low". If the LEDs look
// inverted (mostly on, blinking off), change this to LOW.
#define LED_ON_LEVEL LED_STATE_ON

// IMU mounting. "Wand frame": +X points out of the wand tip, +Z points out of the
// face of the wand that is up in the neutral pose (wand held horizontal, pointing
// forward). Remap here if the IMU is mounted differently in the wand.
#define WAND_AX (ax)
#define WAND_AY (ay)
#define WAND_AZ (az)
#define WAND_GX (gx)
#define WAND_GY (gy)
#define WAND_GZ (gz)
#define ROLL_SIGN 1.0f        // flip to -1.0f if TILT_LEFT and TILT_RIGHT are swapped
// The IMU sits rotated about the long axis inside the cardboard wand. This is the roll
// the wand reports in its neutral pose (measured 2026-11-07 via D:1 telemetry: roll 61,
// pitch -1). Readings are rotated by this so neutral = roll 0.
#define MOUNT_ROLL_DEG 61.0f

const float    SHAKE_G            = 2.5f;   // peak threshold (g)
const float    SHAKE_RELEASE_G    = 1.8f;   // a new peak counts only after dropping below this
const uint8_t  SHAKE_PEAKS        = 3;
const uint32_t SHAKE_WINDOW_MS    = 600;
const uint32_t SHAKING_MS         = 500;    // TWIST/WAVE are suppressed this long after a shake peak

const float    TILT_DEG           = 45.0f;
const float    TILT_MAX_DEG       = 95.0f;  // beyond this the wand is lying on its side, not tilted
                                            // (measured: real tilts 55-85 deg, resting on side ~105 deg)
const float    TILT_MAX_PITCH     = 45.0f;  // tilts only count with the wand roughly level
const uint32_t TILT_HOLD_MS       = 300;
const float    POINT_UP_DEG       = 70.0f;
const uint32_t POINT_UP_HOLD_MS   = 1000;
const float    NEUTRAL_ROLL_DEG   = 25.0f;  // orientation gestures re-arm after returning near neutral
const float    NEUTRAL_PITCH_DEG  = 40.0f;
const float    HOLD_STILL_DPS     = 90.0f;  // a pose only counts as "held" while rotating slower than this
const float    ACTIVE_DPS         = 150.0f; // rotation faster than this is "moving"...
const uint32_t ACTIVE_LOCKOUT_MS  = 600;    // ...and blocks pose gestures for this long afterwards

const float    TWIST_DPS          = 300.0f; // gyro about the long (X) axis
const uint8_t  TWIST_SAMPLES      = 2;      // consecutive samples over threshold
const float    TWIST_RELEASE_DPS  = 120.0f;
const float    TWISTING_DPS       = 250.0f; // WAVE is suppressed while rotating this fast about the long axis
const uint32_t TWISTING_MS        = 600;

// WAVE: the wand swung back and forth (lightstick arc), i.e. alternating rotation about an
// axis perpendicular to the long axis (Y or Z). Measured arcs: ~1.2 s per side, ~100-150 deg/s.
const float    WAVE_SWING_DPS     = 80.0f;  // each swing must pass +/- this (hysteresis)
const uint8_t  WAVE_HALF_SWINGS   = 3;      // 1.5 waves
const uint32_t WAVE_HALF_MIN_MS   = 150;
const uint32_t WAVE_HALF_MAX_MS   = 1800;

// After a motion gesture, that same gesture re-fires only once its motion has stopped
// for QUIET_MS (other gestures are not blocked).
const uint32_t QUIET_MS           = 400;

const uint32_t DEBOUNCE_MS        = 800;
const uint32_t BLINK_MS           = 80;     // local white blink on gesture
const uint32_t BATTERY_PERIOD_MS  = 30000;

// ---------------------------------------------------------------------------
// LSM6DS3TR-C driver (raw registers, no external library needed)
// ---------------------------------------------------------------------------

namespace imu {
const uint8_t REG_WHO_AM_I = 0x0F;
const uint8_t REG_CTRL1_XL = 0x10;
const uint8_t REG_CTRL2_G  = 0x11;
const uint8_t REG_CTRL3_C  = 0x12;
const uint8_t REG_STATUS   = 0x1E;
const uint8_t REG_OUTX_L_G = 0x22;  // gyro X/Y/Z then accel X/Y/Z, 12 bytes

const float ACCEL_G_PER_LSB   = 0.000244f;  // +-8 g
const float GYRO_DPS_PER_LSB  = 0.035f;     // +-1000 dps

uint8_t addr = 0;

bool readRegs(uint8_t reg, uint8_t *buf, uint8_t len) {
  Wire.beginTransmission(addr);
  Wire.write(reg);
  if (Wire.endTransmission(false) != 0) return false;
  if (Wire.requestFrom(addr, len) != len) return false;
  for (uint8_t i = 0; i < len; i++) buf[i] = Wire.read();
  return true;
}

bool writeReg(uint8_t reg, uint8_t val) {
  Wire.beginTransmission(addr);
  Wire.write(reg);
  Wire.write(val);
  return Wire.endTransmission() == 0;
}

// The LSM6DS3TR-C answers on 0x6B (SA0 high) or 0x6A (SA0 low). The board's
// BQ25186 charger can also sit on 0x6A, so identify the IMU by WHO_AM_I
// instead of trusting the address.
bool begin() {
  const uint8_t candidates[] = {0x6B, 0x6A};
  for (uint8_t a : candidates) {
    addr = a;
    uint8_t who = 0;
    if (readRegs(REG_WHO_AM_I, &who, 1) && (who == 0x6A || who == 0x69)) {
      writeReg(REG_CTRL3_C, 0x44);   // BDU=1, IF_INC=1
      writeReg(REG_CTRL1_XL, 0x4C);  // accel 104 Hz, +-8 g
      writeReg(REG_CTRL2_G, 0x48);   // gyro 104 Hz, +-1000 dps
      return true;
    }
  }
  addr = 0;
  return false;
}

// Returns true when a new sample was read. Accel in g, gyro in deg/s.
bool read(float &ax, float &ay, float &az, float &gx, float &gy, float &gz) {
  uint8_t status = 0;
  if (!readRegs(REG_STATUS, &status, 1) || !(status & 0x01)) return false;
  uint8_t b[12];
  if (!readRegs(REG_OUTX_L_G, b, 12)) return false;
  int16_t r[6];
  for (uint8_t i = 0; i < 6; i++) r[i] = (int16_t)(b[2 * i] | (b[2 * i + 1] << 8));
  gx = r[0] * GYRO_DPS_PER_LSB;
  gy = r[1] * GYRO_DPS_PER_LSB;
  gz = r[2] * GYRO_DPS_PER_LSB;
  ax = r[3] * ACCEL_G_PER_LSB;
  ay = r[4] * ACCEL_G_PER_LSB;
  az = r[5] * ACCEL_G_PER_LSB;
  return true;
}
}  // namespace imu

// ---------------------------------------------------------------------------
// LED rendering
// ---------------------------------------------------------------------------
// The NU40 DK has 4 single-color LEDs, so a hex color cannot be shown as a hue.
// Mapping: brightness = the color's max channel; the hue picks which of the 4
// LEDs is brightest (red/orange -> LED1, yellow/green -> LED2, cyan/blue -> LED3,
// purple/pink -> LED4). Low-saturation colors (white, cream) light all 4 evenly.
// This keeps groups distinguishable until an RGB LED is added.

const uint8_t LED_PINS[4] = {PIN_LED1, PIN_LED2, PIN_LED3, PIN_LED4};

enum LedMode { MODE_IDLE, MODE_TWO_COLOR, MODE_OFF };

struct Levels { float v[4]; };

LedMode  ledMode      = MODE_IDLE;
Levels   colorA       = {{0.3f, 0.3f, 0.3f, 0.3f}};  // default before the laptop sets a group
Levels   colorB       = {{0, 0, 0, 0}};
uint16_t pulseBpm     = 0;          // 0 = slow idle breathe
Levels   flashColor;
uint8_t  flashLeft    = 0;
uint32_t flashStart   = 0;
uint32_t blinkUntil   = 0;

Levels colorToLevels(uint32_t rgb) {
  float r = ((rgb >> 16) & 0xFF) / 255.0f;
  float g = ((rgb >> 8) & 0xFF) / 255.0f;
  float b = (rgb & 0xFF) / 255.0f;
  float mx = max(r, max(g, b));
  float mn = min(r, min(g, b));
  Levels out = {{0, 0, 0, 0}};
  if (mx <= 0.0f) return out;
  float sat = (mx - mn) / mx;
  if (sat < 0.25f) {
    for (int i = 0; i < 4; i++) out.v[i] = mx;
    return out;
  }
  float d = mx - mn, hue;
  if (mx == r)      hue = 60.0f * fmodf((g - b) / d, 6.0f);
  else if (mx == g) hue = 60.0f * ((b - r) / d + 2.0f);
  else              hue = 60.0f * ((r - g) / d + 4.0f);
  if (hue < 0) hue += 360.0f;
  int idx;
  if (hue < 45 || hue >= 330) idx = 0;  // red / orange
  else if (hue < 150)         idx = 1;  // yellow / green
  else if (hue < 255)         idx = 2;  // cyan / blue
  else                        idx = 3;  // purple / pink
  for (int i = 0; i < 4; i++) out.v[i] = (i == idx) ? mx : mx * 0.12f;
  return out;
}

void writeLed(uint8_t i, float level) {
  level = constrain(level, 0.0f, 1.0f);
  uint8_t pwm = (uint8_t)(level * level * 255.0f + 0.5f);  // rough gamma
  analogWrite(LED_PINS[i], LED_ON_LEVEL ? pwm : 255 - pwm);
}

void showLevels(const Levels &l, float scale) {
  for (uint8_t i = 0; i < 4; i++) writeLed(i, l.v[i] * scale);
}

void updateLeds(uint32_t now) {
  if ((int32_t)(blinkUntil - now) > 0) {        // gesture blink: all white, full
    Levels white = {{1, 1, 1, 1}};
    showLevels(white, 1.0f);
    return;
  }
  if (flashLeft > 0) {                           // F: flash N times, 150 ms on / 150 ms off
    uint32_t t = now - flashStart;
    uint32_t n = t / 300;
    if (n >= flashLeft) {
      flashLeft = 0;
    } else {
      showLevels(flashColor, (t % 300) < 150 ? 1.0f : 0.0f);
      return;
    }
  }
  if (ledMode == MODE_OFF) {
    showLevels(colorA, 0.0f);
    return;
  }
  float env;
  if (pulseBpm > 0) {                            // P: sharp pulse on each beat
    uint32_t period = 60000UL / pulseBpm;
    float phase = (float)(now % period) / period;
    env = 0.1f + 0.9f * expf(-6.0f * phase);
  } else {                                       // idle: slow 4 s breathe
    float phase = (float)(now % 4000) / 4000.0f;
    env = 0.15f + 0.85f * 0.5f * (1.0f - cosf(2.0f * PI * phase));
  }
  if (ledMode == MODE_TWO_COLOR) {               // C2: alternate every 600 ms
    showLevels(((now / 600) % 2) ? colorB : colorA, env);
  } else {
    showLevels(colorA, env);
  }
}

// ---------------------------------------------------------------------------
// BLE
// ---------------------------------------------------------------------------

BLEUart bleuart;
BLEDis   bledis;
bool     telemetryOn = false;   // D:1 / D:0 (debug: stream T: lines at 10 Hz)
char     rxLine[48];
uint8_t  rxLen = 0;

void sendLine(const char *line) {
  if (!Bluefruit.connected()) return;
  bleuart.print(line);
  bleuart.print('\n');
}

bool parseHex6(const char *s, uint32_t &out) {
  for (uint8_t i = 0; i < 6; i++) if (!isxdigit((unsigned char)s[i])) return false;
  if (s[6] != '\0' && s[6] != ':') return false;
  char tmp[7];
  memcpy(tmp, s, 6);
  tmp[6] = '\0';
  out = strtoul(tmp, nullptr, 16);
  return true;
}

void handleCommand(const char *cmd) {
  uint32_t a, b;
  if (strcmp(cmd, "OFF") == 0) {
    ledMode = MODE_OFF;
    flashLeft = 0;
  } else if (strncmp(cmd, "C2:", 3) == 0) {
    if (strlen(cmd) == 16 && parseHex6(cmd + 3, a) && cmd[9] == ':' && parseHex6(cmd + 10, b)) {
      colorA = colorToLevels(a);
      colorB = colorToLevels(b);
      ledMode = MODE_TWO_COLOR;
    }
  } else if (strncmp(cmd, "C:", 2) == 0) {
    if (strlen(cmd) == 8 && parseHex6(cmd + 2, a)) {
      colorA = colorToLevels(a);
      ledMode = MODE_IDLE;
    }
  } else if (strncmp(cmd, "F:", 2) == 0) {
    if (strlen(cmd) >= 10 && parseHex6(cmd + 2, a) && cmd[8] == ':') {
      long n = atol(cmd + 9);
      if (n > 0) {
        flashColor = colorToLevels(a);
        flashLeft = (uint8_t)min(n, 20L);
        flashStart = millis();
      }
    }
  } else if (strcmp(cmd, "D:1") == 0) {
    telemetryOn = true;
  } else if (strcmp(cmd, "D:0") == 0) {
    telemetryOn = false;
  } else if (strncmp(cmd, "P:", 2) == 0) {
    long bpm = atol(cmd + 2);
    pulseBpm = (uint16_t)constrain(bpm, 0L, 300L);
  }
  // Unknown lines are ignored.
}

void pollBle() {
  while (bleuart.available()) {
    char c = (char)bleuart.read();
    if (c == '\n' || c == '\r') {
      if (rxLen > 0) {
        rxLine[rxLen] = '\0';
        handleCommand(rxLine);
        rxLen = 0;
      }
    } else if (rxLen < sizeof(rxLine) - 1) {
      rxLine[rxLen++] = c;
    } else {
      rxLen = 0;  // overlong line: drop it
    }
  }
}

void onConnect(uint16_t) {
  flashColor = {{1, 1, 1, 1}};
  flashLeft = 2;
  flashStart = millis();
}

void onDisconnect(uint16_t, uint8_t) {
  rxLen = 0;
  pulseBpm = 0;
  telemetryOn = false;
}

void startAdvertising() {
  Bluefruit.Advertising.addFlags(BLE_GAP_ADV_FLAGS_LE_ONLY_GENERAL_DISC_MODE);
  Bluefruit.Advertising.addTxPower();
  Bluefruit.Advertising.addService(bleuart);
  Bluefruit.ScanResponse.addName();
  Bluefruit.Advertising.restartOnDisconnect(true);
  Bluefruit.Advertising.setInterval(32, 244);  // 20 ms fast, 152.5 ms slow (0.625 ms units)
  Bluefruit.Advertising.setFastTimeout(30);
  Bluefruit.Advertising.start(0);              // advertise forever
}

// ---------------------------------------------------------------------------
// Battery (B:<percent>)
// ---------------------------------------------------------------------------
// Schematic: VBAT -> 470K -> P0.02 -> 1M -> GND (through solder bridge SB14).
// Note: the variant's PIN_VBAT points at A7, which does not match the schematic.

const uint8_t BATTERY_PIN = 2;  // P0.02 / A0

int batteryPercent() {
  analogReference(AR_INTERNAL_3_0);
  analogReadResolution(12);
  analogSampleTime(40);  // high-impedance divider needs a long acquisition time
  uint32_t sum = 0;
  for (int i = 0; i < 8; i++) sum += analogRead(BATTERY_PIN);
  float vPin = (sum / 8.0f) * 3.0f / 4095.0f;
  float vBat = vPin * (1470.0f / 1000.0f);
  if (vBat < 2.5f) return -1;  // no battery, or SB14 open
  float pct = (vBat - 3.3f) / (4.2f - 3.3f) * 100.0f;
  return (int)constrain(pct, 0.0f, 100.0f);
}

// ---------------------------------------------------------------------------
// Gesture detection
// ---------------------------------------------------------------------------

uint32_t lastGestureMs = 0;
bool     anyGesture    = false;

// latest values, for T: telemetry
float    tRoll = 0, tPitch = 0, tGx = 0, tGy = 0, tGz = 0, tMag = 0;

uint32_t lastActiveMs  = 0;
uint32_t lastTwistingMs = 0;
bool     anyTwisting   = false;

// Per-gesture latch: after firing, the gesture waits until its motion has been quiet for QUIET_MS.
struct Latch {
  bool latched = false;
  uint32_t quietSince = 0;
  void set() { latched = true; quietSince = 0; }
  void update(bool quiet, uint32_t now) {
    if (!quiet) { quietSince = 0; return; }
    if (!quietSince) quietSince = now;
    if (latched && now - quietSince >= QUIET_MS) latched = false;
  }
};
Latch shakeLatch, twistLatch, waveLatch;

// shake
uint32_t shakePeaks[SHAKE_PEAKS];
uint8_t  shakeIdx = 0;
bool     inShakePeak = false;
uint32_t lastShakePeakMs = 0;
bool     anyShakePeak = false;

// orientation (low-passed gravity)
float    lpX = 0, lpY = 0, lpZ = 1;
uint32_t tiltLeftSince = 0, tiltRightSince = 0, pointUpSince = 0;
bool     orientArmed = true;

// twist
uint8_t  twistCount = 0;

// wave: Schmitt trigger on one swing axis; two instances (Y and Z)
struct WaveAxis {
  int8_t   lastSign = 0;
  uint32_t lastCrossMs = 0;
  uint32_t halfMs[WAVE_HALF_SWINGS];
  uint8_t  count = 0;
  void reset() { lastSign = 0; count = 0; }
  // returns waves per minute when a wave completes, else -1
  int update(float rate, uint32_t now) {
    int8_t cross = 0;
    if (rate > WAVE_SWING_DPS && lastSign != 1) cross = 1;
    else if (rate < -WAVE_SWING_DPS && lastSign != -1) cross = -1;
    if (cross == 0) {
      if (lastSign != 0 && now - lastCrossMs > WAVE_HALF_MAX_MS) reset();
      return -1;
    }
    if (lastSign != 0) {
      uint32_t half = now - lastCrossMs;
      if (half >= WAVE_HALF_MIN_MS && half <= WAVE_HALF_MAX_MS) {
        if (count < WAVE_HALF_SWINGS) halfMs[count++] = half;
      } else {
        count = 0;
      }
    }
    lastSign = cross;
    lastCrossMs = now;
    if (count < WAVE_HALF_SWINGS) return -1;
    uint32_t total = 0;
    for (uint8_t i = 0; i < WAVE_HALF_SWINGS; i++) total += halfMs[i];
    return (int)(60000.0f / (2.0f * total / WAVE_HALF_SWINGS) + 0.5f);
  }
};
WaveAxis waveY, waveZ;

void resetGestureTrackers() {
  memset(shakePeaks, 0, sizeof(shakePeaks));
  tiltLeftSince = tiltRightSince = pointUpSince = 0;
  twistCount = 0;
  waveY.reset();
  waveZ.reset();
}

void fireGesture(const char *name, uint32_t now, Latch *latch, int wpm = -1) {
  char line[24];
  if (wpm >= 0) snprintf(line, sizeof(line), "G:%s:%d", name, wpm);
  else          snprintf(line, sizeof(line), "G:%s", name);
  sendLine(line);
  blinkUntil = now + BLINK_MS;
  lastGestureMs = now;
  anyGesture = true;
  if (latch) latch->set();
  resetGestureTrackers();
}

void processSample(float ax, float ay, float az, float gx, float gy, float gz, uint32_t now) {
  // Undo the mounting rotation about the long (X) axis for both accel and gyro.
  static const float mc = cosf(MOUNT_ROLL_DEG * PI / 180.0f);
  static const float ms = sinf(MOUNT_ROLL_DEG * PI / 180.0f);
  const float wx = WAND_AX;
  const float wy = WAND_AY * mc - WAND_AZ * ms;
  const float wz = WAND_AZ * mc + WAND_AY * ms;
  const float rx = WAND_GX;
  const float ry = WAND_GY * mc - WAND_GZ * ms;
  const float rz = WAND_GZ * mc + WAND_GY * ms;

  // Orientation from low-passed accel.
  lpX += 0.2f * (wx - lpX);
  lpY += 0.2f * (wy - lpY);
  lpZ += 0.2f * (wz - lpZ);
  const float roll  = ROLL_SIGN * atan2f(lpY, lpZ) * 180.0f / PI;
  const float pitch = atan2f(lpX, sqrtf(lpY * lpY + lpZ * lpZ)) * 180.0f / PI;

  const float mag     = sqrtf(wx * wx + wy * wy + wz * wz);
  const float gyroMag = sqrtf(rx * rx + ry * ry + rz * rz);
  tRoll = roll; tPitch = pitch; tGx = rx; tGy = ry; tGz = rz; tMag = mag;

  const bool debouncing = anyGesture && (now - lastGestureMs < DEBOUNCE_MS);
  if (gyroMag > ACTIVE_DPS) lastActiveMs = now;

  shakeLatch.update(mag < SHAKE_RELEASE_G, now);
  twistLatch.update(fabsf(rx) < TWIST_RELEASE_DPS, now);
  waveLatch.update(fabsf(ry) < WAVE_SWING_DPS && fabsf(rz) < WAVE_SWING_DPS, now);

  // --- SHAKE: SHAKE_PEAKS peaks over SHAKE_G within SHAKE_WINDOW_MS
  if (!inShakePeak && mag > SHAKE_G) {
    inShakePeak = true;
    lastShakePeakMs = now;
    anyShakePeak = true;
    shakePeaks[shakeIdx] = now;
    shakeIdx = (shakeIdx + 1) % SHAKE_PEAKS;
    uint32_t oldest = shakePeaks[shakeIdx];  // oldest of the last SHAKE_PEAKS peaks
    if (!debouncing && !shakeLatch.latched && oldest != 0 && now - oldest <= SHAKE_WINDOW_MS) {
      fireGesture("SHAKE", now, &shakeLatch);
      return;
    }
  } else if (inShakePeak && mag < SHAKE_RELEASE_G) {
    inShakePeak = false;
  }
  const bool shaking = anyShakePeak && now - lastShakePeakMs < SHAKING_MS;
  if (fabsf(rx) > TWISTING_DPS) { lastTwistingMs = now; anyTwisting = true; }
  const bool twisting = anyTwisting && now - lastTwistingMs < TWISTING_MS;

  // --- TWIST: fast rotation about the long axis
  twistCount = (fabsf(rx) > TWIST_DPS) ? twistCount + 1 : 0;
  if (!debouncing && !shaking && !twistLatch.latched && twistCount >= TWIST_SAMPLES) {
    fireGesture("TWIST", now, &twistLatch);
    return;
  }

  // --- WAVE: back-and-forth swing about Y or Z (whichever the user swings around)
  int wpm = waveY.update(ry, now);
  int wpmZ = waveZ.update(rz, now);
  if (wpm < 0) wpm = wpmZ;
  if (shaking || twisting) { waveY.reset(); waveZ.reset(); }
  else if (wpm >= 0 && !debouncing && !waveLatch.latched) {
    fireGesture("WAVE", now, &waveLatch, wpm);
    return;
  }

  // --- Orientation gestures (held poses). A pose only counts while the wand is held
  // still, never right after fast motion, and fires once until the wand returns near neutral.
  if (fabsf(roll) < NEUTRAL_ROLL_DEG && pitch < NEUTRAL_PITCH_DEG) orientArmed = true;

  const bool still = gyroMag < HOLD_STILL_DPS && now - lastActiveMs >= ACTIVE_LOCKOUT_MS;
  const bool level = fabsf(pitch) < TILT_MAX_PITCH;
  const bool up    = still && pitch > POINT_UP_DEG;
  const bool left  = still && level && roll < -TILT_DEG && roll > -TILT_MAX_DEG;
  const bool right = still && level && roll >  TILT_DEG && roll <  TILT_MAX_DEG;
  pointUpSince   = up    ? (pointUpSince   ? pointUpSince   : now) : 0;
  tiltLeftSince  = left  ? (tiltLeftSince  ? tiltLeftSince  : now) : 0;
  tiltRightSince = right ? (tiltRightSince ? tiltRightSince : now) : 0;

  if (debouncing || !orientArmed) return;
  if (pointUpSince && now - pointUpSince >= POINT_UP_HOLD_MS) {
    orientArmed = false; fireGesture("POINT_UP", now, nullptr);
  } else if (tiltLeftSince && now - tiltLeftSince >= TILT_HOLD_MS) {
    orientArmed = false; fireGesture("TILT_LEFT", now, nullptr);
  } else if (tiltRightSince && now - tiltRightSince >= TILT_HOLD_MS) {
    orientArmed = false; fireGesture("TILT_RIGHT", now, nullptr);
  }
}

// ---------------------------------------------------------------------------
// Setup / loop
// ---------------------------------------------------------------------------

bool     imuOk          = false;
uint32_t lastImuRetryMs = 0;
uint32_t lastLedMs      = 0;
uint32_t lastBatteryMs  = 0;
uint32_t lastTelemetryMs = 0;

void setup() {
  for (uint8_t i = 0; i < 4; i++) {
    pinMode(LED_PINS[i], OUTPUT);
    digitalWrite(LED_PINS[i], !LED_ON_LEVEL);
  }

  Wire.begin();
  Wire.setClock(400000);
  imuOk = imu::begin();

  Bluefruit.begin();
  Bluefruit.setTxPower(4);
  Bluefruit.setName("KpopWand");
  Bluefruit.autoConnLed(false);  // we own the LEDs
  Bluefruit.Periph.setConnectCallback(onConnect);
  Bluefruit.Periph.setDisconnectCallback(onDisconnect);

  bledis.setManufacturer("NUCODE");
  bledis.setModel("NU40DK KpopWand");
  bledis.begin();
  bleuart.begin();

  startAdvertising();
}

void loop() {
  const uint32_t now = millis();

  pollBle();

  if (imuOk) {
    float ax, ay, az, gx, gy, gz;
    if (imu::read(ax, ay, az, gx, gy, gz)) processSample(ax, ay, az, gx, gy, gz, now);
  } else if (now - lastImuRetryMs > 2000) {
    lastImuRetryMs = now;            // IMU missing: retry (e.g. Qwiic cable plugged in later)
    imuOk = imu::begin();
    if (!imuOk) {                    // signal the problem: LED1 double-blink
      flashColor = {{1, 0, 0, 0}};
      flashLeft = 2;
      flashStart = now;
    }
  }

  if (now - lastLedMs >= 10) {
    lastLedMs = now;
    updateLeds(now);
  }

  if (telemetryOn && Bluefruit.connected() && now - lastTelemetryMs >= 100) {
    lastTelemetryMs = now;
    char line[48];
    if (imuOk) {
      // T:<roll deg>,<pitch deg>,<gyro long-axis dps>,<gyro Z dps>,<accel magnitude centi-g>,<gyro Y dps>
      snprintf(line, sizeof(line), "T:%d,%d,%d,%d,%d,%d", (int)lroundf(tRoll), (int)lroundf(tPitch),
               (int)lroundf(tGx), (int)lroundf(tGz), (int)lroundf(tMag * 100.0f), (int)lroundf(tGy));
    } else {
      snprintf(line, sizeof(line), "T:NOIMU");
    }
    sendLine(line);
  }

  if (Bluefruit.connected() && now - lastBatteryMs >= BATTERY_PERIOD_MS) {
    lastBatteryMs = now;
    int pct = batteryPercent();
    if (pct >= 0) {
      char line[16];
      snprintf(line, sizeof(line), "B:%d", pct);
      sendLine(line);
    }
  }

  delay(1);
}
