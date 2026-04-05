// ============================================================
//  vision.h  —  Camera vision processing
//
//  All functions operate on raw GRAYSCALE uint8_t frame buffers
//  at QQVGA resolution (160x120). Designed to run efficiently
//  on Core 0 at ~15fps without blocking eye rendering.
//
//  Functions:
//    computeBrightness()     — average luminance → pupil dilation
//    computeMotionCentroid() — frame diff centroid → gaze target
//    detectFaceProxy()       — fast skin/oval heuristic → react
// ============================================================
#pragma once
#include <stdint.h>
#include <math.h>
#include "camera_config.h"

// ── computeBrightness ────────────────────────────────────────
//  Returns 0.0 (very dark) → 1.0 (very bright)
//  Samples every 4th pixel for speed
inline float computeBrightness(const uint8_t* frame, int w, int h) {
  uint32_t sum = 0;
  int count = 0;
  for (int i = 0; i < w * h; i += 4) {
    sum += frame[i];
    count++;
  }
  return (count > 0) ? (float)sum / (count * 255.0f) : 0.5f;
}

// ── computeMotionCentroid ────────────────────────────────────
//  Computes the centroid of motion between current and previous
//  grayscale frames. Returns normalised gaze offset (-1..+1).
//
//  gx > 0 = motion on right side of frame
//  gy > 0 = motion in lower part of frame
//  hasMotion = true if enough pixels changed
inline void computeMotionCentroid(
    const uint8_t* cur,
    const uint8_t* prev,
    int w, int h,
    float& gx, float& gy, bool& hasMotion)
{
  long sumX = 0, sumY = 0, count = 0;

  // Sample every 2nd pixel row and column for speed
  for (int y = 0; y < h; y += 2) {
    for (int x = 0; x < w; x += 2) {
      int idx  = y * w + x;
      int diff = abs((int)cur[idx] - (int)prev[idx]);
      if (diff > MOTION_THRESHOLD) {
        sumX += x;
        sumY += y;
        count++;
      }
    }
  }

  if (count < MOTION_MIN_PIXELS / 4) {  // adjusted for 2x2 sampling
    hasMotion = false;
    gx = 0.0f; gy = 0.0f;
    return;
  }

  hasMotion = true;
  float cx = (float)sumX / count;   // centroid X (0..w)
  float cy = (float)sumY / count;   // centroid Y (0..h)

  // Normalise to -1..+1, centred on frame midpoint
  // Note: camera is mounted facing FORWARD on the dog's face
  // Left/right is natural; up/down may need to be inverted
  // depending on camera orientation. Flip gy sign if needed.
  gx =  ((cx / w) - 0.5f) * 2.0f;
  gy =  ((cy / h) - 0.5f) * 2.0f;

  // Clamp
  gx = gx < -1.0f ? -1.0f : (gx > 1.0f ? 1.0f : gx);
  gy = gy < -1.0f ? -1.0f : (gy > 1.0f ? 1.0f : gy);
}

// ── detectFaceProxy ──────────────────────────────────────────
//  Lightweight face heuristic — no ML required.
//  Looks for a large continuous region of mid-grey pixels
//  (skin tones in grayscale: ~100-200) in the upper-centre
//  portion of the frame. Returns true + relative size (0..1).
//
//  This is NOT real face detection — it's a pragmatic trigger
//  that works well enough for "person nearby = eyes react".
//  For real face detection use TFLite + Edge Impulse model.
inline bool detectFaceProxy(const uint8_t* frame, int w, int h,
                            float& faceSize) {
  // Only scan upper 60% of frame (faces tend to be upper half)
  int scanH = (h * 3) / 5;
  // Only scan centre 60% of frame width
  int xStart = w / 5;
  int xEnd   = w - w / 5;

  int skinCount    = 0;
  int regionPixels = scanH * (xEnd - xStart) / 4;  // sampled every 2x2

  for (int y = 0; y < scanH; y += 2) {
    for (int x = xStart; x < xEnd; x += 2) {
      uint8_t v = frame[y * w + x];
      // Skin tone in greyscale: mid-range, not too dark or bright.
      // Narrowed from 90-210 to 100-190 to reduce false positives
      // from walls, floors, and other mid-lit surfaces.
      if (v > 100 && v < 190) {
        skinCount++;
      }
    }
  }

  // Require at least 35% of region to be "skin-like" — raised from 20%
  // to reduce false positives in typical indoor environments.
  faceSize = (regionPixels > 0) ? (float)skinCount / regionPixels : 0.0f;
  return (faceSize > 0.35f);
}

// ── motionIntensity ──────────────────────────────────────────
//  Returns 0.0..1.0 intensity of motion (for future use,
//  e.g. varying eye expression based on activity level)
inline float motionIntensity(const uint8_t* cur, const uint8_t* prev,
                              int w, int h) {
  uint32_t totalDiff = 0;
  int count = w * h / 4;  // sample every 4 pixels
  for (int i = 0; i < w * h; i += 4) {
    totalDiff += abs((int)cur[i] - (int)prev[i]);
  }
  return (count > 0) ? min(1.0f, (float)totalDiff / (count * 60.0f)) : 0.0f;
}
