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
//    Motor LEFT   : GPIO 44  (D7) — digitalWrite, active-HIGH driver
//    Motor RIGHT  : GPIO 43  (D6) — digitalWrite, active-HIGH driver
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

// ── Compile-time check: catch stale User_Setup.h in library folder ──
// If this fails: copy User_Setup.h from this sketch folder to
// Arduino/libraries/TFT_eSPI/User_Setup.h and recompile.
#ifndef DOGGLEEYES_SETUP_ID
  #error "User_Setup.h not copied to TFT_eSPI library folder!"
#endif
#if DOGGLEEYES_SETUP_ID != 42
  #error "Stale User_Setup.h in TFT_eSPI library folder — copy the latest from sketch folder."
#endif
#include "esp_camera.h"
#include "camera_config.h"
#include "vision.h"
#include "eye_graphics.h"
#include "haptic_vision.h"
#include "camera_stream.h"

// ── BLE UUIDs (Nordic UART Service) ──────────────────────────
#define NUS_SERVICE_UUID  "6E400001-B5A3-F393-E0A9-E50E24DCCA9E"
#define NUS_RX_UUID       "6E400002-B5A3-F393-E0A9-E50E24DCCA9E"
#define NUS_TX_UUID       "6E400003-B5A3-F393-E0A9-E50E24DCCA9E"

// ── Display CS pins ───────────────────────────────────────────
#define CS_LEFT   1
#define CS_RIGHT  2

// ── TFT object — pointer so constructor runs inside setup() ──
// Global TFT_eSPI construction causes StoreProhibited on ESP32-S3
TFT_eSPI*    tftPtr = nullptr;
// ── Sprite framebuffer — eliminates rolling-shutter refresh ──
// Eye is drawn to this 240×240 PSRAM buffer, then pushed to the
// display as one continuous SPI burst (no mid-frame partial updates).
TFT_eSprite* spr    = nullptr;

// ── BLE globals ───────────────────────────────────────────────
NimBLEServer*         pServer = nullptr;
NimBLECharacteristic* pTxChar = nullptr;
NimBLECharacteristic* pRxChar = nullptr;
bool             deviceConnected   = false;
volatile bool    blePreviewEnabled  = false;  // BLE camera preview (40×30 grayscale)
volatile bool    reinitRequested    = false;  // set by BLE "reinit" action, consumed in loop()

// ── Mutex (plain — no constructor, safe as global) ───────────
portMUX_TYPE eyeMux = portMUX_INITIALIZER_UNLOCKED;

// ── HapticState (POD struct — safe as global) ────────────────
HapticState hapticState;

// ── Eye state ─────────────────────────────────────────────────
// Pure POD struct — NO default member initializers.
// Any initializer (even = 0.0f) causes the compiler to generate
// a constructor, which runs at static-init time before the Arduino
// runtime is ready on ESP32-S3, producing a StoreProhibited panic.
// All fields are initialised explicitly in setup() via initState().
struct EyeState {
  char     mood[16];
  float    manualGazeX;
  float    manualGazeY;
  uint16_t irisOuter;
  uint16_t irisInner;
  uint16_t irisOuterR;   // right eye — equals irisOuter unless heterochromia is set
  uint16_t irisInnerR;
  float    eyeScale;
  float    pupilScale;
  bool     autoBlink;
  float    blinkRate;
  bool     wiggle;
  bool     mirror;

  bool     cameraActive;
  bool     motionEnabled;
  bool     lightEnabled;
  bool     faceEnabled;

  float    visionGazeX;
  float    visionGazeY;
  float    sceneBrightness;
  bool     motionDetected;
  bool     faceDetected;
  float    faceSize;

  float    blinkPhase;
  bool     blinking;
  unsigned long lastBlink;
  float    wiggleOffX;
  float    wiggleOffY;
  unsigned long lastWiggle;
  unsigned long lastAlert;
  bool     alertActive;
} state;

// Helper — set mood safely
inline void setMood(const char* m) {
  strncpy(state.mood, m, sizeof(state.mood) - 1);
  state.mood[sizeof(state.mood) - 1] = '\0';
}

