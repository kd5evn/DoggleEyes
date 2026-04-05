// ============================================================
//  DoggleEyes Firmware  v3.1  —  Camera + Haptic Edition
//  Seeed Studio XIAO ESP32-S3 Sense  +  OV2640 camera
//  Dual GC9A01 Round TFT (240×240)  +  BLE  +  Haptic Motors
//
//  v3.1 fix: replaced global String mood with char mood[16] to
//  avoid StoreProhibited panic (String heap alloc at global
//  init time crashes on ESP32-S3 before Arduino runtime ready).
//  TFT_eSPI object moved to pointer, initialised inside setup().
//
//  Pin assignments (XIAO ESP32-S3 Sense — camera-safe):
//    Camera       : GPIOs 10-18, 38-40, 47-48 (internal)
//    Display MOSI : GPIO 4
//    Display SCLK : GPIO 5
//    Display CS L : GPIO 1
//    Display CS R : GPIO 2
//    Display DC   : GPIO 3
//    Display RST  : GPIO 6
//    Motor LEFT   : GPIO 7   — LEDC ch4, 200Hz PWM
//    Motor RIGHT  : GPIO 8   — LEDC ch5, 200Hz PWM
//
//  Arduino IDE board settings:
//    Board           : Seeed XIAO ESP32S3
//    PSRAM           : OPI PSRAM  (REQUIRED)
//    Partition scheme: Huge APP
//    Upload speed    : 921600
// ============================================================

#include <Arduino.h>
#include <NimBLEDevice.h>
#include <ArduinoJson.h>
#include <TFT_eSPI.h>
#include <SPI.h>
#include "esp_camera.h"
#include "camera_config.h"
#include "vision.h"
#include "eye_graphics.h"
#include "haptic_vision.h"

// ── BLE UUIDs (Nordic UART Service) ──────────────────────────
#define NUS_SERVICE_UUID  "6E400001-B5A3-F393-E0A9-E50E24DCCA9E"
#define NUS_RX_UUID       "6E400002-B5A3-F393-E0A9-E50E24DCCA9E"
#define NUS_TX_UUID       "6E400003-B5A3-F393-E0A9-E50E24DCCA9E"

// ── Display CS pins ───────────────────────────────────────────
#define CS_LEFT   1
#define CS_RIGHT  2

// ── TFT object — pointer so constructor runs inside setup() ──
// Global TFT_eSPI construction causes StoreProhibited on ESP32-S3
TFT_eSPI* tftPtr = nullptr;

// ── BLE globals ───────────────────────────────────────────────
NimBLEServer*         pServer = nullptr;
NimBLECharacteristic* pTxChar = nullptr;
NimBLECharacteristic* pRxChar = nullptr;
bool deviceConnected = false;

// ── Mutex (plain — no constructor, safe as global) ───────────
portMUX_TYPE eyeMux = portMUX_INITIALIZER_UNLOCKED;

// ── HapticState (POD struct — safe as global) ────────────────
HapticState hapticState;

// ── Eye state ─────────────────────────────────────────────────
// mood uses char[16] instead of String — avoids heap alloc at
// global init time which causes StoreProhibited on ESP32-S3.
struct EyeState {
  char     mood[16];
  float    manualGazeX    = 0.0f;
  float    manualGazeY    = 0.0f;
  uint16_t irisOuter      = 0x6204;
  uint16_t irisInner      = 0xA145;
  float    eyeScale       = 1.0f;
  float    pupilScale     = 1.0f;
  bool     autoBlink      = true;
  float    blinkRate      = 4.0f;
  bool     wiggle         = false;
  bool     mirror         = true;

  bool     cameraActive   = false;
  bool     motionEnabled  = true;
  bool     lightEnabled   = true;
  bool     faceEnabled    = true;

  float    visionGazeX    = 0.0f;
  float    visionGazeY    = 0.0f;
  float    sceneBrightness = 1.0f;
  bool     motionDetected  = false;
  bool     faceDetected    = false;
  float    faceSize        = 0.0f;

  float    blinkPhase      = 0.0f;
  bool     blinking        = false;
  unsigned long lastBlink  = 0;
  float    wiggleOffX      = 0.0f;
  float    wiggleOffY      = 0.0f;
  unsigned long lastWiggle = 0;
  unsigned long lastAlert  = 0;
  bool     alertActive     = false;
} state;

