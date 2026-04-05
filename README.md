# DoggleVision
Dog Goggles, eyes, and haptic
# DoggleEyes Firmware v3.1 — Camera + Haptic Edition

Animated LCD eyes for a blind dog's goggles.
The camera sees the world. The motors tell the dog what's coming.

---

## Hardware Required

| Component | Qty | Notes |
|---|---|---|
| Seeed Studio XIAO ESP32-S3 **Sense** | 1 | Must be Sense variant — has OV2640 camera |
| GC9A01 1.28" Round TFT 240×240 | 2 | Left and right eye displays |
| Coin vibration motor 3V 10mm ERM | 2 | Left and right goggle frame |
| 2N2222 NPN transistor TO-92 | 2 | Motor driver |
| 1N4001 rectifier diode | 2 | Motor flyback protection |
| 100Ω resistor ¼W | 2 | Transistor base current limiter |
| LiPo 3.7V 500mAh JST 1.25mm | 1 | Battery |
| TP4056 USB-C charger module (with DW01 protection) | 1 | Charger |
| Slide switch SS12D00 | 1 | Power on/off |

---

## Wiring — All Pin Assignments

### Displays (both GC9A01 share SPI bus, individual CS)

| Signal | XIAO GPIO | Display Pin |
|---|---|---|
| MOSI / SDA | GPIO 4 | SDA on both displays |
| SCLK / SCL | GPIO 5 | SCL on both displays |
| CS Left eye | GPIO 1 | CS on left GC9A01 only |
| CS Right eye | GPIO 2 | CS on right GC9A01 only |
| DC | GPIO 3 | DC on both displays |
| RST | GPIO 6 | RST on both displays |
| VCC | 3V3 | VCC + BL on both displays |
| GND | GND | GND on both displays |

> **⚠ Note:** If upgrading from v1 or v2, these pins have changed.
> GPIO 11 and 12 are now used by the internal camera bus.

### Haptic Motors (v3.1 addition)

Each motor uses a transistor driver circuit:
`GPIO → 100Ω → 2N2222 Base → Collector to Motor M− → Motor M+ to 3V3 → Emitter to GND`
A 1N4001 flyback diode sits across each motor (cathode to M+).

| Signal | XIAO GPIO | Goes To |
|---|---|---|
| Left motor PWM | GPIO 7 | 100Ω → Q1 Base |
| Right motor PWM | GPIO 8 | 100Ω → Q2 Base |
| Motor power | 3V3 | Motor M+ (both) |
| Motor ground | GND | Q1/Q2 Emitter (both) |

### Camera
Internal to the XIAO ESP32-S3 Sense daughterboard.
**No wiring required.** GPIOs 10–18, 38–40, 47–48 are used internally.

---

## Arduino IDE Setup

1. **Install ESP32 core** via Boards Manager:
   URL: `https://raw.githubusercontent.com/espressif/arduino-esp32/gh-pages/package_esp32_index.json`
   Install: **esp32 by Espressif Systems** version 2.0 or later

2. **Board settings:**
   - Board: `Seeed XIAO ESP32S3`
   - PSRAM: **OPI PSRAM** ← required for camera frame buffers
   - Partition Scheme: **Huge APP** ← required for BLE + camera + TFT
   - Upload Speed: `921600`

3. **Install libraries** via Library Manager:
   - `TFT_eSPI` by Bodmer
   - `ArduinoJson` by Benoit Blanchon (version 7.x)
   - `NimBLE-Arduino` by h2zero
   - `esp_camera` — already included in ESP32 Arduino core ≥ 2.0

4. **Copy User_Setup.h:**
   Replace the `User_Setup.h` inside your TFT_eSPI library folder with
   the `User_Setup.h` included in this package. This sets the correct
   display pin numbers for the XIAO ESP32-S3 Sense.
   
   Location: `Arduino/libraries/TFT_eSPI/User_Setup.h`

---

## Files in This Package

```
DoggleEyes/
├── DoggleEyes.ino       ← Main sketch — open this in Arduino IDE
├── camera_config.h      ← OV2640 GPIO definitions for XIAO Sense
├── vision.h             ← Motion centroid, brightness, face heuristic
├── eye_graphics.h       ← Eye drawing for GC9A01 round displays
└── haptic_vision.h      ← ★ Coin motor PWM driver (v3.1)
User_Setup.h             ← TFT_eSPI config — copy to library folder
README.md                ← This file
```

