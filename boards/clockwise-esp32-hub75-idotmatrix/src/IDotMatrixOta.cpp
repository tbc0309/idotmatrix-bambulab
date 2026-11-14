#include "IDotMatrixOta.h"
#include "IDotMatrixMemoryTelemetry.h"

#ifndef DEBUG_SERIAL
#define DEBUG_SERIAL 1
#endif

#if IDOTMATRIX_OTA_AVAILABLE

#include <WiFi.h>
#include <DNSServer.h>
#include <WebServer.h>
#include <Update.h>

namespace {
DNSServer dnsServer;
WebServer server(80);
bool otaActive = false;
bool dnsActive = false;
bool rebootPending = false;
uint32_t rebootAt = 0;
bool triggerRawPressed = false;
bool triggerStablePressed = false;
bool triggerLongActionDone = false;
uint32_t triggerDebounceSince = 0;
uint32_t triggerSince = 0;
bool uploadCommitted = false;
String releaseText;
uint32_t buildNumber = 0;
String apSsid;
#if IDOTMATRIX_MEMORY_TELEMETRY
uint32_t uploadTelemetryStartedAtUs = 0;
#endif

bool triggerPressed() {
  const int level = digitalRead(IDOTMATRIX_OTA_TRIGGER_PIN);
  return IDOTMATRIX_OTA_TRIGGER_ACTIVE_LOW ? (level == LOW) : (level == HIGH);
}

String portalUrl() {
  return String("http://") + WiFi.softAPIP().toString() + "/";
}

void redirectToPortal() {
  server.sendHeader("Location", portalUrl(), true);
  server.sendHeader("Cache-Control", "no-store");
  server.send(302, "text/plain", "");
}

String htmlPage() {
  String page;
  page.reserve(1800);
  page += F("<!doctype html><html><head><meta charset='utf-8'>");
  page += F("<meta name='viewport' content='width=device-width,initial-scale=1'>");
  page += F("<title>iDotMatrix OTA</title><style>");
  page += F("body{font-family:sans-serif;max-width:680px;margin:40px auto;padding:0 18px;line-height:1.45}");
  page += F("input,button{font-size:1rem;padding:10px;margin-top:8px}button{cursor:pointer}</style></head><body>");
  page += F("<h1>iDotMatrix Firmware Update</h1><p>Current firmware: <strong>");
  page += releaseText;
  page += F(" / Build ");
  page += String(buildNumber);
  page += F("</strong></p>");
  page += F("<p>Select a PlatformIO <code>firmware.bin</code> built for the Waveshare ESP32-S3 RGB Matrix target.</p>");
  page += F("<form method='POST' action='/update' enctype='multipart/form-data'>");
  page += F("<input type='file' name='firmware' accept='.bin,application/octet-stream' required><br>");
  page += F("<button type='submit'>Upload firmware</button></form>");
  page += F("<p>Do not remove power while the upload is in progress. An interrupted upload leaves the currently running OTA slot unchanged.</p>");
  page += F("</body></html>");
  return page;
}

void startAccessPoint() {
  if (otaActive) return;

  idotMemoryTelemetrySnapshot("ota.ap.before_wifi");
  uint64_t chip = ESP.getEfuseMac();
  char suffix[7];
  snprintf(suffix, sizeof(suffix), "%06llX", (unsigned long long)(chip & 0xFFFFFFULL));
  apSsid = String("IDotMatrix-OTA-") + suffix;

  WiFi.mode(WIFI_AP);
  WiFi.setSleep(false);
  const bool apOk = WiFi.softAP(apSsid.c_str(), IDOTMATRIX_OTA_AP_PASSWORD);
  if (!apOk) {
#if DEBUG_SERIAL
    Serial.println("OTA: failed to start Wi-Fi access point");
#endif
    WiFi.mode(WIFI_OFF);
    return;
  }

  const IPAddress apIp = WiFi.softAPIP();
  dnsActive = dnsServer.start(53, "*", apIp);
#if DEBUG_SERIAL
  if (!dnsActive) {
    Serial.println("OTA: captive DNS failed to start; direct URL remains available");
  }
#endif

  server.on("/", HTTP_GET, []() {
    server.sendHeader("Cache-Control", "no-store");
    server.send(200, "text/html", htmlPage());
  });

  // Common captive-portal probes used by Android, Apple and Windows.
  // Wildcard DNS resolves their hostnames to the maintenance AP; these
  // redirects make the OS open the OTA page instead of requiring the user
  // to discover 192.168.4.1 from the serial log.
  server.on("/generate_204", HTTP_GET, redirectToPortal);
  server.on("/gen_204", HTTP_GET, redirectToPortal);
  server.on("/hotspot-detect.html", HTTP_GET, redirectToPortal);
  server.on("/library/test/success.html", HTTP_GET, redirectToPortal);
  server.on("/connecttest.txt", HTTP_GET, redirectToPortal);
  server.on("/ncsi.txt", HTTP_GET, redirectToPortal);
  server.on("/fwlink", HTTP_GET, redirectToPortal);

  server.on("/health", HTTP_GET, []() {
    String body = String("release=") + releaseText + "\nbuild=" + String(buildNumber) + "\nota=active\n";
    server.send(200, "text/plain", body);
  });

  server.on(
      "/update", HTTP_POST,
      []() {
        const bool ok = uploadCommitted;
        server.sendHeader("Connection", "close");
        server.send(ok ? 200 : 500, "text/plain",
                    ok ? "Update accepted. Rebooting..." : "Update failed. Current firmware remains active.");
        if (ok) {
          rebootPending = true;
          rebootAt = millis() + 1200UL;
        }
      },
      []() {
        HTTPUpload &upload = server.upload();
        switch (upload.status) {
          case UPLOAD_FILE_START:
            uploadCommitted = false;
            idotMemoryTelemetrySnapshot("ota.upload.before_begin");
#if IDOTMATRIX_MEMORY_TELEMETRY
            uploadTelemetryStartedAtUs = micros();
#endif
#if DEBUG_SERIAL
            Serial.print("OTA: upload start name="); Serial.println(upload.filename);
#endif
            if (!Update.begin(UPDATE_SIZE_UNKNOWN, U_FLASH)) {
#if DEBUG_SERIAL
              Update.printError(Serial);
#endif
            }
            idotMemoryTelemetrySnapshot("ota.upload.after_begin");
            break;
          case UPLOAD_FILE_WRITE:
            if (!Update.hasError()) {
              const size_t written = Update.write(upload.buf, upload.currentSize);
              if (written != upload.currentSize) {
#if DEBUG_SERIAL
                Serial.println("OTA: short write");
                Update.printError(Serial);
#endif
              }
            }
            break;
          case UPLOAD_FILE_END:
            if (!Update.hasError()) {
              uploadCommitted = Update.end(true);
              if (!uploadCommitted) {
#if DEBUG_SERIAL
                Update.printError(Serial);
#endif
              }
            }
#if DEBUG_SERIAL
            if (uploadCommitted) {
              Serial.print("OTA: upload complete bytes="); Serial.println(upload.totalSize);
            }
#endif
#if IDOTMATRIX_MEMORY_TELEMETRY
            idotMemoryTelemetryLatency(uploadCommitted ? "ota.upload.complete_us" : "ota.upload.failed_us",
                                       (uint32_t)(micros() - uploadTelemetryStartedAtUs));
#endif
            idotMemoryTelemetrySnapshot(uploadCommitted ? "ota.upload.complete" : "ota.upload.failed");
            break;
          case UPLOAD_FILE_ABORTED:
            uploadCommitted = false;
            Update.abort();
#if IDOTMATRIX_MEMORY_TELEMETRY
            idotMemoryTelemetryLatency("ota.upload.aborted_us", (uint32_t)(micros() - uploadTelemetryStartedAtUs));
#endif
            idotMemoryTelemetrySnapshot("ota.upload.aborted");
#if DEBUG_SERIAL
            Serial.println("OTA: upload aborted; current firmware preserved");
#endif
            break;
          default:
            break;
        }
      });

  server.onNotFound([]() {
    redirectToPortal();
  });

  server.begin();
  otaActive = true;
  idotMemoryTelemetrySnapshot("ota.ap.ready");
#if DEBUG_SERIAL
  Serial.println("=== IDOTMATRIX OTA MAINTENANCE ===");
  Serial.print("OTA AP SSID: "); Serial.println(apSsid);
  Serial.print("OTA AP password: "); Serial.println(IDOTMATRIX_OTA_AP_PASSWORD);
  Serial.print("OTA URL: http://"); Serial.print(WiFi.softAPIP()); Serial.println('/');
  Serial.println("OTA: Wi-Fi stays enabled until reboot");
  Serial.println("==================================");
#endif
}
}  // namespace