// Helper — set mood safely
inline void setMood(const char* m) {
  strncpy(state.mood, m, sizeof(state.mood) - 1);
  state.mood[sizeof(state.mood) - 1] = '\0';
}

const int FRAME_MS = 1000 / 30;

// ─────────────────────────────────────────────────────────────
//  BLE CALLBACKS
// ─────────────────────────────────────────────────────────────
class ServerCallbacks : public NimBLEServerCallbacks {
  void onConnect(NimBLEServer* s, NimBLEConnInfo& connInfo) override {
    deviceConnected = true;
    Serial.println("[BLE] Phone connected.");
    if (pTxChar) { pTxChar->setValue("{\"connected\":true}\n"); pTxChar->notify(); }
  }
  void onDisconnect(NimBLEServer* s, NimBLEConnInfo& connInfo, int reason) override {
    deviceConnected = false;
    Serial.println("[BLE] Disconnected — restarting advertising.");
    NimBLEDevice::startAdvertising();
  }
};

void processCommand(const char* json) {
  StaticJsonDocument<512> doc;
  if (deserializeJson(doc, json)) return;

  portENTER_CRITICAL(&eyeMux);
  if (doc.containsKey("mood"))          { strncpy(state.mood, doc["mood"].as<const char*>(), sizeof(state.mood) - 1); state.mood[sizeof(state.mood) - 1] = '\0'; }
  if (doc.containsKey("gazeX"))          state.manualGazeX   = doc["gazeX"].as<float>();
  if (doc.containsKey("gazeY"))          state.manualGazeY   = doc["gazeY"].as<float>();
  if (doc.containsKey("eyeScale"))       state.eyeScale      = doc["eyeScale"].as<float>();
  if (doc.containsKey("pupilScale"))     state.pupilScale    = doc["pupilScale"].as<float>();
  if (doc.containsKey("blinkRate"))      state.blinkRate     = doc["blinkRate"].as<float>();
  if (doc.containsKey("autoBlink"))      state.autoBlink     = doc["autoBlink"].as<bool>();
  if (doc.containsKey("wiggle"))         state.wiggle        = doc["wiggle"].as<bool>();
  if (doc.containsKey("mirror"))         state.mirror        = doc["mirror"].as<bool>();
  if (doc.containsKey("motionEnabled"))  state.motionEnabled = doc["motionEnabled"].as<bool>();
  if (doc.containsKey("lightEnabled"))   state.lightEnabled  = doc["lightEnabled"].as<bool>();
  if (doc.containsKey("faceEnabled"))    state.faceEnabled   = doc["faceEnabled"].as<bool>();
  if (doc.containsKey("hapticEnabled"))     hapticState.enabled    = doc["hapticEnabled"].as<bool>();
  if (doc.containsKey("hapticSensitivity")) hapticState.minDensity = constrain(doc["hapticSensitivity"].as<float>(), 0.005f, 0.30f);

  if (doc.containsKey("irisOuter")) {
    state.irisOuter = tftPtr->color565((uint8_t)doc["irisOuter"]["r"],
                                       (uint8_t)doc["irisOuter"]["g"],
                                       (uint8_t)doc["irisOuter"]["b"]);
  }
  if (doc.containsKey("irisInner")) {
    state.irisInner = tftPtr->color565((uint8_t)doc["irisInner"]["r"],
                                       (uint8_t)doc["irisInner"]["g"],
                                       (uint8_t)doc["irisInner"]["b"]);
  }
  if (doc.containsKey("action")) {
    const char* a = doc["action"].as<const char*>();
    if (strcmp(a,"blink")==0)  { state.blinking = true; state.blinkPhase = 0.0f; }
    if (strcmp(a,"dilate")==0) { state.pupilScale = 1.6f; }
    if (strcmp(a,"alert")==0)  { state.alertActive = true; state.lastAlert = millis(); }
  }
  portEXIT_CRITICAL(&eyeMux);

  if (pTxChar && deviceConnected) {
    pTxChar->setValue("{\"ok\":true}\n");
    pTxChar->notify();
  }
}