---

## How It Works

### Dual-core architecture

```
Core 0  (vision + haptic, ~15fps)      Core 1  (display + BLE, 30fps)
  │                                         │
  │  esp_camera_fb_get()                    │  BLE RX → processCommand()
  │  computeBrightness()                    │  blink / wiggle animation
  │  computeMotionCentroid()                │  merge manual + vision gaze
  │  detectFaceProxy()                      │  drawEye() × 2 displays
  │  updateHaptic()  ← v3.1                 │  BLE ping with hapticL/R
  │  write EyeState + HapticState           │  read EyeState + HapticState
  └──────────── portMUX (eyeMux) ───────────┘
```

### Haptic obstacle sensing

The camera frame (160×120px) is split down the middle into left and right halves.
`updateHaptic()` in `haptic_vision.h` computes the density of motion/change in each
half, with pixels weighted more heavily toward the bottom of frame (closer to the dog).
The result drives GPIO 7 and GPIO 8 via LEDC PWM (200Hz, 8-bit, channels 4 and 5).

- **No obstacle** → PWM = 0, motors silent
- **Distant object** → low PWM (~60–120), gentle buzz
- **Close/large object** → high PWM (~200–230), strong buzz
- **Haptic enabled/disabled** via `{"hapticEnabled": true/false}` BLE command

---

## Phone Controller

Use `DoggleEyes_Controller_Haptic.html` (from the Haptic Add-on package).
Open in Chrome or Edge on Android, or use the **Bluefy** app on iPhone.

The controller shows:
- Live left/right motor intensity bars (from BLE telemetry)
- Haptic on/off toggle and sensitivity slider
- All camera vision toggles
- Mood selector, gaze joystick, iris colour, adjustments

---

## Serial Monitor Output (115200 baud)

```
[DoggleEyes v3.1 Haptic] Booting...
[Init] Displays OK.
[Haptic] Motors initialised on GPIO 7 (L) and GPIO 8 (R).
[Camera] Initialised OK — 160×120 grayscale.
[Vision] Task started on Core 0.
[BLE] Advertising as 'DoggleEyes'
[DoggleEyes v3.1] Ready — displays + haptic + camera + BLE.
```

---

## Troubleshooting

| Problem | Solution |
|---|---|
| `[Camera] Init FAILED` | Ensure PSRAM is set to OPI PSRAM in board settings; try power cycle |
| Display shows garbage | Verify User_Setup.h is the v3 version with GPIO 4/5/1/2/3/6 |
| Motors run continuously at full power | Transistor inserted backwards — check flat face orientation (EBC left→right) |
| No motor vibration at all | Check diode orientation; check GPIO 7/8 are free; run `hapticTest()` |
| Eyes don't track motion | Toggle Motion Tracking on in phone controller; wipe camera lens |
| Board overheats | Normal — add small heatsink to back of XIAO |
| Pupils never react to light | Point camera at dark area; ensure Light Sensing is toggled on |
| iPhone can't connect | Use Bluefy app (Web Bluetooth not in Safari) |

---

## Tuning Haptic Sensitivity

In `haptic_vision.h`:

```cpp
#define HAPTIC_MOTION_THRESHOLD   18    // pixel diff to count as motion (0-255)
#define HAPTIC_MIN_DENSITY        0.04f // % of frame that must move to trigger
#define HAPTIC_MIN_PWM            60    // minimum PWM to spin motor
#define HAPTIC_MAX_PWM            230   // maximum PWM cap
#define HAPTIC_PROXIMITY_WEIGHT   2.0f  // bottom-of-frame weight multiplier
#define HAPTIC_SMOOTH_ALPHA       0.35f // smoothing (higher = more responsive)
```

Increase `HAPTIC_MIN_DENSITY` if motors buzz too easily (e.g. from subtle lighting).
Decrease `HAPTIC_MOTION_THRESHOLD` if response feels slow to weak motion.

---

*Built with love for a very good blind dog.* 🐾
