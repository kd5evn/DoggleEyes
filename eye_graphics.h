// ============================================================
//  eye_graphics.h  v3.0  —  Eye drawing (camera edition)
//
//  Updated function signature passes all parameters explicitly
//  rather than reading from global state, making it safe to
//  call from Core 1 with a local snapshot of EyeState.
// ============================================================
#pragma once
#include <TFT_eSPI.h>

extern TFT_eSPI* tftPtr;

#define C_BLACK      0x0000
#define C_WHITE      0xFFFF
#define C_EYE_WHITE  0xEF5D
#define C_PUPIL      0x0861
#define C_SPECULAR   0xFFFF
#define C_HEART_RED  0xF800

#define CX  120
#define CY  120
#define CR  108

// Forward declarations
inline void drawAngryEye(float gx, float gy, float lidT, float eyeScale, float pupilScale, uint16_t irisOuter, uint16_t irisInner);
inline void drawClosedLids();
inline void drawHeartEye();

// ── Main draw entry point ────────────────────────────────────
inline void drawEye(float gx, float gy, bool isRight, float blinkPhase,
                    const char* mood, float eyeScale, float pupilScale,
                    uint16_t irisOuter, uint16_t irisInner) {

  float lidT = (blinkPhase <= 0.5f) ? (blinkPhase * 2.0f) : (2.0f - blinkPhase * 2.0f);
  lidT = constrain(lidT, 0.0f, 1.0f);

  tftPtr->fillScreen(C_BLACK);

  if (strcmp(mood, "closed") == 0) { drawClosedLids(); return; }
  if (strcmp(mood, "heart")  == 0) { drawHeartEye();   return; }
  if (strcmp(mood, "angry")  == 0) {
    drawAngryEye(gx, gy, lidT, eyeScale, pupilScale, irisOuter, irisInner);
    return;
  }

  // Sclera
  tftPtr->fillCircle(CX, CY, CR, C_EYE_WHITE);

  // Iris
  int irisR = (int)(44 * eyeScale);
  int ox = (int)(gx * CR * 0.75f);
  int oy = (int)(gy * CR * 0.75f);

  if (strcmp(mood, "derp") == 0) { ox = isRight ? -18 : 18; oy = 14; }

  int ix = CX + ox, iy = CY + oy;
  tftPtr->fillCircle(ix, iy, irisR, irisOuter);
  tftPtr->fillCircle(ix, iy, (int)(irisR * 0.72f), irisInner);

  int pupilR = (int)(22 * pupilScale * eyeScale);
  pupilR = constrain(pupilR, 8, 50);
  tftPtr->fillCircle(ix, iy, pupilR, C_PUPIL);
  tftPtr->fillCircle(ix + (int)(pupilR * 0.45f), iy - (int)(pupilR * 0.45f),
                 (int)(pupilR * 0.28f), C_SPECULAR);

  // Mood overlays
  if (strcmp(mood, "wide") == 0) {
    tftPtr->drawCircle(CX, CY, CR,     C_WHITE);
    tftPtr->drawCircle(CX, CY, CR - 1, C_EYE_WHITE);
  }
  if (strcmp(mood, "squint") == 0) {
    int squintY = CY;
    tftPtr->fillRect(0, squintY, 240, 240, C_BLACK);
    tftPtr->drawCircle(CX, CY, CR, C_BLACK);
  }
  if (strcmp(mood, "laser") == 0) {
    for (int i = 1; i <= 4; i++) {
      uint16_t glow = tftPtr->color565(200 - i * 30, 0, 0);
      tftPtr->drawCircle(ix, iy, pupilR + i * 5, glow);
    }
    tftPtr->drawCircle(CX, CY, CR, tftPtr->color565(180, 0, 0));
  }
  if (strcmp(mood, "sleepy") == 0) {
    int droopY = CY - (int)(CR * 0.35f);
    tftPtr->fillRect(0, 0, 240, droopY, C_BLACK);
  }

  // Blink lid
  if (lidT > 0.01f && strcmp(mood, "squint") != 0 && strcmp(mood, "sleepy") != 0) {
    int lidH = (int)(CR * 2 * lidT);
    tftPtr->fillRect(0, 0, 240, CY - CR + lidH, C_BLACK);
    tftPtr->drawFastHLine(0, CY - CR + lidH, 240, tftPtr->color565(40, 30, 20));
  }

  // Circular mask
  for (int r = CR + 1; r <= 120; r++) tftPtr->drawCircle(CX, CY, r, C_BLACK);
}