class RxCallbacks : public NimBLECharacteristicCallbacks {
  void onWrite(NimBLECharacteristic* pChar, NimBLEConnInfo& connInfo) override {
    std::string val = pChar->getValue();
    static char rxBuf[1024];
    static int  rxBufLen = 0;

    int inLen = (int)val.size();
    if (rxBufLen + inLen >= (int)sizeof(rxBuf) - 1) rxBufLen = 0;
    memcpy(rxBuf + rxBufLen, val.c_str(), inLen);
    rxBufLen += inLen;
    rxBuf[rxBufLen] = '\0';

    int start = 0, depth = 0;
    for (int i = 0; i < rxBufLen; i++) {
      if (rxBuf[i] == '{') depth++;
      if (rxBuf[i] == '}' && --depth == 0) {
        char tmp[512];
        int len = i + 1 - start;
        if (len < (int)sizeof(tmp)) {
          memcpy(tmp, rxBuf + start, len);
          tmp[len] = '\0';
          processCommand(tmp);
        }
        start = i + 1;
      }
    }
    int remaining = rxBufLen - start;
    if (remaining > 0) memmove(rxBuf, rxBuf + start, remaining);
    rxBufLen = remaining;
    rxBuf[rxBufLen] = '\0';
  }
};

// ─────────────────────────────────────────────────────────────
//  VISION TASK  (Core 0)
// ─────────────────────────────────────────────────────────────
void visionTask(void* param) {
  Serial.println("[Vision] Task started on Core 0.");
  const int W = VISION_W;
  const int H = VISION_H;

  uint8_t* prevFrame = (uint8_t*)ps_malloc(W * H);
  if (!prevFrame) {
    Serial.println("[Vision] ERROR: Cannot allocate prev frame — need PSRAM.");
    vTaskDelete(NULL);
  }
  memset(prevFrame, 128, W * H);

  for (;;) {
    camera_fb_t* fb = esp_camera_fb_get();
    if (!fb) { vTaskDelay(pdMS_TO_TICKS(50)); continue; }

    if (fb->format != PIXFORMAT_GRAYSCALE || fb->width != W || fb->height != H) {
      esp_camera_fb_return(fb);
      vTaskDelay(pdMS_TO_TICKS(50));
      continue;
    }

    uint8_t* cur = fb->buf;

    float brightness = computeBrightness(cur, W, H);

    float motionX = 0.0f, motionY = 0.0f;
    bool  hasMotion = false;
    computeMotionCentroid(cur, prevFrame, W, H, motionX, motionY, hasMotion);

    float faceSize = 0.0f;
    bool  hasFace  = detectFaceProxy(cur, W, H, faceSize);

    portENTER_CRITICAL(&eyeMux);
    state.sceneBrightness = brightness;
    state.motionDetected  = hasMotion;
    if (hasMotion && state.motionEnabled) {
      state.visionGazeX = state.visionGazeX * 0.6f + motionX * 0.4f;
      state.visionGazeY = state.visionGazeY * 0.6f + motionY * 0.4f;
    } else {
      state.visionGazeX *= 0.92f;
      state.visionGazeY *= 0.92f;
    }
    state.faceDetected = hasFace;
    state.faceSize     = faceSize;
    portEXIT_CRITICAL(&eyeMux);

    updateHaptic(cur, prevFrame, W, H);

    memcpy(prevFrame, cur, W * H);
    esp_camera_fb_return(fb);
    vTaskDelay(pdMS_TO_TICKS(VISION_INTERVAL_MS));
  }
}

// ─────────────────────────────────────────────────────────────
//  DISPLAY HELPERS
// ─────────────────────────────────────────────────────────────
void selectDisplay(bool right) {
  digitalWrite(right ? CS_LEFT  : CS_RIGHT, HIGH);
  digitalWrite(right ? CS_RIGHT : CS_LEFT,  LOW);
}

