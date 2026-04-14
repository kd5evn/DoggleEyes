// ============================================================
//  haptic_vision.h  —  Obstacle proximity haptic feedback
//  DoggleEyes v3.1  ·  ADD-ON to vision.h (no edits to vision.h)
//
//  Two coin vibration motors give the dog a directional "radar":
//    · Left motor  → obstacle detected in LEFT half of camera frame
//    · Right motor → obstacle detected in RIGHT half of camera frame
//    · Intensity   → 0-255 PWM, proportional to how large/close
//                    the obstacle is (fills more of the frame)
//    · Pattern     → continuous buzz, not a single pulse, so the
//                    dog gets sustained directional feedback while
//                    an obstacle remains in that zone
//
//  GPIO assignments (camera-safe, display-safe):
//    Motor LEFT  PWM : GPIO 7   (free on XIAO ESP32-S3 Sense)
//    Motor RIGHT PWM : GPIO 8   (free on XIAO ESP32-S3 Sense)
//
//  Transistor circuit per motor (see wiring diagram):
//    GPIO → 100Ω → NPN base (2N2222)
//    NPN collector → Motor (-) lead
//    Motor (+) lead → 3.3V
//    Flyback diode across motor terminals (cathode to +)
//    NPN emitter → GND
//
//  LEDC (ESP32 PWM) channels:
//    Channel 4 → GPIO 7  (motor left)   [channels 0-3 used by camera XCLK]
//    Channel 5 → GPIO 8  (motor right)
//
//  Integration: include this file in DoggleEyes.ino and call
//    hapticInit() in setup()
//    updateHaptic(frame, W, H) in visionTask() after each frame
// ============================================================
#pragma once
#include <stdint.h>
#include <math.h>
#include "driver/ledc.h"

// ── Pin assignments ──────────────────────────────────────────
#define MOTOR_LEFT_PIN    7    // GPIO 7  — coin motor, left side of goggles
#define MOTOR_RIGHT_PIN   8    // GPIO 8  — coin motor, right side of goggles

// ── LEDC config ──────────────────────────────────────────────
#define MOTOR_LEDC_FREQ   200          // Hz — low freq = stronger feel
#define MOTOR_LEDC_RES    LEDC_TIMER_8_BIT   // 8-bit = 0-255
#define MOTOR_LEFT_CH     LEDC_CHANNEL_4
#define MOTOR_RIGHT_CH    LEDC_CHANNEL_5
#define MOTOR_LEDC_TIMER  LEDC_TIMER_2       // separate from camera

// ── Tuning constants ─────────────────────────────────────────
// Minimum % of a half-frame that must change to trigger vibration
#define HAPTIC_MOTION_THRESHOLD   18    // per-pixel diff to count (0-255)
#define HAPTIC_MIN_DENSITY        0.04f // 4% of half-frame pixels must move
#define HAPTIC_MIN_PWM            60    // below this the motor won't spin
#define HAPTIC_MAX_PWM            230   // cap — don't run motor at full blast
// Proximity weighting: pixels in the BOTTOM of the frame are closer
// to the dog. We weight them more heavily so approaching objects
// ramp up the haptic before lateral objects do.
#define HAPTIC_PROXIMITY_WEIGHT   2.0f  // bottom row gets this multiplier vs top

// ── Smoothing ────────────────────────────────────────────────
// Exponential moving average applied to raw intensity each frame
// Higher alpha = more responsive but jittery
// Lower alpha = smoother but slower to react
#define HAPTIC_SMOOTH_ALPHA       0.35f

// ── Shared state (written by visionTask on Core 0) ───────────
struct HapticState {
  float leftIntensity  = 0.0f;   // 0.0 – 1.0
  float rightIntensity = 0.0f;
  bool  enabled        = true;
  uint8_t leftPWM      = 0;
  uint8_t rightPWM     = 0;
  float minDensity     = HAPTIC_MIN_DENSITY;  // tunable via BLE hapticSensitivity command
};

// Declare extern — defined in DoggleEyes.ino
extern portMUX_TYPE eyeMux;
extern HapticState hapticState;

