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
| Motor driver board (active-HIGH) | 1 | e.g. Amazon B0F9P6LSXY dual-channel |
| LiPo 3.7V 500mAh JST 1.25mm | 1 | Battery |
| TP4056 USB-C charger module (with DW01 protection) | 1 | Charger |
| Slide switch SS12D00 | 1 | Power on/off |

---

## Wiring — All Pin Assignments

### Displays (both GC9A01 share SPI bus, individual CS)

| Signal | XIAO Pin | XIAO GPIO | Display Pin |
|---|---|---|---|
| MOSI / SDA | D3 | GPIO 4 | SDA on both displays |
| SCLK / SCL | D4 | GPIO 5 | SCL on both displays |
| CS Left eye | D0 | GPIO 1 | CS on left GC9A01 only |
| CS Right eye | D1 | GPIO 2 | CS on right GC9A01 only |
| DC | D2 | GPIO 3 | DC on both displays |
| RST | D5 | GPIO 6 | RST on both displays |
| VCC | 3V3 | — | VCC + BL on both displays |
| GND | GND | — | GND on both displays |

> **Note:** GPIO 7, 8, and 9 (D8/D9/D10) are physically wired on the Sense
> expansion board to the microSD card slot and are **not available** as general
> GPIO outputs. GPIO 43/44 (D6/D7) are the correct free pins on the Sense.

### Haptic Motors (v3.1)

Motors are driven by an active-HIGH motor driver board (HIGH = motor ON, LOW = motor OFF).

| Signal | XIAO Pin | XIAO GPIO | Goes To |
|---|---|---|---|
| Left motor | D6 | GPIO 43 | Driver board IN1 |
| Right motor | D7 | GPIO 44 | Driver board IN2 |
| Motor power | 3V3 or 5V | — | Driver board VM |
| Ground | GND | — | Driver board GND |

### Camera

Internal to the XIAO ESP32-S3 Sense daughterboard.
**No wiring required.** GPIOs 10–18, 38–40, 47–48 are used internally.

---

## Arduino IDE Setup

1. **Install ESP32 core** via Boards Manager:
   URL: `https://raw.githubusercontent.com/espressif/arduino-esp32/gh-pages/package_esp32_index.json`
   Install: **esp32 by Espressif Systems**

2. **Board settings:**
   - Board: `Seeed XIAO ESP32S3`
   - PSRAM: **OPI PSRAM** ← required; this resets after board package updates — check it every time you update
   - Partition Scheme: **Huge APP** ← required for BLE + camera + TFT
   - Upload Speed: `921600`

3. **Install libraries** via Library Manager:
   - `TFT_eSPI` by Bodmer
   - `ArduinoJson` by Benoit Blanchon (version 7.x)
   - `NimBLE-Arduino` by h2zero
   - `esp_camera` — included in ESP32 Arduino core

4. **Copy User_Setup.h:**
   Replace the `User_Setup.h` inside your TFT_eSPI library folder with
   the `User_Setup.h` included in this package.

   Location: `Arduino/libraries/TFT_eSPI/User_Setup.h`

---

## Files in This Package

```
DoggleEyes/
├── DoggleEyes.ino              ← Main sketch — open this in Arduino IDE
├── camera_config.h             ← OV2640 GPIO definitions + frame buffer config
├── vision.h                    ← Motion centroid, brightness, face heuristic
├── eye_graphics.h              ← Eye drawing for GC9A01 round displays
├── haptic_vision.h             ← Coin motor driver (v3.1)
├── camera_stream.h             ← WiFi AP + MJPEG live camera stream
├── DoggleEyes_Controller_Haptic.html  ← BLE phone controller
User_Setup.h                    ← TFT_eSPI config — copy to library folder
README.md                       ← This file
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
  │  updateHaptic()                         │  BLE ping with hapticL/R
  │  BLE camera preview (40×30)             │  WiFi camera stream tick
  │  write EyeState + HapticState           │  read EyeState + HapticState
  └──────────── portMUX (eyeMux) ───────────┘
```

### Eye animation

Two GC9A01 240×240 round TFT displays show animated eyes with:
- Automatic blinking at configurable rate
- Gaze direction driven by camera motion centroid
- Pupil dilation based on scene brightness
- Moods: normal, wide, angry, squint, sleepy, laser, derp, heart, closed
- Both eyes mirror gaze correctly with `setRotation(2)` (180° physical mount)

### Camera vision

The OV2640 runs at 160×120 grayscale (QQVGA) at ~15fps on Core 0.
- **Motion centroid** — computes the weighted centre of changed pixels to drive gaze direction
- **Brightness** — average pixel luminance drives pupil dilation
- **Face proxy** — large bright oval in centre triggers "wide" eye mood
- **Camera warmup** — first 60 frames discarded to avoid false triggers at boot

### Haptic obstacle sensing

The camera frame is split down the middle into left and right halves.
`updateHaptic()` computes motion density in each half with proximity weighting
(bottom of frame = closer = weighted more heavily).