// ─────────────────────────────────────────────────────────────
//  SETUP
// ─────────────────────────────────────────────────────────────
void setup() {
  Serial.begin(115200);
  delay(200);   // brief settle before first print
  Serial.println("\n[DoggleEyes v3.1 Haptic] Booting...");

  // Initialise mood — must be done here, not at global scope
  setMood("normal");

  // ── TFT — create on heap inside setup() ───────────────
  tftPtr = new TFT_eSPI();

  // ── Displays ──────────────────────────────────────────
  pinMode(CS_LEFT,  OUTPUT); digitalWrite(CS_LEFT,  HIGH);
  pinMode(CS_RIGHT, OUTPUT); digitalWrite(CS_RIGHT, HIGH);

  selectDisplay(true);
  tftPtr->init(); tftPtr->setRotation(0); tftPtr->fillScreen(TFT_BLACK);
  selectDisplay(false);
  tftPtr->init(); tftPtr->setRotation(0); tftPtr->fillScreen(TFT_BLACK);
  Serial.println("[Init] Displays OK.");

  // ── Haptic motors ──────────────────────────────────────
  hapticInit();
  // hapticTest(); // uncomment to test motors at boot

  // ── Camera ────────────────────────────────────────────
  camera_config_t camCfg = buildCameraConfig();
  esp_err_t err = esp_camera_init(&camCfg);
  if (err != ESP_OK) {
    Serial.printf("[Camera] Init FAILED: 0x%x — running without vision.\n", err);
    portENTER_CRITICAL(&eyeMux);
    state.cameraActive = false;
    portEXIT_CRITICAL(&eyeMux);
  } else {
    sensor_t* s = esp_camera_sensor_get();
    s->set_framesize(s, FRAMESIZE_QQVGA);
    s->set_pixformat(s, PIXFORMAT_GRAYSCALE);
    s->set_gainceiling(s, (gainceiling_t)6);
    s->set_whitebal(s, 1);
    s->set_awb_gain(s, 1);
    portENTER_CRITICAL(&eyeMux);
    state.cameraActive = true;
    portEXIT_CRITICAL(&eyeMux);
    Serial.println("[Camera] Initialised OK — 160x120 grayscale.");
    xTaskCreatePinnedToCore(visionTask, "VisionTask", 8192, NULL, 1, NULL, 0);
  }

  // ── BLE ───────────────────────────────────────────────
  NimBLEDevice::init("DoggleEyes");
  NimBLEDevice::setPower(ESP_PWR_LVL_P9);
  pServer = NimBLEDevice::createServer();
  pServer->setCallbacks(new ServerCallbacks());
  NimBLEService* pSvc = pServer->createService(NUS_SERVICE_UUID);
  pTxChar = pSvc->createCharacteristic(NUS_TX_UUID, NIMBLE_PROPERTY::NOTIFY);
  pRxChar = pSvc->createCharacteristic(NUS_RX_UUID,
              NIMBLE_PROPERTY::WRITE | NIMBLE_PROPERTY::WRITE_NR);
  pRxChar->setCallbacks(new RxCallbacks());
  pSvc->start();
  NimBLEAdvertising* pAdv = NimBLEDevice::getAdvertising();
  pAdv->addServiceUUID(NUS_SERVICE_UUID);
  pAdv->enableScanResponse(true);
  NimBLEDevice::startAdvertising();
  Serial.println("[BLE] Advertising as 'DoggleEyes'");
  Serial.println("[DoggleEyes v3.1] Ready — displays + haptic + camera + BLE.");
}

