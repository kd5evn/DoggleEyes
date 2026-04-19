// ============================================================
//  haptic_vision.h  —  Obstacle proximity haptic feedback
//  DoggleEyes v3.1  ·  ADD-ON to vision.h (no edits to vision.h)
//
//  Two coin vibration motors give the dog a directional "radar":
//    · Left motor  → obstacle detected in LEFT half of camera frame
//    · Right motor → obstacle detected in RIGHT half of camera frame
//    · Intensity   → binary on/off (motion above threshold = motor on)
//    · Pattern     → continuous buzz while obstacle remains in zone
//
//  GPIO assignments (camera-safe, display-safe):
//    Motor LEFT  : GPIO 7   (free on XIAO ESP32-S3 Sense)
//    Motor RIGHT : GPIO 8   (free on XIAO ESP32-S3 Sense)
//
//  Motor driver board is active-LOW:
//    IN=LOW  → motor ON
//    IN=HIGH → motor OFF
//
//  Integration: include this file in DoggleEyes.ino and call
//    hapticInit() in setup()
//    updateHaptic(frame, W, H) in visionTask() after each frame
// ============================================================
#pragma once
#include <stdint.h>
#include <math.h>
#include <Arduino.h>

// ── Pin assignments ──────────────────────────────────────────
#define MOTOR_LEFT_PIN    44   // GPIO 44 (D7) — coin motor, left side of goggles
#define MOTOR_RIGHT_PIN   43   // GPIO 43 (D6) — coin motor, right side of goggles

// ── Tuning constants ─────────────────────────────────────────
// Minimum % of a half-frame that must change to trigger vibration
#define HAPTIC_MOTION_THRESHOLD   18    // per-pixel diff to count (0-255)
#define HAPTIC_MIN_DENSITY        0.04f // 4% of half-frame pixels must move
// Proximity weighting: pixels in the BOTTOM of the frame are closer
// to the dog. We weight them more heavily so approaching objects
// ramp up the haptic before lateral objects do.
#define HAPTIC_PROXIMITY_WEIGHT   2.0f  // bottom row gets this multiplier vs top

// ── Smoothing ────────────────────────────────────────────────
// Exponential moving average applied to raw intensity each frame
#define HAPTIC_SMOOTH_ALPHA       0.35f

// ── Shared state (written by visionTask on Core 0) ───────────
struct HapticState {
  float leftIntensity  = 0.0f;   // 0.0 – 1.0
  float rightIntensity = 0.0f;
  bool  enabled        = false;
  uint8_t leftPWM      = 0;
  uint8_t rightPWM     = 0;
  float minDensity     = HAPTIC_MIN_DENSITY;  // tunable via BLE hapticSensitivity command
};

// Declare extern — defined in DoggleEyes.ino
extern portMUX_TYPE eyeMux;
extern HapticState hapticState;

// ── hapticWrite ──────────────────────────────────────────────
//  duty > 0 → motor ON  (drive pin LOW  — active-low board)
//  duty = 0 → motor OFF (drive pin HIGH — active-low board)
inline void hapticWrite(int pin, uint8_t duty) {
  digitalWrite(pin, duty > 0 ? HIGH : LOW);
}

// ── hapticInit ───────────────────────────────────────────────
inline void hapticInit() {
  pinMode(MOTOR_LEFT_PIN,  OUTPUT);
  pinMode(MOTOR_RIGHT_PIN, OUTPUT);
  digitalWrite(MOTOR_LEFT_PIN,  LOW);   // LOW = OFF for active-high board
  digitalWrite(MOTOR_RIGHT_PIN, LOW);
  Serial.println("[Haptic] Motors initialised on GPIO 44/D7 (L) and GPIO 43/D6 (R) — digitalWrite mode.");
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
//  smooths it, and drives the motor pins directly.
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
    digitalWrite(MOTOR_LEFT_PIN,  LOW);   // OFF
    digitalWrite(MOTOR_RIGHT_PIN, LOW);   // OFF
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

  // Binary on/off: motor fires when smoothed intensity is above noise floor
  uint8_t pwmL = (smoothL > 0.01f) ? 1 : 0;
  uint8_t pwmR = (smoothR > 0.01f) ? 1 : 0;

  // Debug — print density every ~2s (every 30 frames at 15fps)
  static int dbgCount = 0;
  if (++dbgCount >= 30) {
    Serial.printf("[Haptic] rawL=%.3f rawR=%.3f smoothL=%.3f smoothR=%.3f motL=%d motR=%d threshold=%.3f\n",
                  rawLeft, rawRight, smoothL, smoothR, pwmL, pwmR, minDensity);
    dbgCount = 0;
  }

  hapticWrite(MOTOR_LEFT_PIN,  pwmL);
  hapticWrite(MOTOR_RIGHT_PIN, pwmR);

  // Store final state for BLE telemetry (use 0 or 255 for on/off display)
  portENTER_CRITICAL(&eyeMux);
  hapticState.leftPWM  = pwmL ? 200 : 0;
  hapticState.rightPWM = pwmR ? 200 : 0;
  portEXIT_CRITICAL(&eyeMux);
}

// ── hapticSetEnabled ─────────────────────────────────────────
//  Toggle haptics from BLE command (call from Core 1)
inline void hapticSetEnabled(bool en) {
  portENTER_CRITICAL(&eyeMux);
  hapticState.enabled = en;
  portEXIT_CRITICAL(&eyeMux);
  if (!en) {
    digitalWrite(MOTOR_LEFT_PIN,  LOW);   // OFF
    digitalWrite(MOTOR_RIGHT_PIN, LOW);   // OFF
  }
}

// ── hapticTest ───────────────────────────────────────────────
//  Single 500ms pulse per motor — call manually to verify wiring.
//  Not called from setup() during normal operation.
inline void hapticTest() {
  Serial.println("[Haptic] Testing LEFT motor...");
  digitalWrite(MOTOR_LEFT_PIN, HIGH); delay(500); digitalWrite(MOTOR_LEFT_PIN, LOW);
  delay(200);
  Serial.println("[Haptic] Testing RIGHT motor...");
  digitalWrite(MOTOR_RIGHT_PIN, HIGH); delay(500); digitalWrite(MOTOR_RIGHT_PIN, LOW);
  Serial.println("[Haptic] Motor test complete.");
}
