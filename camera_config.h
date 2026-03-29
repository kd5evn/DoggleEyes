// ============================================================
//  camera_config.h  —  OV2640/OV3660 pin config
//  Seeed Studio XIAO ESP32-S3 Sense
//
//  These pins are FIXED — wired on the Sense daughterboard.
//  Do NOT change them. They match the official Seeed Wiki
//  and are confirmed against the board schematic.
// ============================================================
#pragma once
#include "esp_camera.h"

// ── Vision processing constants ──────────────────────────────
#define VISION_W           160   // QQVGA width
#define VISION_H           120   // QQVGA height
#define VISION_INTERVAL_MS  66   // ~15fps vision (leaves headroom for rendering)
#define MOTION_THRESHOLD    18   // per-pixel diff to count as motion (0-255)
#define MOTION_MIN_PIXELS   80   // min pixels changed to register motion event

// ── XIAO ESP32-S3 Sense camera GPIOs ─────────────────────────
#define PWDN_GPIO_NUM    -1
#define RESET_GPIO_NUM   -1
#define XCLK_GPIO_NUM    10
#define SIOD_GPIO_NUM    40   // SDA (I2C for camera)
#define SIOC_GPIO_NUM    39   // SCL (I2C for camera)

#define Y9_GPIO_NUM      48
#define Y8_GPIO_NUM      11
#define Y7_GPIO_NUM      12
#define Y6_GPIO_NUM      14
#define Y5_GPIO_NUM      16
#define Y4_GPIO_NUM      18
#define Y3_GPIO_NUM      17
#define Y2_GPIO_NUM      15

#define VSYNC_GPIO_NUM   38
#define HREF_GPIO_NUM    47
#define PCLK_GPIO_NUM    13

// ── Build the esp_camera config struct ──────────────────────
inline camera_config_t buildCameraConfig() {
  camera_config_t config;

  config.ledc_channel = LEDC_CHANNEL_0;
  config.ledc_timer   = LEDC_TIMER_0;
  config.pin_d0       = Y2_GPIO_NUM;
  config.pin_d1       = Y3_GPIO_NUM;
  config.pin_d2       = Y4_GPIO_NUM;
  config.pin_d3       = Y5_GPIO_NUM;
  config.pin_d4       = Y6_GPIO_NUM;
  config.pin_d5       = Y7_GPIO_NUM;
  config.pin_d6       = Y8_GPIO_NUM;
  config.pin_d7       = Y9_GPIO_NUM;
  config.pin_xclk     = XCLK_GPIO_NUM;
  config.pin_pclk     = PCLK_GPIO_NUM;
  config.pin_vsync    = VSYNC_GPIO_NUM;
  config.pin_href     = HREF_GPIO_NUM;
  config.pin_sccb_sda = SIOD_GPIO_NUM;
  config.pin_sccb_scl = SIOC_GPIO_NUM;
  config.pin_pwdn     = PWDN_GPIO_NUM;
  config.pin_reset    = RESET_GPIO_NUM;

  config.xclk_freq_hz = 20000000;   // 20 MHz XCLK
  config.pixel_format = PIXFORMAT_GRAYSCALE;
  config.frame_size   = FRAMESIZE_QQVGA;   // 160x120 — fast + fits in DRAM
  config.jpeg_quality = 12;
  config.fb_count     = 2;          // double-buffer for smooth capture
  config.grab_mode    = CAMERA_GRAB_LATEST;

  // PSRAM: if available use it for frame buffers
  config.fb_location  = CAMERA_FB_IN_PSRAM;

  return config;
}