// ─────────────────────────────────────────────────────────────
//  MAIN LOOP  (Core 1)
// ─────────────────────────────────────────────────────────────
void loop() {
  static unsigned long lastFrame = 0;
  unsigned long now = millis();
  if (now - lastFrame < (unsigned long)FRAME_MS) return;
  lastFrame = now;
  float dt = FRAME_MS / 1000.0f;

  portENTER_CRITICAL(&eyeMux);
  EyeState s = state;
  portEXIT_CRITICAL(&eyeMux);

  // ── Blink ─────────────────────────────────────────────
  if (s.autoBlink && strcmp(s.mood,"closed")!=0 && strcmp(s.mood,"heart")!=0) {
    if (!s.blinking && (now - s.lastBlink) > (unsigned long)(s.blinkRate * 1000)) {
      portENTER_CRITICAL(&eyeMux);
      state.blinking = true; state.blinkPhase = 0.0f;
      portEXIT_CRITICAL(&eyeMux);
      s.blinking = true; s.blinkPhase = 0.0f;
    }
    if (s.blinking) {
      float bp = s.blinkPhase + dt * 6.0f;
      portENTER_CRITICAL(&eyeMux);
      if (bp > 1.0f) { state.blinkPhase=0.0f; state.blinking=false; state.lastBlink=now; }
      else           { state.blinkPhase=bp; }
      portEXIT_CRITICAL(&eyeMux);
      s.blinkPhase = min(bp, 1.0f);
    }
  }

  // ── Wiggle ────────────────────────────────────────────
  if (s.wiggle && (now - s.lastWiggle > 1200)) {
    portENTER_CRITICAL(&eyeMux);
    state.wiggleOffX = random(-12,12) / 100.0f;
    state.wiggleOffY = random(-12,12) / 100.0f;
    state.lastWiggle = now;
    portEXIT_CRITICAL(&eyeMux);
    s.wiggleOffX = state.wiggleOffX;
    s.wiggleOffY = state.wiggleOffY;
  }

  // ── Alert timeout ─────────────────────────────────────
  if (s.alertActive && (now - s.lastAlert > 1500)) {
    portENTER_CRITICAL(&eyeMux);
    state.alertActive = false;
    if (strcmp(state.mood,"wide")==0) setMood("normal");
    portEXIT_CRITICAL(&eyeMux);
    s.alertActive = false;
  }

  // ── Effective gaze ────────────────────────────────────
  float gx, gy;
  bool manualActive = (fabsf(s.manualGazeX)>0.02f || fabsf(s.manualGazeY)>0.02f);
  if (manualActive) {
    gx = s.manualGazeX; gy = s.manualGazeY;
  } else if (s.cameraActive && s.motionEnabled && s.motionDetected) {
    gx = s.visionGazeX + s.wiggleOffX;
    gy = s.visionGazeY + s.wiggleOffY;
  } else {
    gx = s.wiggleOffX; gy = s.wiggleOffY;
  }
  gx = constrain(gx, -0.45f, 0.45f);
  gy = constrain(gy, -0.45f, 0.45f);

  // ── Pupil dilation ────────────────────────────────────
  // pupilMul is computed locally — not written back to state.pupilScale
  // so the BLE-set base value is always preserved.
  float pupilMul = s.pupilScale;
  if (s.cameraActive && s.lightEnabled) {
    float lightFactor = 0.6f + (1.0f - s.sceneBrightness) * 0.8f;
    pupilMul = constrain(s.pupilScale * lightFactor, 0.4f, 1.8f);
  }

  // ── Face reaction ─────────────────────────────────────
  char effectiveMood[16];
  strncpy(effectiveMood, s.mood, sizeof(effectiveMood) - 1);
  effectiveMood[sizeof(effectiveMood) - 1] = '\0';
  if (s.cameraActive && s.faceEnabled && s.faceDetected && s.faceSize > 0.15f) {
    if (strcmp(effectiveMood, "normal") == 0) strncpy(effectiveMood, "wide", sizeof(effectiveMood) - 1);
  }

  // ── Draw both eyes ────────────────────────────────────
  selectDisplay(false);
  drawEye(gx, gy, false, s.blinkPhase, effectiveMood, s.eyeScale, pupilMul,
          s.irisOuter, s.irisInner);
  selectDisplay(true);
  drawEye(s.mirror ? -gx : gx, gy, true, s.blinkPhase, effectiveMood,
          s.eyeScale, pupilMul, s.irisOuter, s.irisInner);

  // ── BLE status ping (~every 3s) ───────────────────────
  static unsigned long lastPing = 0;
  if (deviceConnected && pTxChar && (now - lastPing > 3000)) {
    StaticJsonDocument<256> doc;
    doc["mood"]       = s.mood;
    doc["camera"]     = s.cameraActive;
    doc["motion"]     = s.motionDetected;
    doc["face"]       = s.faceDetected;
    doc["brightness"] = (int)(s.sceneBrightness * 100);
    portENTER_CRITICAL(&eyeMux);
    doc["hapticL"] = hapticState.leftPWM;
    doc["hapticR"] = hapticState.rightPWM;
    portEXIT_CRITICAL(&eyeMux);
    String out; serializeJson(doc, out); out += "\n";
    pTxChar->setValue(out.c_str());
    pTxChar->notify();
    lastPing = now;
  }
}
