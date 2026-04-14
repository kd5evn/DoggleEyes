// ============================================================
//  eye_graphics.h  v3.0  —  Eye drawing (camera edition)
//
//  Updated function signature passes all parameters explicitly
//  rather than reading from global state, making it safe to
//  call from Core 1 with a local snapshot of EyeState.
// ============================================================
#pragma once
#include <TFT_eSPI.h>

extern TFT_eSPI*    tftPtr;   // display hardware — used only for pushSprite
extern TFT_eSprite* spr;      // framebuffer — all drawing goes here

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
inline void drawAngryEye(float gx, float gy, bool isRight, float lidT, float eyeScale, float pupilScale, uint16_t irisOuter, uint16_t irisInner);
inline void drawClosedLids();
inline void drawHeartEye();

// ── Main draw entry point ────────────────────────────────────
inline void drawEye(float gx, float gy, bool isRight, float blinkPhase,
                    const char* mood, float eyeScale, float pupilScale,
                    uint16_t irisOuter, uint16_t irisInner) {

  float lidT = (blinkPhase <= 0.5f) ? (blinkPhase * 2.0f) : (2.0f - blinkPhase * 2.0f);
  lidT = constrain(lidT, 0.0f, 1.0f);

  spr->fillScreen(C_BLACK);

  if (strcmp(mood, "closed") == 0) { drawClosedLids(); return; }
  if (strcmp(mood, "heart")  == 0) { drawHeartEye();   return; }
  if (strcmp(mood, "angry")  == 0) {
    drawAngryEye(gx, gy, isRight, lidT, eyeScale, pupilScale, irisOuter, irisInner);
    return;
  }

  // Sclera
  spr->fillCircle(CX, CY, CR, C_EYE_WHITE);

  // Iris
  int irisR = (int)(44 * eyeScale);
  int ox = (int)(gx * CR * 0.75f);
  int oy = (int)(gy * CR * 0.75f);

  if (strcmp(mood, "derp") == 0) { ox = isRight ? -18 : 18; oy = 14; }

  int ix = CX + ox, iy = CY + oy;
  spr->fillCircle(ix, iy, irisR, irisOuter);
  spr->fillCircle(ix, iy, (int)(irisR * 0.72f), irisInner);

  int pupilR = (int)(22 * pupilScale * eyeScale);
  pupilR = constrain(pupilR, 8, 50);
  spr->fillCircle(ix, iy, pupilR, C_PUPIL);
  spr->fillCircle(ix + (int)(pupilR * 0.45f), iy - (int)(pupilR * 0.45f),
                 (int)(pupilR * 0.28f), C_SPECULAR);

  // Mood overlays
  if (strcmp(mood, "wide") == 0) {
    spr->drawCircle(CX, CY, CR,     C_WHITE);
    spr->drawCircle(CX, CY, CR - 1, C_EYE_WHITE);
  }
  if (strcmp(mood, "squint") == 0) {
    int squintY = CY;
    spr->fillRect(0, squintY, 240, 240, C_BLACK);
    spr->drawCircle(CX, CY, CR, C_BLACK);
  }
  if (strcmp(mood, "laser") == 0) {
    for (int i = 1; i <= 4; i++) {
      uint16_t glow = spr->color565(200 - i * 30, 0, 0);
      spr->drawCircle(ix, iy, pupilR + i * 5, glow);
    }
    spr->drawCircle(CX, CY, CR, spr->color565(180, 0, 0));
  }
  if (strcmp(mood, "sleepy") == 0) {
    int droopY = CY - (int)(CR * 0.35f);
    spr->fillRect(0, 0, 240, droopY, C_BLACK);
  }

  // Blink lid
  if (lidT > 0.01f && strcmp(mood, "squint") != 0 && strcmp(mood, "sleepy") != 0) {
    int lidH = (int)(CR * 2 * lidT);
    spr->fillRect(0, 0, 240, CY - CR + lidH, C_BLACK);
    spr->drawFastHLine(0, CY - CR + lidH, 240, spr->color565(40, 30, 20));
  }
  // Note: no circular mask needed — fillScreen(BLACK) already blacks the corners,
  // and the GC9A01 is physically round so off-sclera pixels are never seen.
}

inline void drawAngryEye(float gx, float gy, bool isRight, float lidT, float eyeScale,
                          float pupilScale, uint16_t irisOuter, uint16_t irisInner) {
  spr->fillCircle(CX, CY, CR, C_EYE_WHITE);
  int brow_y = CY - (int)(CR * 0.55f);
  // Outside corner: with setRotation(2) X is physically flipped, so
  // outside (temple side) = CX+CR for left eye, CX-CR for right eye.
  int browX = isRight ? (CX - CR) : (CX + CR);
  spr->fillTriangle(browX, CY - CR, CX, brow_y, browX, CY, C_BLACK);
  int irisR  = (int)(44 * eyeScale);
  int ox = (int)(gx * CR * 0.55f);
  int oy = (int)(gy * CR * 0.55f) + 8;
  spr->fillCircle(CX + ox, CY + oy, irisR, irisOuter);
  spr->fillCircle(CX + ox, CY + oy, (int)(irisR * 0.7f), irisInner);
  int pupilR = (int)(22 * pupilScale * eyeScale);
  pupilR = constrain(pupilR, 8, 50);
  spr->fillCircle(CX + ox, CY + oy, pupilR, C_PUPIL);
  spr->fillCircle(CX + ox + (int)(pupilR*0.4f), CY + oy - (int)(pupilR*0.4f),
                 (int)(pupilR*0.28f), C_SPECULAR);
  if (lidT > 0.01f) {
    int lidH = (int)(CR * 2 * lidT);
    spr->fillRect(0, 0, 240, CY - CR + lidH, C_BLACK);
  }
}

inline void drawClosedLids() {
  int lineY = CY, halfW = (int)(CR * 0.75f);
  for (int y = lineY - 12; y <= lineY; y++) {
    float frac = 1.0f - (float)(lineY - y) / 12.0f;
    int xOff = (int)(sqrt(1.0f - frac * frac) * halfW);
    spr->drawFastHLine(CX - xOff, y, xOff * 2, spr->color565(60, 50, 40));
  }
  for (int y = lineY; y <= lineY + 8; y++) {
    float frac = (float)(y - lineY) / 8.0f;
    int xOff = (int)(sqrt(1.0f - frac * frac) * halfW * 0.9f);
    spr->drawFastHLine(CX - xOff, y, xOff * 2, spr->color565(50, 40, 35));
  }
}

inline void drawHeartEye() {
  int hx = CX, hy = CY + 8, hr = 38;
  uint16_t heartCol = C_HEART_RED;
  uint16_t pinkCol  = spr->color565(255, 120, 160);
  spr->fillCircle(hx - (int)(hr*0.5f), hy - (int)(hr*0.3f), hr/2 + 2, heartCol);
  spr->fillCircle(hx + (int)(hr*0.5f), hy - (int)(hr*0.3f), hr/2 + 2, heartCol);
  spr->fillTriangle(hx - hr, hy - (int)(hr*0.2f), hx + hr, hy - (int)(hr*0.2f),
                   hx, hy + hr, heartCol);
  spr->fillCircle(hx - (int)(hr*0.28f), hy - (int)(hr*0.18f),
                 (int)(hr*0.22f), pinkCol);
}
