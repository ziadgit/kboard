# K-pop Wand: Project Spec (shared context for Claude Code and Grok Bot)

## Concept
A battery-powered K-pop lightstick that acts as a **gesture controller**. The wand detects motions with its
IMU (3-axis accelerometer + 3-axis gyroscope), sends gesture events over Bluetooth Low Energy (BLE) to a laptop,
and the laptop pulls up matching K-pop content (YouTube videos, news, group info). The wand's LEDs give feedback
(group color, flashes to confirm actions).

## Hardware
- Board: Nucode NU40DK (nRF52840, 64 MHz Cortex-M4F, BLE 5, native USB)
- Arduino core: `nucode:nrf52` 1.0.2 (fork of Adafruit nRF52 core; use Bluefruit BLE library: `#include <bluefruit.h>`)
- Board manager URL: https://raw.githubusercontent.com/Nucode01/Adafruit_nRF52_Arduino/refs/heads/master/package_nuduino_index.json
- FQBN: `nucode:nrf52:nu40dk`
- IMU: **not onboard.** External NU-IMU (ST LSM6DS3TR-C, 6-axis) on the Qwiic connector, I2C SDA=P0.26, SCL=P0.27.
  I2C address 0x6B or 0x6A; firmware identifies it by WHO_AM_I because the board's BQ25186 charger can also use 0x6A.
- LEDs: 4 **single-color** LEDs, LED1..LED4 = P0.13..P0.16 (Arduino pins 13..16). There is **no RGB LED**, so
  hex colors are approximated: brightness = max channel, hue picks the brightest LED (red/orange=LED1,
  yellow/green=LED2, cyan/blue=LED3, purple/pink=LED4), pale colors light all four. Adding an RGB LED/strip
  would allow true group colors without protocol changes.
- Buttons S1..S4 = P0.11, P0.12, P0.24, P0.25 (internal pull-up). Not used by the protocol yet.
- Power: LiPo on the BATT connector (USB also works). Battery voltage on P0.02 via 470K/1M divider (solder
  bridge SB14); `B:` is only sent when a plausible battery voltage is read.
- Orientation convention: neutral pose is the wand held horizontal, pointing forward, IMU face up.
  POINT_UP = tip raised > 70 deg. Holding the wand upright like a real lightstick reads as POINT_UP.

## Division of work
- **Claude Code**: firmware (`firmware/kpop_wand/kpop_wand.ino`): IMU reading, gesture detection, BLE service, LEDs.
- **Grok Bot**: laptop side (`laptop/`): BLE receiver script, gesture-to-action mapping, K-pop data (`kpop_data.json`).
- Both sides must follow the BLE protocol below exactly.

## Gestures (firmware detects, sends name)
| Gesture       | How to detect (suggested)                                   | Laptop action                              |
|---------------|-------------------------------------------------------------|--------------------------------------------|
| `SHAKE`       | accel magnitude spikes > ~2.5 g, 3+ times within 600 ms     | Play a random song from current group      |
| `TILT_LEFT`   | roll < -45 deg held ~300 ms                                 | Previous song                              |
| `TILT_RIGHT`  | roll > +45 deg held ~300 ms                                 | Next song                                  |
| `POINT_UP`    | pitch > +70 deg held ~1 s                                    | Fetch latest news/comeback for group       |
| `TWIST`       | gyro about long axis > ~300 deg/s                           | Switch to next group                       |
| `WAVE`        | periodic side-to-side motion; report tempo                  | Pick a song matching the wave energy       |

Tune thresholds on the real device. Debounce: ignore new gestures for ~800 ms after one fires.

## BLE protocol
Use Nordic UART Service (NUS) via Bluefruit `BLEUart` (service UUID `6E400001-B5A3-F393-E0A9-E50E24DCCA9E`).
Device name: `KpopWand`. Messages are newline-terminated ASCII lines.

Wand -> laptop:
- `G:<GESTURE>` e.g. `G:SHAKE`
- `G:WAVE:<wavesPerMinute>` e.g. `G:WAVE:120`
- `B:<batteryPercent>` (optional, every 30 s)

Laptop -> wand:
- `C:<RRGGBB>` set solid color, e.g. `C:A020F0`
- `C2:<RRGGBB>:<RRGGBB>` two-color alternating (groups with two colors)
- `F:<RRGGBB>:<count>` flash N times (action confirmation)
- `P:<bpm>` pulse current color at given beats per minute (0 = stop)
- `OFF`

Debug (optional, for tuning; normal laptop app can ignore `T:` lines):
- Laptop -> wand `D:1` / `D:0`: start/stop telemetry (auto-stops on disconnect).
- Wand -> laptop `T:<roll>,<pitch>,<gyroLong>,<gyroZ>,<accelMag_centi_g>` at 10 Hz (integers, deg, deg/s),
  or `T:NOIMU` if the IMU is not detected. `tools/wand_monitor.py` prints these.

## LED behavior
- Idle: breathe slowly in current group color.
- On gesture detected: brief white blink (local, immediate), then laptop sends color/flash.

## Data
`kpop_data.json` holds groups, fandom colors (hex), and songs with YouTube search queries. Colors are
approximations of official or fan-recognized colors.