// Initialise all EyeState fields — called from setup(), never at global scope
inline void initState() {
  memset(&state, 0, sizeof(state));
  setMood("normal");
  state.irisOuter      = 0x11F6;  // deep cobalt blue  ~RGB(16,  60, 176)
  state.irisInner      = 0x551E;  // light cerulean    ~RGB(80, 160, 240)
  state.irisOuterR     = 0x11F6;  // right eye defaults to same as left
  state.irisInnerR     = 0x551E;
  state.eyeScale       = 1.0f;
  state.pupilScale     = 1.0f;
  state.autoBlink      = true;
  state.blinkRate      = 7.5f;
  state.mirror         = true;
  state.motionEnabled  = true;
  state.lightEnabled   = true;
  state.faceEnabled    = true;
  state.sceneBrightness = 1.0f;
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
  if (doc.containsKey("preview"))           blePreviewEnabled      = doc["preview"].as<bool>();

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
  if (doc.containsKey("irisOuterR")) {
    state.irisOuterR = tftPtr->color565((uint8_t)doc["irisOuterR"]["r"],
                                        (uint8_t)doc["irisOuterR"]["g"],
                                        (uint8_t)doc["irisOuterR"]["b"]);
  }
  if (doc.containsKey("irisInnerR")) {
    state.irisInnerR = tftPtr->color565((uint8_t)doc["irisInnerR"]["r"],
                                        (uint8_t)doc["irisInnerR"]["g"],
                                        (uint8_t)doc["irisInnerR"]["b"]);
  }
  if (doc.containsKey("action")) {
    const char* a = doc["action"].as<const char*>();
    if (strcmp(a,"blink")==0)  { state.blinking = true; state.blinkPhase = 0.0f; }
    if (strcmp(a,"dilate")==0) { state.pupilScale = 1.6f; }
    if (strcmp(a,"alert")==0)  { state.alertActive = true; state.lastAlert = millis(); }
    if (strcmp(a,"reinit")==0) { reinitRequested = true; }
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

  // Skip first 60 frames — camera produces noisy output during warmup
  // which triggers false haptic motion detections
  for (int warmup = 0; warmup < 60; warmup++) {
    camera_fb_t* fb = esp_camera_fb_get();
    if (fb) { memcpy(prevFrame, fb->buf, W * H); esp_camera_fb_return(fb); }
    vTaskDelay(pdMS_TO_TICKS(VISION_INTERVAL_MS));
  }
  Serial.println("[Vision] Warmup complete — haptics and gaze now active.");

  for (;;) {
    // Pause while camera stream client is connected (camera is in JPEG mode)
    if (streamActive) { vTaskDelay(pdMS_TO_TICKS(50)); continue; }

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

    // ── BLE camera preview (diagnostic) ──────────────────
    // Downsample 160×120 → 40×30 and send as binary BLE notifications.
    // Protocol: first chunk = [0x01, W, H, pixels...],
    //           subsequent  = [0x02, pixels...]
    // HTML reconstructs and renders to a canvas element.
    if (blePreviewEnabled && deviceConnected && pTxChar && !streamActive) {
      static unsigned long lastPreview = 0;
      if (millis() - lastPreview >= 333) {  // max ~3 fps — keeps BLE stack comfortable
        lastPreview = millis();
        const int PW = 40, PH = 30;
        const int TOTAL = PW * PH;            // 1200 bytes
        const int PAYLOAD = 176;              // pixels per BLE packet
        uint8_t small[TOTAL];

        // 4×4 box filter downsample
        for (int py = 0; py < PH; py++) {
          for (int px = 0; px < PW; px++) {
            uint32_t sum = 0;
            for (int dy = 0; dy < 4; dy++)
              for (int dx = 0; dx < 4; dx++)
                sum += cur[(py*4+dy) * W + (px*4+dx)];
            small[py*PW + px] = (uint8_t)(sum >> 4);  // divide by 16
          }
        }

        // Chunk and notify
        int sent = 0;
        bool isFirst = true;
        while (sent < TOTAL) {
          int hlen = isFirst ? 3 : 1;
          int plen = min(PAYLOAD, TOTAL - sent);
          uint8_t buf[PAYLOAD + 3];
          buf[0] = isFirst ? 0x01 : 0x02;
          if (isFirst) { buf[1] = (uint8_t)PW; buf[2] = (uint8_t)PH; }
          memcpy(buf + hlen, small + sent, plen);
          pTxChar->setValue(buf, hlen + plen);
          pTxChar->notify();
          sent += plen;
          if (sent < TOTAL) vTaskDelay(pdMS_TO_TICKS(20));  // pace BLE stack
          isFirst = false;
        }
      }
    }

    memcpy(prevFrame, cur, W * H);
    esp_camera_fb_return(fb);
    vTaskDelay(pdMS_TO_TICKS(VISION_INTERVAL_MS));
  }
}

// ─────────────────────────────────────────────────────────────
//  DISPLAY HELPERS
// ─────────────────────────────────────────────────────────────
void selectDisplay(bool right) {
  tftPtr->endWrite();               // flush any pending SPI transaction
  digitalWrite(CS_LEFT,  HIGH);    // deassert BOTH before transitioning
  digitalWrite(CS_RIGHT, HIGH);
  delayMicroseconds(10);           // GPIO matrix propagation settle (~4 cycles @240MHz = 17ns, but RC on lines needs more)
  digitalWrite(right ? CS_RIGHT : CS_LEFT, LOW);
  delayMicroseconds(5);            // CS assert settle before first SPI clock
}

// ─────────────────────────────────────────────────────────────
//  DISPLAY RE-INIT  (called from loop() on BLE "reinit" command)
// ─────────────────────────────────────────────────────────────
void reinitDisplays() {
  Serial.println("[Display] Re-init requested — pulsing RST and re-running two-pass init...");

  // Hardware reset both displays — necessary if a controller is fully stuck
  pinMode(6, OUTPUT);
  digitalWrite(6, LOW);  delay(20);
  digitalWrite(6, HIGH); delay(500);

  // Pass 1 — init sequence only
  selectDisplay(false); tftPtr->init(); delay(50);
  selectDisplay(true);  tftPtr->init(); delay(50);
  delay(50);

  // Pass 2 — configure both
  selectDisplay(false); tftPtr->setRotation(2); tftPtr->fillScreen(TFT_BLACK); delay(20);
  selectDisplay(true);  tftPtr->setRotation(2); tftPtr->fillScreen(TFT_BLACK); delay(20);

  digitalWrite(CS_LEFT, HIGH); digitalWrite(CS_RIGHT, HIGH);
  Serial.println("[Display] Re-init complete.");
}

// ─────────────────────────────────────────────────────────────
//  SETUP
// ─────────────────────────────────────────────────────────────
void setup() {
  Serial.begin(115200);
  delay(200);   // brief settle before first print
  Serial.println("\n[DoggleEyes v3.1 Haptic] Booting...");

  // Initialise all state — must be done here, not at global scope
  initState();

  // ── TFT — create on heap inside setup() ───────────────
  tftPtr = new TFT_eSPI();

  // ── Sprite framebuffer (240×240 × 2 bytes = 115 kB in PSRAM) ─
  spr = new TFT_eSprite(tftPtr);
  spr->setColorDepth(16);
  if (!spr->createSprite(240, 240)) {
    Serial.println("[Sprite] FAILED — check board settings: PSRAM must be OPI PSRAM.");
    while (true) delay(1000);
  }
  Serial.println("[Sprite] 240x240 framebuffer allocated in PSRAM.");

  // ── Displays ──────────────────────────────────────────
  pinMode(CS_LEFT,  OUTPUT); digitalWrite(CS_LEFT,  HIGH);
  pinMode(CS_RIGHT, OUTPUT); digitalWrite(CS_RIGHT, HIGH);

  // Hardware reset both displays together once.
  // TFT_RST=-1 in User_Setup.h so TFT_eSPI never pulses RST internally —
  // if it did, the second init() call would reset both displays, leaving
  // the first uninitialised.
  pinMode(6, OUTPUT);
  digitalWrite(6, LOW);  delay(20);   // hold reset long enough for both displays
  digitalWrite(6, HIGH); delay(500);  // extended settle — cold power-up needs more time
                                       // (increased from 250ms: left eye intermittently
                                       //  failed to init at 250ms on cold start)

  // Two-pass init: send init sequence to both displays first, then
  // configure both. This prevents the second init() from disturbing
  // the first display's settings — whichever is initialised first
  // would otherwise lose its rotation/fill when the SPI bus is
  // reconfigured for the second init().

  // Pass 1 — init sequence only, no configuration yet
  selectDisplay(false);
  tftPtr->init();
  delay(50);  // increased from 20ms — left display needs more settle time

  selectDisplay(true);
  tftPtr->init();
  delay(50);  // match left for symmetry

  // Brief pause between passes — lets both controllers finish internal
  // post-init calibration before we start sending configuration commands
  delay(50);

  // Pass 2 — configure both now that both are initialised
  selectDisplay(false);
  tftPtr->setRotation(2);
  tftPtr->fillScreen(TFT_BLACK);
  delay(20);  // increased from 10ms

  selectDisplay(true);
  tftPtr->setRotation(2);
  tftPtr->fillScreen(TFT_BLACK);
  delay(20);  // match left

  digitalWrite(CS_LEFT,  HIGH);
  digitalWrite(CS_RIGHT, HIGH);

  // ── Boot test pattern ─────────────────────────────────
  // Flash each display a solid colour so wiring can be confirmed
  // visually before the main loop starts.
  // RIGHT = green, LEFT = red for 600 ms each.
  selectDisplay(true);  tftPtr->fillScreen(0x07E0); // right = green
  selectDisplay(false); tftPtr->fillScreen(0xF800); // left  = red
  delay(600);
  selectDisplay(true);  tftPtr->fillScreen(TFT_BLACK);
  selectDisplay(false); tftPtr->fillScreen(TFT_BLACK);
  digitalWrite(CS_LEFT, HIGH); digitalWrite(CS_RIGHT, HIGH);

  Serial.println("[Init] Displays OK.");

  // ── Haptic motors ──────────────────────────────────────
  hapticInit();

  // ── BLE — init BEFORE camera ───────────────────────────
  // NimBLE and esp_camera both use Core 0. Starting BLE first gives the
  // NimBLE stack time to stabilise before the camera task competes for
  // Core 0 resources. Starting camera first has been observed to silently
  // prevent BLE from advertising on ESP32-S3.
  NimBLEDevice::init("DoggleEyes");
  NimBLEDevice::setMTU(512);          // request larger MTU for BLE camera preview chunks
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
  pAdv->setMinInterval(32);          // 20 ms
  pAdv->setMaxInterval(64);          // 40 ms
  bool advOK = NimBLEDevice::startAdvertising();
  Serial.printf("[BLE] startAdvertising() = %s\n", advOK ? "OK" : "FAILED");
  Serial.printf("[BLE] MAC address  : %s\n", NimBLEDevice::getAddress().toString().c_str());
  Serial.printf("[BLE] Service UUID : %s\n", NUS_SERVICE_UUID);
  Serial.printf("[BLE] RX char UUID : %s\n", NUS_RX_UUID);
  Serial.printf("[BLE] TX char UUID : %s\n", NUS_TX_UUID);
  Serial.printf("[BLE] Adv name     : DoggleEyes\n");
  Serial.printf("[BLE] Adv interval : 20-40 ms\n");
  if (advOK) Serial.println("[BLE] Advertising as 'DoggleEyes' — visible in nRF Connect now.");

  // ── Camera — init AFTER BLE ────────────────────────────
  camera_config_t camCfg = buildCameraConfig();
  esp_err_t err = esp_camera_init(&camCfg);
  if (err != ESP_OK) {
    Serial.printf("[Camera] Init FAILED: 0x%x — running without vision.\n", err);
    portENTER_CRITICAL(&eyeMux);
    state.cameraActive = false;
    portEXIT_CRITICAL(&eyeMux);
  } else {
    sensor_t* s = esp_camera_sensor_get();
    if (!s) {
      Serial.println("[Camera] sensor_get() returned NULL — halting.");
      while (true) delay(1000);
    }
    s->set_framesize(s, FRAMESIZE_QQVGA);
    s->set_pixformat(s, PIXFORMAT_GRAYSCALE);
    s->set_gainceiling(s, (gainceiling_t)6);
    s->set_whitebal(s, 1);
    s->set_awb_gain(s, 1);
    portENTER_CRITICAL(&eyeMux);
    state.cameraActive = true;
    portEXIT_CRITICAL(&eyeMux);
    Serial.println("[Camera] Initialised OK — 160x120 grayscale.");
    initCameraStream();
    xTaskCreatePinnedToCore(visionTask, "VisionTask", 8192, NULL, 1, NULL, 0);
  }

  // ── BLE advertising check — camera init can silently stop it ──
  {
    bool stillAdv = NimBLEDevice::getAdvertising()->isAdvertising();
    Serial.printf("[BLE] After camera init: advertising = %s\n", stillAdv ? "YES (good)" : "NO — restarting");
    if (!stillAdv) {
      NimBLEDevice::startAdvertising();
      Serial.println("[BLE] Advertising restarted.");
    }
  }

  // Re-assert CS and motor pins — camera init reconfigures IO matrix,
  // which can float GPIO outputs set before esp_camera_init().
  pinMode(CS_LEFT,  OUTPUT); digitalWrite(CS_LEFT,  HIGH);
  pinMode(CS_RIGHT, OUTPUT); digitalWrite(CS_RIGHT, HIGH);
  pinMode(MOTOR_LEFT_PIN,  OUTPUT); digitalWrite(MOTOR_LEFT_PIN,  LOW);  // LOW = motor OFF
  pinMode(MOTOR_RIGHT_PIN, OUTPUT); digitalWrite(MOTOR_RIGHT_PIN, LOW);

  Serial.println("[DoggleEyes v3.1] Ready — displays + haptic + camera + BLE.");
}

// ─────────────────────────────────────────────────────────────
//  MAIN LOOP  (Core 1)
// ─────────────────────────────────────────────────────────────
void loop() {
  // Service display re-init request before anything else — runs full two-pass
  // init including RST pulse. Safe here because we're on Core 1, the same core
  // that owns the display, so there's no concurrent SPI access.
  if (reinitRequested) {
    reinitRequested = false;
    reinitDisplays();
  }

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
      float bp = s.blinkPhase + dt * 18.0f;  // 18x speed = ~55ms blink vs old 300ms
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
  // Draw left eye into sprite, then push to display in one SPI burst.
  // This eliminates rolling-shutter: the display only updates once the
  // full frame is ready, transferring as a single continuous pixel stream.
  drawEye(-gx, gy, false, s.blinkPhase, effectiveMood, s.eyeScale, pupilMul,
          s.irisOuter, s.irisInner);  // negate X — setRotation(2) mirrors horizontal axis
  selectDisplay(false);
  spr->pushSprite(0, 0);

  // Right eye
  drawEye(s.mirror ? -gx : gx, gy, true, s.blinkPhase, effectiveMood, s.eyeScale, pupilMul,
          s.irisOuterR, s.irisInnerR);
  selectDisplay(true);
  spr->pushSprite(0, 0);

  // Deassert both CS at end of frame — leaves bus idle
  digitalWrite(CS_LEFT,  HIGH);
  digitalWrite(CS_RIGHT, HIGH);

  // ── BLE advertising heartbeat (~every 5s) ────────────
  static unsigned long lastAdvCheck = 0;
  if (now - lastAdvCheck > 5000) {
    bool adv = NimBLEDevice::getAdvertising()->isAdvertising();
    Serial.printf("[BLE] heartbeat — advertising=%s connected=%s\n",
                  adv ? "YES" : "NO", deviceConnected ? "YES" : "NO");
    if (!adv && !deviceConnected) NimBLEDevice::startAdvertising();
    lastAdvCheck = now;
  }

  // ── Camera stream server tick ─────────────────────────
  handleCameraStream();

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