// ── hapticInit ───────────────────────────────────────────────
//  Call once in setup() after Serial.begin()
inline void hapticInit() {
  // Configure LEDC timer
  ledc_timer_config_t timer_conf = {};
  timer_conf.speed_mode      = LEDC_LOW_SPEED_MODE;
  timer_conf.duty_resolution  = MOTOR_LEDC_RES;
  timer_conf.timer_num        = MOTOR_LEDC_TIMER;
  timer_conf.freq_hz          = MOTOR_LEDC_FREQ;
  timer_conf.clk_cfg          = LEDC_AUTO_CLK;
  ledc_timer_config(&timer_conf);

  // Configure left motor channel
  ledc_channel_config_t ch_left = {};
  ch_left.gpio_num   = MOTOR_LEFT_PIN;
  ch_left.speed_mode = LEDC_LOW_SPEED_MODE;
  ch_left.channel    = MOTOR_LEFT_CH;
  ch_left.timer_sel  = MOTOR_LEDC_TIMER;
  ch_left.duty       = 0;
  ch_left.hpoint     = 0;
  ledc_channel_config(&ch_left);

  // Configure right motor channel
  ledc_channel_config_t ch_right = {};
  ch_right.gpio_num   = MOTOR_RIGHT_PIN;
  ch_right.speed_mode = LEDC_LOW_SPEED_MODE;
  ch_right.channel    = MOTOR_RIGHT_CH;
  ch_right.timer_sel  = MOTOR_LEDC_TIMER;
  ch_right.duty       = 0;
  ch_right.hpoint     = 0;
  ledc_channel_config(&ch_right);

  // Start both motors at 0
  ledc_set_duty(LEDC_LOW_SPEED_MODE, MOTOR_LEFT_CH,  0);
  ledc_update_duty(LEDC_LOW_SPEED_MODE, MOTOR_LEFT_CH);
  ledc_set_duty(LEDC_LOW_SPEED_MODE, MOTOR_RIGHT_CH, 0);
  ledc_update_duty(LEDC_LOW_SPEED_MODE, MOTOR_RIGHT_CH);

  Serial.println("[Haptic] Motors initialised on GPIO 7 (L) and GPIO 8 (R).");
}

// ── computeHalfFrameDensity ──────────────────────────────────
//  Measures the weighted motion density in one horizontal half
//  of the frame. Returns 0.0 (no motion) → 1.0 (fully active).
//
//  xStart, xEnd : pixel column range for this half
//  Proximity weighting: rows near the bottom (large Y) score higher
//  because objects filling the bottom of frame are physically closer.
inline float computeHalfFrameDensity(
    const uint8_t* cur,
    const uint8_t* prev,
    int w, int h,
    int xStart, int xEnd)
{
  float weightedSum  = 0.0f;
  float totalWeight  = 0.0f;

  // Sample every 2nd pixel in both axes for speed
  for (int y = 0; y < h; y += 2) {
    // Proximity weight: linearly ramps from 1.0 at top to
    // HAPTIC_PROXIMITY_WEIGHT at the bottom of the frame
    float proximityW = 1.0f + (HAPTIC_PROXIMITY_WEIGHT - 1.0f) * ((float)y / h);

    for (int x = xStart; x < xEnd; x += 2) {
      int idx  = y * w + x;
      int diff = abs((int)cur[idx] - (int)prev[idx]);

      totalWeight += proximityW;
      if (diff > HAPTIC_MOTION_THRESHOLD) {
        weightedSum += proximityW;
      }
    }
  }

  if (totalWeight < 1.0f) return 0.0f;
  return weightedSum / totalWeight;
}

// ── updateHaptic ─────────────────────────────────────────────
//  Call this in visionTask() after computing motion.
//  Reads current + previous frames, computes per-side intensity,
//  smooths it, and drives the LEDC PWM outputs directly.
//
//  prev : previous frame buffer (same pointer used in vision.h)
inline void updateHaptic(
    const uint8_t* cur,
    const uint8_t* prev,
    int w, int h)
{
  // Check if haptics are enabled and read current sensitivity (under mutex)
  bool  enabled;
  float minDensity;
  portENTER_CRITICAL(&eyeMux);
  enabled    = hapticState.enabled;
  minDensity = hapticState.minDensity;
  portEXIT_CRITICAL(&eyeMux);

  if (!enabled) {
    ledc_set_duty(LEDC_LOW_SPEED_MODE, MOTOR_LEFT_CH,  0);
    ledc_update_duty(LEDC_LOW_SPEED_MODE, MOTOR_LEFT_CH);
    ledc_set_duty(LEDC_LOW_SPEED_MODE, MOTOR_RIGHT_CH, 0);
    ledc_update_duty(LEDC_LOW_SPEED_MODE, MOTOR_RIGHT_CH);
    return;
  }

  int midX = w / 2;

  // Raw density per side (0.0 – 1.0)
  float rawLeft  = computeHalfFrameDensity(cur, prev, w, h, 0,    midX);
  float rawRight = computeHalfFrameDensity(cur, prev, w, h, midX, w);

  // Apply minimum density gate — ignore noise below threshold (BLE-tunable)
  if (rawLeft  < minDensity) rawLeft  = 0.0f;
  if (rawRight < minDensity) rawRight = 0.0f;

  // Exponential smoothing (running average with previous intensity)
  portENTER_CRITICAL(&eyeMux);
  float smoothL = hapticState.leftIntensity  * (1.0f - HAPTIC_SMOOTH_ALPHA)
                  + rawLeft  * HAPTIC_SMOOTH_ALPHA;
  float smoothR = hapticState.rightIntensity * (1.0f - HAPTIC_SMOOTH_ALPHA)
                  + rawRight * HAPTIC_SMOOTH_ALPHA;
  hapticState.leftIntensity  = smoothL;
  hapticState.rightIntensity = smoothR;
  portEXIT_CRITICAL(&eyeMux);

  // Map 0.0-1.0 → PWM range, with minimum spin threshold
  auto toPWM = [](float v) -> uint8_t {
    if (v < 0.01f) return 0;
    int pwm = (int)(HAPTIC_MIN_PWM + v * (HAPTIC_MAX_PWM - HAPTIC_MIN_PWM));
    if (pwm > HAPTIC_MAX_PWM) pwm = HAPTIC_MAX_PWM;
    return (uint8_t)pwm;
  };

  uint8_t pwmL = toPWM(smoothL);
  uint8_t pwmR = toPWM(smoothR);

  // Debug — print density + PWM every ~2s (every 30 frames at 15fps)
  static int dbgCount = 0;
  if (++dbgCount >= 30) {
    Serial.printf("[Haptic] rawL=%.3f rawR=%.3f pwmL=%d pwmR=%d threshold=%.3f\n",
                  rawLeft, rawRight, pwmL, pwmR, minDensity);
    dbgCount = 0;
  }

  // Write to LEDC hardware (direct, no FreeRTOS call needed from Core 0)
  ledc_set_duty(LEDC_LOW_SPEED_MODE, MOTOR_LEFT_CH,  pwmL);
  ledc_update_duty(LEDC_LOW_SPEED_MODE, MOTOR_LEFT_CH);
  ledc_set_duty(LEDC_LOW_SPEED_MODE, MOTOR_RIGHT_CH, pwmR);
  ledc_update_duty(LEDC_LOW_SPEED_MODE, MOTOR_RIGHT_CH);

  // Store final PWM for BLE telemetry
  portENTER_CRITICAL(&eyeMux);
  hapticState.leftPWM  = pwmL;
  hapticState.rightPWM = pwmR;
  portEXIT_CRITICAL(&eyeMux);
}