void idotOtaBegin(const char *release, uint32_t build) {
  releaseText = release ? release : "unknown";
  buildNumber = build;
  pinMode(IDOTMATRIX_OTA_TRIGGER_PIN,
          IDOTMATRIX_OTA_TRIGGER_ACTIVE_LOW ? INPUT_PULLUP : INPUT_PULLDOWN);
  WiFi.mode(WIFI_OFF);
#if DEBUG_SERIAL
  Serial.print("BOOT BUTTON: GPIO"); Serial.print(IDOTMATRIX_OTA_TRIGGER_PIN);
  Serial.print(IDOTMATRIX_OTA_TRIGGER_ACTIVE_LOW ? " active LOW" : " active HIGH");
  Serial.print(", short press=reboot, hold >= "); Serial.print(IDOTMATRIX_OTA_TRIGGER_HOLD_MS);
  Serial.println(" ms=OTA maintenance AP");
#endif
}

void idotOtaLoop(uint32_t nowMs) {
  if (rebootPending && (int32_t)(nowMs - rebootAt) >= 0) {
    delay(50);
    ESP.restart();
  }

  if (otaActive) {
    if (dnsActive) dnsServer.processNextRequest();
    server.handleClient();
    delay(1);
    return;
  }

  const bool rawPressed = triggerPressed();
  if (rawPressed != triggerRawPressed) {
    triggerRawPressed = rawPressed;
    triggerDebounceSince = nowMs;
  }

  if (rawPressed != triggerStablePressed &&
      (uint32_t)(nowMs - triggerDebounceSince) >= IDOTMATRIX_OTA_TRIGGER_DEBOUNCE_MS) {
    triggerStablePressed = rawPressed;
    if (triggerStablePressed) {
      triggerSince = nowMs;
      triggerLongActionDone = false;
#if DEBUG_SERIAL
      Serial.println("BOOT BUTTON: pressed");
#endif
    } else {
      const bool shortPress = !triggerLongActionDone;
      triggerSince = 0;
      if (shortPress) {
#if DEBUG_SERIAL
        Serial.println("BOOT BUTTON: short press -> reboot");
        Serial.flush();
#endif
        delay(30);
        ESP.restart();
        return;
      }
#if DEBUG_SERIAL
      Serial.println("BOOT BUTTON: released after OTA hold");
#endif
    }
  }

  if (triggerStablePressed && !triggerLongActionDone && triggerSince != 0 &&
      (uint32_t)(nowMs - triggerSince) >= IDOTMATRIX_OTA_TRIGGER_HOLD_MS) {
    triggerLongActionDone = true;
#if DEBUG_SERIAL
    Serial.println("BOOT BUTTON: long press -> OTA maintenance");
#endif
    startAccessPoint();
  }
}

bool idotOtaIsActive() {
  return otaActive;
}

const char *idotOtaStateText() {
  return otaActive ? "active" : "armed";
}

#endif
