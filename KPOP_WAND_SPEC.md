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
- Onboard IMU (accel + gyro) and onboard LEDs. TODO (Claude): confirm IMU part/I2C address and LED pins from the
  variant files in `~/Library/Arduino15/packages/nucode/hardware/nrf52/1.0.2/variants/`.
- Powered by battery pack.

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

## LED behavior
- Idle: breathe slowly in current group color.
- On gesture detected: brief white blink (local, immediate), then laptop sends color/flash.

## Data
`kpop_data.json` holds groups, fandom colors (hex), and songs with YouTube search queries. Colors are
approximations of official or fan-recognized colors.
