// ============================================================
//  camera_stream.h  —  WiFi AP + MJPEG preview stream
//
//  Creates a soft-AP "DoggleEyes-CAM".  While a client is
//  connected to /stream, vision processing is paused and the
//  camera switches to JPEG output.  When the client disconnects,
//  the camera reverts to grayscale and vision resumes.
//
//  Usage:
//    setup(): initCameraStream() after camera init
//    loop() : handleCameraStream()
//    visionTask: skip frame when streamActive == true
// ============================================================
#pragma once
#include <WiFi.h>
#include <WebServer.h>
#include "esp_camera.h"

#define CAM_SSID     "DoggleEyes-CAM"
#define CAM_PASSWORD "doggles1"

WebServer camServer(80);
volatile bool streamActive = false;  // read by visionTask to pause

// ── Inline page ───────────────────────────────────────────────
static const char CAM_PAGE[] =
  "<!DOCTYPE html><html>"
  "<head><meta name='viewport' content='width=device-width,initial-scale=1'>"
  "<title>DoggleEyes Camera</title>"
  "<style>body{margin:0;background:#111;display:flex;flex-direction:column;"
  "align-items:center;justify-content:center;min-height:100vh;gap:.5rem}"
  "img{width:100%;max-width:480px;image-rendering:pixelated}"
  "p{color:#888;font-family:monospace;font-size:.75rem;text-align:center}</style></head>"
  "<body><p>DoggleEyes &mdash; live camera &mdash; 160x120</p>"
  "<img src='/stream'>"
  "<p>Close tab to stop stream and resume motion tracking.</p>"
  "</body></html>";

// ── Reconfigure camera between JPEG (stream) and grayscale (vision) ──
// Must fully deinit+reinit — the frame buffer is sized at init time and
// cannot be resized in-place. Grayscale QQVGA = 19200 bytes fixed;
// JPEG size is variable so the driver refuses to switch without reinit.
static void reconfigCamera(bool forJpeg) {
  esp_camera_deinit();
  delay(100);

  camera_config_t cfg = buildCameraConfig();
  if (forJpeg) {
    cfg.pixel_format = PIXFORMAT_JPEG;
    cfg.frame_size   = FRAMESIZE_QVGA;   // 320×240 — better preview than QQVGA
    cfg.jpeg_quality = 10;
    cfg.fb_count     = 1;                // single buffer — JPEG frames are larger
    cfg.fb_location  = CAMERA_FB_IN_PSRAM;
  }
  // grayscale path uses buildCameraConfig() defaults (QQVGA, fb_count=2)

  esp_err_t err = esp_camera_init(&cfg);
  if (err != ESP_OK) {
    Serial.printf("[Stream] camera reinit failed: 0x%x\n", err);
  }
  delay(100);
}

// ── HTTP handlers ─────────────────────────────────────────────
static void handleRoot() {
  camServer.send(200, "text/html", CAM_PAGE);
}

static void handleStream() {
  // Pause vision task, then switch camera to JPEG
  streamActive = true;
  delay(150);  // wait for any in-progress vision frame to complete
  reconfigCamera(true);

  WiFiClient client = camServer.client();
  if (!client) {
    streamActive = false;
    reconfigCamera(false);
    return;
  }

  // Send MJPEG response headers — use print() not printf() (more reliable on ESP32)
  client.print("HTTP/1.1 200 OK\r\n");
  client.print("Content-Type: multipart/x-mixed-replace; boundary=frame\r\n");
  client.print("Access-Control-Allow-Origin: *\r\n");
  client.print("Cache-Control: no-cache\r\n");
  client.print("Connection: close\r\n");
  client.print("\r\n");

  Serial.println("[Stream] Client connected — streaming MJPEG, vision paused.");
  unsigned long frameCount = 0;

  while (client.connected()) {
    camera_fb_t* fb = esp_camera_fb_get();
    if (!fb) {
      delay(30);
      continue;
    }

    // Verify we actually got JPEG (not leftover grayscale)
    if (fb->format != PIXFORMAT_JPEG || fb->len < 4) {
      esp_camera_fb_return(fb);
      delay(20);
      continue;
    }

    // Write MJPEG boundary + part headers
    client.print("--frame\r\n");
    client.print("Content-Type: image/jpeg\r\n");
    client.print("Content-Length: ");
    client.print((unsigned int)fb->len);
    client.print("\r\n\r\n");

    // Write pixel data in chunks to avoid WiFi TX buffer overflow
    const uint8_t* p   = fb->buf;
    size_t         rem = fb->len;
    while (rem > 0 && client.connected()) {
      size_t chunk = rem < 1024 ? rem : 1024;
      client.write(p, chunk);
      p   += chunk;
      rem -= chunk;
    }
    client.print("\r\n");

    esp_camera_fb_return(fb);
    frameCount++;

    delay(66);  // ~15 fps
  }

  Serial.print("[Stream] Disconnected after ");
  Serial.print(frameCount);
  Serial.println(" frames — resuming vision.");
  reconfigCamera(false);
  streamActive = false;
}

static void handleNotFound() {
  camServer.send(404, "text/plain", "Not found");
}

// ── Init ──────────────────────────────────────────────────────
static void initCameraStream() {
  WiFi.softAP(CAM_SSID, CAM_PASSWORD);
  delay(200);
  IPAddress ip = WiFi.softAPIP();
  Serial.print("[WiFi] AP '");
  Serial.print(CAM_SSID);
  Serial.println("' started (password: doggles1)");
  Serial.print("[Stream] Connect to that WiFi then open  http://");
  Serial.println(ip);

  camServer.on("/",       HTTP_GET, handleRoot);
  camServer.on("/stream", HTTP_GET, handleStream);
  camServer.onNotFound(handleNotFound);
  camServer.begin();
  Serial.println("[Stream] HTTP server ready on port 80.");
}

// ── Tick — call from loop() ───────────────────────────────────
static void handleCameraStream() {
  camServer.handleClient();
}
