// ============================================================
//  User_Setup.h  —  TFT_eSPI config for DoggleEyes v3.0
//
//  !! THIS FILE MUST BE COPIED TO: !!
//  Arduino/libraries/TFT_eSPI/User_Setup.h
//  after every change. The sketch folder copy is the source of
//  truth; the library folder copy is what gets compiled.
//
//  !! IMPORTANT CHANGE FROM v1/v2 !!
//  The XIAO ESP32-S3 Sense camera daughterboard occupies
//  GPIO 11, 12, 13, 14, 15, 16, 17, 18, 38, 39, 40, 47, 48.
//  The display pins from v1/v2 conflicted with camera GPIO 11, 12.
//  All display pins have been moved to camera-safe GPIOs.
//
//  NEW pin assignments:
//    MOSI : GPIO 4   (was 11)
//    SCLK : GPIO 5   (was 12)
//    CS   : GPIO 1   (was 9)   — left eye only; right=GPIO2
//    DC   : GPIO 3   (was 7)
//    RST  : GPIO 6   (unchanged)
// ============================================================

// Sentinel — checked at compile time in DoggleEyes.ino to confirm
// this file (not a stale copy) is in the TFT_eSPI library folder.
#define DOGGLEEYES_SETUP_ID 42

#define GC9A01_DRIVER
#define TFT_WIDTH  240
#define TFT_HEIGHT 240

// ── Camera-safe SPI pins ─────────────────────────────────────
#define TFT_MOSI   4    // ← CHANGED (was 11, conflicts with camera Y8)
#define TFT_SCLK   5    // ← CHANGED (was 12, conflicts with camera Y7)
#define TFT_CS    -1    // Disabled — CS managed manually via selectDisplay()
#define TFT_DC     3    // ← CHANGED (was 7)
#define TFT_RST   -1    // Disabled — RST pulsed manually in setup() so both
                        // displays reset together before independent init

// Right eye CS = GPIO 2  (controlled manually in DoggleEyes.ino)

// ── Fonts ────────────────────────────────────────────────────
#define LOAD_GLCD
#define LOAD_FONT2
#define LOAD_FONT4

// ── SPI port — required for ESP32-S3 ─────────────────────────
// TFT_eSPI defaults to VSPI (port 3) which is not properly mapped
// on ESP32-S3 — register base resolves to 0, so any SPI register
// write at offset 0x10 crashes with StoreProhibited.
// USE_HSPI_PORT selects port 2 (HSPI), valid on both ESP32 and S3.
// GPIO matrix routes our pins (MOSI=4, SCLK=5) to whichever
// SPI host is selected — no IOMUX conflict.
#define USE_HSPI_PORT

// ── SPI speed ────────────────────────────────────────────────
// Note: camera uses its own parallel interface (not SPI),
// so there is no bus conflict. Display SPI runs independently.
// GPIO matrix routing (non-native pins) caps reliable SPI at ~20 MHz.
// Raise to 40 MHz only once displays are confirmed working.
#define SPI_FREQUENCY       20000000
#define SPI_READ_FREQUENCY   5000000