inline void drawAngryEye(float gx, float gy, float lidT, float eyeScale,
                          float pupilScale, uint16_t irisOuter, uint16_t irisInner) {
  tftPtr->fillCircle(CX, CY, CR, C_EYE_WHITE);
  int brow_y = CY - (int)(CR * 0.55f);
  tftPtr->fillTriangle(CX - CR, CY - CR, CX, brow_y, CX - CR, CY, C_BLACK);
  int irisR  = (int)(44 * eyeScale);
  int ox = (int)(gx * CR * 0.55f);
  int oy = (int)(gy * CR * 0.55f) + 8;
  tftPtr->fillCircle(CX + ox, CY + oy, irisR, irisOuter);
  tftPtr->fillCircle(CX + ox, CY + oy, (int)(irisR * 0.7f), irisInner);
  int pupilR = (int)(22 * pupilScale * eyeScale);
  pupilR = constrain(pupilR, 8, 50);
  tftPtr->fillCircle(CX + ox, CY + oy, pupilR, C_PUPIL);
  tftPtr->fillCircle(CX + ox + (int)(pupilR*0.4f), CY + oy - (int)(pupilR*0.4f),
                 (int)(pupilR*0.28f), C_SPECULAR);
  if (lidT > 0.01f) {
    int lidH = (int)(CR * 2 * lidT);
    tftPtr->fillRect(0, 0, 240, CY - CR + lidH, C_BLACK);
  }
  for (int r = CR + 1; r <= 120; r++) tftPtr->drawCircle(CX, CY, r, C_BLACK);
}

inline void drawClosedLids() {
  int lineY = CY, halfW = (int)(CR * 0.75f);
  for (int y = lineY - 12; y <= lineY; y++) {
    float frac = 1.0f - (float)(lineY - y) / 12.0f;
    int xOff = (int)(sqrt(1.0f - frac * frac) * halfW);
    tftPtr->drawFastHLine(CX - xOff, y, xOff * 2, tftPtr->color565(60, 50, 40));
  }
  for (int y = lineY; y <= lineY + 8; y++) {
    float frac = (float)(y - lineY) / 8.0f;
    int xOff = (int)(sqrt(1.0f - frac * frac) * halfW * 0.9f);
    tftPtr->drawFastHLine(CX - xOff, y, xOff * 2, tftPtr->color565(50, 40, 35));
  }
}

inline void drawHeartEye() {
  int hx = CX, hy = CY + 8, hr = 38;
  uint16_t heartCol = C_HEART_RED;
  uint16_t pinkCol  = tftPtr->color565(255, 120, 160);
  tftPtr->fillCircle(hx - (int)(hr*0.5f), hy - (int)(hr*0.3f), hr/2 + 2, heartCol);
  tftPtr->fillCircle(hx + (int)(hr*0.5f), hy - (int)(hr*0.3f), hr/2 + 2, heartCol);
  tftPtr->fillTriangle(hx - hr, hy - (int)(hr*0.2f), hx + hr, hy - (int)(hr*0.2f),
                   hx, hy + hr, heartCol);
  tftPtr->fillCircle(hx - (int)(hr*0.28f), hy - (int)(hr*0.18f),
                 (int)(hr*0.22f), pinkCol);
  for (int r = CR + 1; r <= 120; r++) tftPtr->drawCircle(CX, CY, r, C_BLACK);
}