// ── hapticSetEnabled ─────────────────────────────────────────
//  Toggle haptics from BLE command (call from Core 1)
inline void hapticSetEnabled(bool en) {
  portENTER_CRITICAL(&eyeMux);
  hapticState.enabled = en;
  portEXIT_CRITICAL(&eyeMux);
  if (!en) {
    ledc_set_duty(LEDC_LOW_SPEED_MODE, MOTOR_LEFT_CH,  0);
    ledc_update_duty(LEDC_LOW_SPEED_MODE, MOTOR_LEFT_CH);
    ledc_set_duty(LEDC_LOW_SPEED_MODE, MOTOR_RIGHT_CH, 0);
    ledc_update_duty(LEDC_LOW_SPEED_MODE, MOTOR_RIGHT_CH);
  }
}

// ── hapticTest ───────────────────────────────────────────────
//  One-shot test: pulses left then right motor.
//  Phase 1 — plain digitalWrite (no PWM) to verify GPIO + transistor.
//  Phase 2 — LEDC PWM to verify PWM path.
inline void hapticTest() {
  // ── Phase 1: raw GPIO HIGH — motor should spin at full speed ──
  Serial.println("[Haptic] Phase 1 — GPIO HIGH test (no PWM)");
  Serial.println("[Haptic] Testing LEFT motor (GPIO HIGH)...");
  pinMode(MOTOR_LEFT_PIN,  OUTPUT);
  pinMode(MOTOR_RIGHT_PIN, OUTPUT);
  digitalWrite(MOTOR_LEFT_PIN, HIGH);
  delay(1000);
  digitalWrite(MOTOR_LEFT_PIN, LOW);
  delay(200);

  Serial.println("[Haptic] Testing RIGHT motor (GPIO HIGH)...");
  digitalWrite(MOTOR_RIGHT_PIN, HIGH);
  delay(1000);
  digitalWrite(MOTOR_RIGHT_PIN, LOW);
  delay(200);

  // ── Phase 2: LEDC PWM at ~63% duty ───────────────────────────
  Serial.println("[Haptic] Phase 2 — LEDC PWM test");
  Serial.println("[Haptic] Testing LEFT motor (PWM)...");
  ledc_set_duty(LEDC_LOW_SPEED_MODE, MOTOR_LEFT_CH, 160);
  ledc_update_duty(LEDC_LOW_SPEED_MODE, MOTOR_LEFT_CH);
  delay(1000);
  ledc_set_duty(LEDC_LOW_SPEED_MODE, MOTOR_LEFT_CH, 0);
  ledc_update_duty(LEDC_LOW_SPEED_MODE, MOTOR_LEFT_CH);
  delay(200);

  Serial.println("[Haptic] Testing RIGHT motor (PWM)...");
  ledc_set_duty(LEDC_LOW_SPEED_MODE, MOTOR_RIGHT_CH, 160);
  ledc_update_duty(LEDC_LOW_SPEED_MODE, MOTOR_RIGHT_CH);
  delay(1000);
  ledc_set_duty(LEDC_LOW_SPEED_MODE, MOTOR_RIGHT_CH, 0);
  ledc_update_duty(LEDC_LOW_SPEED_MODE, MOTOR_RIGHT_CH);

  Serial.println("[Haptic] Motor test complete.");
}