- **Motion below threshold** → motor OFF
- **Motion above threshold** → motor ON (binary, no PWM)
- **Left half motion** → left motor (GPIO 43 / D6)
- **Right half motion** → right motor (GPIO 44 / D7)
- Exponential smoothing (alpha = 0.35) prevents jitter
- Enable/disable and sensitivity tunable via BLE

### WiFi camera stream

Connect to the `DoggleEyes-CAM` WiFi AP (password: `doggles1`) and open
`http://192.168.4.1` for a live MJPEG preview at 320×240. Vision processing
pauses while a client is connected and resumes on disconnect.

### BLE camera preview

The BLE controller includes a diagnostic view showing the 40×30 grayscale
camera image at ~3fps with haptic zone overlay (vertical centre line +
orange intensity fill). Enable it with the Diagnostics button in the HTML controller.

---

## Phone Controller

Open `DoggleEyes_Controller_Haptic.html` in Chrome or Edge (desktop or Android).
On iPhone use the **Bluefy** app — Safari does not support Web Bluetooth.

Connect via the Connect button — the controller filters by NUS service UUID
so it finds DoggleEyes regardless of what other BLE devices are nearby.

The controller provides:
- **Mood selector** — normal, wide, angry, squint, sleepy, laser, derp, heart, closed
- **Gaze joystick** — manual gaze override
- **Eye/pupil scale** sliders
- **Iris colour** pickers (inner + outer)
- **Blink rate** slider + manual blink trigger
- **Vision toggles** — motion tracking, light sensing, face reaction
- **Haptic controls** — enable/disable toggle + sensitivity slider
- **Diagnostics** — live 40×30 camera preview with haptic zone overlay

---

## Serial Monitor Output (115200 baud)

```
[DoggleEyes v3.1 Haptic] Booting...
[Sprite] 240x240 framebuffer allocated in PSRAM.
[Init] Displays OK.
[Haptic] Motors initialised on GPIO 43/D6 (L) and GPIO 44/D7 (R) — digitalWrite mode.
[BLE] startAdvertising() = OK
[BLE] Advertising as 'DoggleEyes'
[Camera] Initialised OK — 160x120 grayscale.
[WiFi] AP 'DoggleEyes-CAM' started (password: doggles1)
[Stream] HTTP server ready on port 80.
[Vision] Task started on Core 0.
[Vision] Warmup complete — haptics and gaze now active.
[DoggleEyes v3.1] Ready — displays + haptic + camera + BLE.
[Haptic] rawL=0.000 rawR=0.000 smoothL=0.000 smoothR=0.000 motL=0 motR=0 threshold=0.040
```

---

## Troubleshooting

| Problem | Solution |
|---|---|
| `[Camera] Init FAILED` | Check PSRAM is set to **OPI PSRAM** in board settings — this resets after IDE/board updates |
| Camera fails after board package update | Re-select OPI PSRAM in Tools menu and re-flash |
| Display shows garbage | Verify User_Setup.h is the v3 version with GPIO 4/5/1/2/3/6 |
| Motors run continuously | Check active-HIGH/LOW logic of your driver board; verify correct motor driver pins |
| No motor vibration | Confirm wires on D6/D7 (not D8/D9); check driver board power supply |
| Motors won't stop running on GPIO 7/8 | Those are SD card bus pins on the Sense board — use D6/D7 (GPIO 43/44) instead |
| Eyes don't track motion | Toggle Motion Tracking on in controller; wipe camera lens; wait for warmup (~4s) |
| BLE not visible in nRF Connect on iOS | iOS 26+ has issues with nRF Connect; use Chrome Web Bluetooth on desktop or Bluefy on iPhone |
| Chrome can't find DoggleEyes | Controller filters by NUS service UUID — ensure you haven't changed the UUID |
| Camera stream blank | Connect to `DoggleEyes-CAM` WiFi first; stream pauses while BLE preview is active |
| Pupils never react to light | Point camera at a dark area; ensure Light Sensing is toggled on |
| Board overheats | Normal at full load — add a small heatsink to the back of the XIAO |

---

## Tuning Haptic Sensitivity

In `haptic_vision.h`:

```cpp
#define HAPTIC_MOTION_THRESHOLD   18    // pixel diff to count as motion (0-255)
                                        // lower = more sensitive to subtle motion
#define HAPTIC_MIN_DENSITY        0.04f // fraction of half-frame that must move
                                        // increase if motors trigger on camera noise
#define HAPTIC_PROXIMITY_WEIGHT   2.0f  // bottom-of-frame weight multiplier
                                        // higher = stronger response to close objects
#define HAPTIC_SMOOTH_ALPHA       0.35f // EMA smoothing factor
                                        // higher = faster response, more jitter
```

Sensitivity is also tunable at runtime via the BLE controller's haptic sensitivity slider
without reflashing.

---

*Built with love for a very good blind dog.*
