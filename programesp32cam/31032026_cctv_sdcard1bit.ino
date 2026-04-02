/*
  ESP32-CAM — FreeRTOS Edition Non Observability
  Stream + VPS upload + SD recording + WiFiManager + OTA via web upload
  Board  : AI-Thinker (esp32cam)
  Partition: min_spiffs

  Library yang WAJIB diinstall (Library Manager):
    - WiFiManager by tzapu/tablatronix
*/

#include <Arduino.h>

#ifdef __cplusplus
extern "C" {
#endif
uint8_t temprature_sens_read(void);
#ifdef __cplusplus
}
#endif

#include "esp_camera.h"
#include <WiFi.h>
#include <WiFiManager.h>       // https://github.com/tzapu/WiFiManager
#include <WebServer.h>
#include <SPIFFS.h>
#include <HTTPClient.h>
#include <ArduinoJson.h>
#include <Update.h>
#include <HTTPUpdate.h>
#include "SD_MMC.h"
#include <time.h>
#include "esp_system.h"
#include "esp32/rom/ets_sys.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/semphr.h"

// ──────────────────────────────────────────────────────────────────────────────
// LOGGING
// ──────────────────────────────────────────────────────────────────────────────
#define LOG_I(tag, fmt, ...) Serial.printf("[INFO][%s] " fmt "\n", tag, ##__VA_ARGS__)
#define LOG_W(tag, fmt, ...) Serial.printf("[WARN][%s] " fmt "\n", tag, ##__VA_ARGS__)
#define LOG_E(tag, fmt, ...) Serial.printf("[ERR ][%s] " fmt "\n", tag, ##__VA_ARGS__)

// ──────────────────────────────────────────────────────────────────────────────
// VPS CONFIG
// ──────────────────────────────────────────────────────────────────────────────
const char* VPS_HOST = "103.31.205.52";
const int   VPS_PORT = 3001;

String CAMERA_ID   = "";
String DEVICE_ALIAS = "";

String URL_STATUS  = "";
String URL_UPLOAD  = "";
String URL_GALLERY = "";
String URL_SDINFO  = "";

// ──────────────────────────────────────────────────────────────────────────────
// WiFiManager Config
// ──────────────────────────────────────────────────────────────────────────────
#define AP_SSID_PREFIX  "ESP32CAM-Setup-"
#define AP_PASSWORD     "12345678"
#define AP_TIMEOUT      300        // detik — AP aktif sebelum ESP restart
#define WIFI_MAX_FAIL   5          // max gagal reconnect sebelum buka portal

WiFiManager wifiManager;
int         wifiFailCount = 0;

// ──────────────────────────────────────────────────────────────────────────────
// TIMING CONSTANTS
// ──────────────────────────────────────────────────────────────────────────────
const unsigned long STATUS_POLL_MS      = 2000UL;
const unsigned long SDINFO_INTERVAL     = 60000UL;
const unsigned long TIME_SYNC_INTERVAL  = 6UL * 3600UL * 1000UL;
const unsigned long TEMP_INTERVAL       = 10000UL;
const unsigned long WIFI_CHECK_INTERVAL = 10000UL;

const uint8_t  STREAM_FPS_MIN    = 1;
const uint8_t  STREAM_FPS_MAX    = 20;
const unsigned long THUMB_MIN_MS   = 1000UL;
const unsigned long GALLERY_MIN_MS = 60000UL;

// ──────────────────────────────────────────────────────────────────────────────
// CAMERA CONFIG (AI-Thinker)
// ──────────────────────────────────────────────────────────────────────────────
camera_config_t camCfg = {
  .pin_pwdn    = 32,
  .pin_reset   = -1,
  .pin_xclk    = 0,
  .pin_sscb_sda = 26,
  .pin_sscb_scl = 27,
  .pin_d7 = 35, .pin_d6 = 34, .pin_d5 = 39, .pin_d4 = 36,
  .pin_d3 = 21, .pin_d2 = 19, .pin_d1 = 18, .pin_d0 = 5,
  .pin_vsync = 25, .pin_href = 23, .pin_pclk = 22,
  .xclk_freq_hz = 20000000,
  .pixel_format = PIXFORMAT_JPEG,
  .frame_size   = FRAMESIZE_VGA,
  .jpeg_quality = 14,
  .fb_count     = 1
};

// ──────────────────────────────────────────────────────────────────────────────
// SHARED STATE (volatile untuk akses lintas task)
// ──────────────────────────────────────────────────────────────────────────────
volatile bool     serverRun        = false;
volatile uint8_t  streamFps        = 5;
volatile unsigned long thumbIntervalMs   = 60000UL;
volatile unsigned long galleryIntervalMs = 300000UL;

bool sdMounted = false;

// ──────────────────────────────────────────────────────────────────────────────
// FREERTOS MUTEX
// ──────────────────────────────────────────────────────────────────────────────
SemaphoreHandle_t camMutex;   // proteksi esp_camera_fb_get / fb_return
SemaphoreHandle_t sdMutex;    // proteksi akses SD_MMC
SemaphoreHandle_t wifiMutex;  // proteksi WiFi state read/write

// ──────────────────────────────────────────────────────────────────────────────
// SD RECORDING STATE
// ──────────────────────────────────────────────────────────────────────────────
#define RECORD_DURATION_MS  3600000UL  // 1 jam
#define RECORD_FPS          1

unsigned long lastRecordFrame    = 0;
unsigned long recordStartTime    = 0;
unsigned long lastRecordLog      = 0;
const unsigned long RECORD_LOG_INTERVAL = 30000UL;

File     recordFile;
bool     recordingActive  = false;
uint32_t recordFrameCount = 0;
uint32_t recordMoviDataSize = 0;

// ──────────────────────────────────────────────────────────────────────────────
// LOCAL WEBSERVER (OTA + stream + jpg)
// ──────────────────────────────────────────────────────────────────────────────
WebServer server(80);

// ──────────────────────────────────────────────────────────────────────────────
// HELPERS
// ──────────────────────────────────────────────────────────────────────────────
unsigned long msFromFps(uint8_t fps) {
  if (fps < 1) fps = 1;
  return 1000UL / fps;
}

// ──────────────────────────────────────────────────────────────────────────────
// DEVICE ID & ALIAS
// ──────────────────────────────────────────────────────────────────────────────
String generateDeviceID() {
  uint8_t mac[6];
  WiFi.macAddress(mac);
  char macStr[18];
  sprintf(macStr, "%02X%02X%02X%02X%02X%02X",
          mac[0], mac[1], mac[2], mac[3], mac[4], mac[5]);
  return String(macStr);
}

void loadDeviceAlias() {
  if (!SPIFFS.begin(true)) { DEVICE_ALIAS = ""; return; }
  if (!SPIFFS.exists("/alias.txt")) { DEVICE_ALIAS = ""; return; }
  File f = SPIFFS.open("/alias.txt", "r");
  if (!f) { DEVICE_ALIAS = ""; return; }
  DEVICE_ALIAS = f.readStringUntil('\n');
  DEVICE_ALIAS.trim();
  f.close();
  LOG_I("ALIAS", "loaded: %s", DEVICE_ALIAS.c_str());
}

void saveDeviceAlias(const String &alias) {
  if (!SPIFFS.begin(true)) { LOG_E("ALIAS", "SPIFFS fail"); return; }
  File f = SPIFFS.open("/alias.txt", "w");
  if (!f) { LOG_E("ALIAS", "open fail"); return; }
  f.println(alias);
  f.close();
  DEVICE_ALIAS = alias;
  LOG_I("ALIAS", "saved: %s", alias.c_str());
}

void constructURLs() {
  String base = String("http://") + VPS_HOST + ":" + String(VPS_PORT);
  URL_STATUS  = base + "/status?cam_id="       + CAMERA_ID;
  URL_UPLOAD  = base + "/upload?cam_id="        + CAMERA_ID;
  URL_GALLERY = base + "/galleryUpload?cam_id=" + CAMERA_ID;
  URL_SDINFO  = base + "/sdinfo?cam_id="        + CAMERA_ID;
  LOG_I("URL", "constructed for cam_id=%s", CAMERA_ID.c_str());
}

// ──────────────────────────────────────────────────────────────────────────────
// WIFI SETUP VIA WiFiManager
// ──────────────────────────────────────────────────────────────────────────────
void setup_wifi() {
  // Buat SSID AP unik dari 6 char terakhir MAC chip
  String chipId = String((uint32_t)(ESP.getEfuseMac() >> 24), HEX);
  chipId.toUpperCase();
  String apSsid = String(AP_SSID_PREFIX) + chipId;

  // Timeout AP — ESP restart otomatis jika tidak dikonfigurasi
  wifiManager.setConfigPortalTimeout(AP_TIMEOUT);
  // 20 detik coba connect sebelum buka AP
  wifiManager.setConnectTimeout(20);
  // 3x retry sebelum menyerah
  wifiManager.setConnectRetries(3);
  // Simpan & langsung connect setelah config
  wifiManager.setSaveConnect(true);
  // Tetap di AP sampai connect berhasil / timeout
  wifiManager.setBreakAfterConfig(false);

  LOG_I("WIFI", "WiFiManager: mencoba koneksi saved WiFi...");
  LOG_I("WIFI", "AP fallback: '%s' | pass: '%s' | timeout: %ds",
        apSsid.c_str(), AP_PASSWORD, AP_TIMEOUT);

  // autoConnect: jika sudah ada saved WiFi → langsung connect
  //              jika belum / gagal → buka AP portal
  bool connected = wifiManager.autoConnect(apSsid.c_str(), AP_PASSWORD);

  if (!connected) {
    LOG_E("WIFI", "WiFiManager timeout / gagal. Restart...");
    delay(1000);
    ESP.restart();
  }

  LOG_I("WIFI", "terhubung! IP=%s", WiFi.localIP().toString().c_str());
  wifiFailCount = 0;
}

// ──────────────────────────────────────────────────────────────────────────────
// WIFI RECONNECT (dipanggil dari taskNetMonitor)
// ──────────────────────────────────────────────────────────────────────────────
void wifi_reconnect() {
  if (WiFi.status() == WL_CONNECTED) {
    wifiFailCount = 0;
    return;
  }

  LOG_W("WIFI", "terputus, mencoba reconnect... (kegagalan ke-%d)", wifiFailCount + 1);
  WiFi.disconnect(false);
  WiFi.reconnect();

  unsigned long t = millis();
  while (WiFi.status() != WL_CONNECTED && millis() - t < 15000UL) {
    vTaskDelay(500 / portTICK_PERIOD_MS);
  }

  if (WiFi.status() == WL_CONNECTED) {
    LOG_I("WIFI", "reconnected! IP=%s, RSSI=%d",
          WiFi.localIP().toString().c_str(), WiFi.RSSI());
    wifiFailCount = 0;
  } else {
    wifiFailCount++;
    LOG_E("WIFI", "reconnect gagal (%d/%d)", wifiFailCount, WIFI_MAX_FAIL);

    if (wifiFailCount >= WIFI_MAX_FAIL) {
      LOG_W("WIFI", "Gagal %d kali. Membuka AP portal...", WIFI_MAX_FAIL);
      String chipId = String((uint32_t)(ESP.getEfuseMac() >> 24), HEX);
      chipId.toUpperCase();
      String apSsid = String(AP_SSID_PREFIX) + chipId;

      wifiManager.setConfigPortalTimeout(AP_TIMEOUT);
      bool ok = wifiManager.startConfigPortal(apSsid.c_str(), AP_PASSWORD);

      if (ok && WiFi.status() == WL_CONNECTED) {
        LOG_I("WIFI", "WiFi berhasil dikonfigurasi ulang!");
        wifiFailCount = 0;
      } else {
        LOG_W("WIFI", "Portal timeout. Restart...");
        delay(1000);
        ESP.restart();
      }
    }
  }
}

// ──────────────────────────────────────────────────────────────────────────────
// NTP TIME
// ──────────────────────────────────────────────────────────────────────────────
void initTime() {
  const long gmtOffset_sec = 7 * 3600;
  configTime(gmtOffset_sec, 0, "pool.ntp.org", "time.google.com", "time.cloudflare.com");
  LOG_I("TIME", "syncing...");
  time_t now = time(nullptr);
  int tries = 0;
  while (now < 1609459200 && tries < 40) {
    vTaskDelay(500 / portTICK_PERIOD_MS);
    now = time(nullptr);
    tries++;
  }
  if (now >= 1609459200) LOG_I("TIME", "NTP OK");
  else                    LOG_W("TIME", "NTP may have failed");
}

// ──────────────────────────────────────────────────────────────────────────────
// CPU TEMPERATURE
// ──────────────────────────────────────────────────────────────────────────────
float getCpuTemperature() {
  static float lastValidTemp = 0;
  uint8_t raw = temprature_sens_read();
  if (raw == 128) {
    return (lastValidTemp != 0) ? lastValidTemp : -127.0f;
  }
  float tempC = (raw - 32) / 1.8f;
  lastValidTemp = tempC;
  return tempC;
}

// ──────────────────────────────────────────────────────────────────────────────
// HTTP POST JPEG
// ──────────────────────────────────────────────────────────────────────────────
int httpPostJpeg(const String &url, uint8_t *buf, size_t len) {
  if (WiFi.status() != WL_CONNECTED) {
    LOG_W("HTTP", "skip POST, no WiFi");
    return -1;
  }
  HTTPClient http;
  http.setTimeout(3000);
  http.begin(url);
  http.addHeader("Content-Type", "image/jpeg");
  int code = http.sendRequest("POST", buf, len);
  http.end();
  if (code < 0) LOG_E("HTTP", "POST failed (%s)", url.c_str());
  return code;
}

// ──────────────────────────────────────────────────────────────────────────────
// CAPTURE & UPLOAD (dengan camMutex)
// ──────────────────────────────────────────────────────────────────────────────
bool captureAndUpload(const String &url, const char *tag) {
  camera_fb_t *fb = nullptr;

  if (xSemaphoreTake(camMutex, pdMS_TO_TICKS(2000)) != pdTRUE) {
    LOG_W(tag, "camMutex timeout");
    return false;
  }

  for (int r = 3; r > 0 && fb == nullptr; r--) {
    fb = esp_camera_fb_get();
    if (!fb && r > 1) {
      LOG_W(tag, "capture fail, retry %d", r - 1);
      vTaskDelay(100 / portTICK_PERIOD_MS);
    }
  }

  if (!fb) {
    xSemaphoreGive(camMutex);
    LOG_E(tag, "capture failed all retries");
    return false;
  }

  // Salin frame ke heap buffer agar camMutex bisa dilepas SEBELUM HTTP
  // → taskSDRecord tidak perlu menunggu network I/O selesai
  size_t len = fb->len;
  uint8_t *buf = (uint8_t*)malloc(len);
  if (!buf) {
    esp_camera_fb_return(fb);
    xSemaphoreGive(camMutex);
    LOG_E(tag, "malloc failed (%u bytes)", (unsigned)len);
    return false;
  }
  memcpy(buf, fb->buf, len);
  esp_camera_fb_return(fb);
  xSemaphoreGive(camMutex);  // ← lepas mutex SEBELUM HTTP (hanya tahan saat capture!)

  int code = httpPostJpeg(url, buf, len);
  free(buf);

  if (code >= 200 && code < 300) {
    LOG_I(tag, "OK (%u bytes)", (unsigned)len);
    return true;
  }
  LOG_W(tag, "FAIL HTTP %d", code);
  return false;
}

// ──────────────────────────────────────────────────────────────────────────────
// STATUS POLL
// ──────────────────────────────────────────────────────────────────────────────
void pollStatus() {
  if (WiFi.status() != WL_CONNECTED) {
    LOG_W("STATUS", "skip (no WiFi)");
    return;
  }
  HTTPClient http;
  http.setTimeout(3000);
  http.begin(URL_STATUS);
  int code = http.GET();
  if (code != 200) {
    LOG_W("STATUS", "HTTP %d", code);
    http.end();
    return;
  }
  String body = http.getString();
  http.end();

  StaticJsonDocument<256> doc;
  if (deserializeJson(doc, body)) {
    LOG_E("STATUS", "JSON parse error");
    return;
  }

  if (doc.containsKey("run"))             serverRun        = doc["run"].as<bool>();
  if (doc.containsKey("streamFps")) {
    int v = doc["streamFps"]; v = constrain(v, STREAM_FPS_MIN, STREAM_FPS_MAX);
    streamFps = v;
  }
  if (doc.containsKey("thumbIntervalMs")) {
    unsigned long v = doc["thumbIntervalMs"];
    thumbIntervalMs = max(v, THUMB_MIN_MS);
  }
  if (doc.containsKey("galleryIntervalMs")) {
    unsigned long v = doc["galleryIntervalMs"];
    galleryIntervalMs = max(v, GALLERY_MIN_MS);
  }

  LOG_I("STATUS", "run=%d fps=%u thumb=%lus gallery=%lus",
        serverRun, streamFps,
        thumbIntervalMs / 1000, galleryIntervalMs / 1000);
}

// ──────────────────────────────────────────────────────────────────────────────
// SD INFO SENDER
// ──────────────────────────────────────────────────────────────────────────────
void sendSdInfo() {
  if (!sdMounted || WiFi.status() != WL_CONNECTED) return;

  if (xSemaphoreTake(sdMutex, pdMS_TO_TICKS(2000)) != pdTRUE) {
    LOG_W("SD", "sdMutex timeout (sendSdInfo)");
    return;
  }
  uint64_t total = SD_MMC.totalBytes();
  uint64_t used  = SD_MMC.usedBytes();
  xSemaphoreGive(sdMutex);

  uint64_t freeB = total - used;
  String json = "{";
  json += "\"totalMB\":" + String(total / (1024*1024)) + ",";
  json += "\"usedMB\":"  + String(used  / (1024*1024)) + ",";
  json += "\"freeMB\":"  + String(freeB / (1024*1024));
  json += "}";

  HTTPClient http;
  http.begin(URL_SDINFO);
  http.addHeader("Content-Type", "application/json");
  int code = http.POST(json);
  http.end();
  LOG_I("SD", "info sent HTTP %d", code);
}

// Temperature telemetry sengaja dinonaktifkan pada fase 1.
// Server VPS aktif belum menyediakan endpoint /temperature.

// ──────────────────────────────────────────────────────────────────────────────
// SD AVI RECORDING HELPERS
// ──────────────────────────────────────────────────────────────────────────────
String getTimestampString() {
  time_t now; time(&now);
  struct tm info; localtime_r(&now, &info);
  char buf[32];
  sprintf(buf, "%04d%02d%02d_%02d%02d%02d",
          info.tm_year+1900, info.tm_mon+1, info.tm_mday,
          info.tm_hour, info.tm_min, info.tm_sec);
  return String(buf);
}

void write32LE(File &f, uint32_t val) {
  f.write((uint8_t)(val & 0xFF));
  f.write((uint8_t)((val >> 8)  & 0xFF));
  f.write((uint8_t)((val >> 16) & 0xFF));
  f.write((uint8_t)((val >> 24) & 0xFF));
}
void write16LE(File &f, uint16_t val) {
  f.write((uint8_t)(val & 0xFF));
  f.write((uint8_t)((val >> 8) & 0xFF));
}

void writeAVIHeader(File &f) {
  f.write((const uint8_t*)"RIFF", 4); write32LE(f, 0);
  f.write((const uint8_t*)"AVI ", 4);
  f.write((const uint8_t*)"LIST", 4); write32LE(f, 192);
  f.write((const uint8_t*)"hdrl", 4);
  f.write((const uint8_t*)"avih", 4); write32LE(f, 56);
  write32LE(f, 1000000 / RECORD_FPS);
  write32LE(f, 0); write32LE(f, 0); write32LE(f, 0x10);
  write32LE(f, 0); write32LE(f, 0); write32LE(f, 1);
  write32LE(f, 0); write32LE(f, 320); write32LE(f, 240);
  write32LE(f, 0); write32LE(f, 0); write32LE(f, 0); write32LE(f, 0);
  f.write((const uint8_t*)"LIST", 4); write32LE(f, 124);
  f.write((const uint8_t*)"strl", 4);
  f.write((const uint8_t*)"strh", 4); write32LE(f, 56);
  f.write((const uint8_t*)"vids", 4); f.write((const uint8_t*)"MJPG", 4);
  write32LE(f, 0); write16LE(f, 0); write16LE(f, 0);
  write32LE(f, 0); write32LE(f, 1); write32LE(f, RECORD_FPS);
  write32LE(f, 0); write32LE(f, 0); write32LE(f, 0);
  write32LE(f, (uint32_t)-1); write32LE(f, 0);
  write16LE(f, 0); write16LE(f, 0); write16LE(f, 320); write16LE(f, 240);
  f.write((const uint8_t*)"strf", 4); write32LE(f, 40);
  write32LE(f, 40); write32LE(f, 320); write32LE(f, 240);
  write16LE(f, 1); write16LE(f, 24);
  f.write((const uint8_t*)"MJPG", 4);
  write32LE(f, 320*240*3);
  write32LE(f, 0); write32LE(f, 0); write32LE(f, 0); write32LE(f, 0);
  f.write((const uint8_t*)"LIST", 4); write32LE(f, 4);
  f.write((const uint8_t*)"movi", 4);
}

void updateAVIHeader(File &f) {
  uint32_t fileSize = f.size();
  f.seek(4);   write32LE(f, fileSize - 8);
  f.seek(48);  write32LE(f, recordFrameCount);
  f.seek(140); write32LE(f, recordFrameCount);
  f.seek(220); write32LE(f, recordMoviDataSize + 4);
  f.seek(0, SeekEnd);
}

void startNewRecordingFile() {
  if (!sdMounted) return;
  if (!SD_MMC.exists("/video")) SD_MMC.mkdir("/video");
  String filename = "/video/rec_" + getTimestampString() + ".avi";
  recordFile = SD_MMC.open(filename, FILE_WRITE);
  if (!recordFile) { LOG_E("REC", "open failed"); recordingActive = false; return; }
  writeAVIHeader(recordFile);
  recordingActive     = true;
  recordStartTime     = millis();
  recordFrameCount    = 0;
  recordMoviDataSize  = 0;
  lastRecordFrame     = 0;
  lastRecordLog       = millis();
  LOG_I("REC", "AVI created: %s", filename.c_str());
}

// handleRecording dipanggil dari dalam taskSDRecord — camMutex + sdMutex sudah dipegang oleh caller
void handleRecordingLocked() {
  if (!sdMounted) return;
  if (!recordingActive) { startNewRecordingFile(); return; }

  if (millis() - recordStartTime > RECORD_DURATION_MS) {
    LOG_I("REC", "duration reached, finalizing AVI...");
    updateAVIHeader(recordFile);
    recordFile.flush();
    recordFile.close();
    recordingActive = false;
    return;
  }

  if (RECORD_FPS > 0 && millis() - lastRecordFrame < (1000UL / RECORD_FPS)) return;
  lastRecordFrame = millis();

  camera_fb_t *fb = esp_camera_fb_get();
  if (!fb) { LOG_W("REC", "frame capture failed"); return; }

  recordFile.write((const uint8_t*)"00dc", 4);
  write32LE(recordFile, fb->len);
  size_t written = recordFile.write(fb->buf, fb->len);
  if (fb->len % 2 == 1) recordFile.write((uint8_t)0);

  uint32_t chunkSize = fb->len + 8 + (fb->len % 2 == 1 ? 1 : 0);
  recordFrameCount++;
  recordMoviDataSize += chunkSize;

  if (recordFrameCount % 10 == 0) recordFile.flush();
  esp_camera_fb_return(fb);

  if (written != fb->len) {
    LOG_E("REC", "SD write error, closing file");
    updateAVIHeader(recordFile);
    recordFile.flush();
    recordFile.close();
    recordingActive = false;
  }

  if (millis() - lastRecordLog > RECORD_LOG_INTERVAL) {
    LOG_I("REC", "recording... %lus, %u frames, %u KB",
          (millis() - recordStartTime) / 1000, recordFrameCount, recordMoviDataSize / 1024);
    lastRecordLog = millis();
  }
}

// ──────────────────────────────────────────────────────────────────────────────
// LOCAL WEBSERVER — HANDLERS
// ──────────────────────────────────────────────────────────────────────────────
void handleJPG() {
  if (xSemaphoreTake(camMutex, pdMS_TO_TICKS(2000)) != pdTRUE) {
    server.send(503, "text/plain", "Camera busy");
    return;
  }
  camera_fb_t *fb = esp_camera_fb_get();
  if (!fb) {
    xSemaphoreGive(camMutex);
    server.send(500, "text/plain", "Camera error");
    return;
  }
  server.sendHeader("Content-Type", "image/jpeg");
  server.send(200);
  server.client().write(fb->buf, fb->len);
  esp_camera_fb_return(fb);
  xSemaphoreGive(camMutex);
}

void handleStream() {
  WiFiClient client = server.client();
  client.print("HTTP/1.1 200 OK\r\nContent-Type: multipart/x-mixed-replace; boundary=frame\r\n\r\n");
  while (client.connected()) {
    if (xSemaphoreTake(camMutex, pdMS_TO_TICKS(500)) != pdTRUE) {
      vTaskDelay(10 / portTICK_PERIOD_MS);
      continue;
    }
    camera_fb_t *fb = esp_camera_fb_get();
    if (!fb) {
      xSemaphoreGive(camMutex);
      vTaskDelay(10 / portTICK_PERIOD_MS);
      continue;
    }
    client.print("--frame\r\n");
    client.print("Content-Type: image/jpeg\r\n");
    client.printf("Content-Length: %u\r\n\r\n", fb->len);
    client.write(fb->buf, fb->len);
    client.print("\r\n");
    esp_camera_fb_return(fb);
    xSemaphoreGive(camMutex);
    vTaskDelay(5 / portTICK_PERIOD_MS);
  }
}

// ── OTA via web upload ──
void forceStopAll() {
  LOG_W("FORCE", "stopping camera and recording...");
  serverRun = false;
  if (recordingActive) {
    recordingActive = false;
    if (recordFile) { recordFile.flush(); recordFile.close(); }
  }
  esp_camera_deinit();
  vTaskDelay(200 / portTICK_PERIOD_MS);
}

const char OTA_HTML[] PROGMEM = R"rawliteral(
<!DOCTYPE html><html><head><title>ESP32-CAM OTA</title></head>
<body style="font-family:Arial;text-align:center;margin-top:30px;">
<h2>OTA Update Firmware</h2>
<form method='POST' action='/update' enctype='multipart/form-data'>
<input type='file' name='firmware' required><br><br>
<button>Upload Firmware</button>
</form></body></html>
)rawliteral";

const char OTA_SUCCESS[] PROGMEM = R"rawliteral(
<!DOCTYPE html><html><body style="font-family:Arial;text-align:center;margin-top:30px;">
<h2 style="color:green;">OTA SUCCESS</h2><p>Rebooting...</p></body></html>
)rawliteral";

void handleUpdatePage() { server.send(200, "text/html", OTA_HTML); }

void handleOTAUpload() {
  HTTPUpload &upload = server.upload();
  if (upload.status == UPLOAD_FILE_START) {
    uint32_t freeHeap = ESP.getFreeHeap();
    LOG_I("OTA", "Pre-check: Free heap = %u bytes", freeHeap);
    if (freeHeap < 100000) {
      LOG_E("OTA", "Insufficient memory: %u bytes", freeHeap);
      server.send(500, "text/plain", "OTA failed: not enough memory");
      return;
    }
    LOG_I("OTA", "Start: %s", upload.filename.c_str());
    forceStopAll();
    SD_MMC.end();
    SPIFFS.end();
    vTaskDelay(100 / portTICK_PERIOD_MS);
    size_t size = upload.totalSize;
    bool ok = (size > 0) ? Update.begin(size) : false;
    if (!ok && !Update.begin(UPDATE_SIZE_UNKNOWN)) {
      server.send(500, "text/plain", "OTA begin failed");
      delay(1000); ESP.restart(); return;
    }
    LOG_I("OTA", "Update.begin OK");
  } else if (upload.status == UPLOAD_FILE_WRITE) {
    if (Update.write(upload.buf, upload.currentSize) != (int)upload.currentSize) {
      Update.end();
      server.send(500, "text/plain", "OTA write failed");
      delay(2000); ESP.restart(); return;
    }
  } else if (upload.status == UPLOAD_FILE_END) {
    if (Update.end(true)) {
      LOG_I("OTA", "Success: %u bytes", (unsigned)upload.totalSize);
      server.send(200, "text/html", OTA_SUCCESS);
      delay(1200); ESP.restart();
    } else {
      server.send(500, "text/plain", String("OTA Failed: ") + Update.errorString());
      delay(2000); ESP.restart();
    }
  } else if (upload.status == UPLOAD_FILE_ABORTED) {
    Update.end();
    server.send(500, "text/plain", "OTA Aborted");
    delay(500); ESP.restart();
  }
}

void setupWebServerRoutes() {
  server.on("/", []() {
    String txt = "ESP32-CAM (FreeRTOS)\n";
    txt += "/jpg    -> snapshot\n";
    txt += "/stream -> mjpeg stream\n";
    txt += "/update -> OTA firmware upload\n";
    txt += "/memory -> system info (JSON)\n";
    txt += "/info   -> device info (JSON)\n";
    txt += "/alias  -> get/set device alias\n";
    txt += "/resetwifi -> reset WiFi via WiFiManager\n";
    server.send(200, "text/plain", txt);
  });

  server.on("/jpg",    HTTP_GET, handleJPG);
  server.on("/stream", HTTP_GET, handleStream);
  server.on("/update", HTTP_GET, handleUpdatePage);
  server.on("/update", HTTP_POST, [](){}, handleOTAUpload);

  server.on("/memory", HTTP_GET, []() {
    StaticJsonDocument<256> doc;
    doc["freeHeap"]   = ESP.getFreeHeap();
    doc["heapSize"]   = ESP.getHeapSize();
    doc["freePsram"]  = ESP.getFreePsram();
    doc["psramSize"]  = ESP.getPsramSize();
    doc["uptime"]     = millis() / 1000;
    doc["recording"]  = recordingActive;
    doc["wifiRSSI"]   = WiFi.RSSI();
    String json; serializeJson(doc, json);
    server.send(200, "application/json", json);
  });

  // Reset WiFi via WiFiManager
  server.on("/resetwifi", HTTP_GET, []() {
    server.send(200, "text/html",
      "<html><body style='font-family:Arial;text-align:center;margin-top:50px;'>"
      "<h2>WiFi Reset</h2>"
      "<p>Menghapus config WiFi & membuka AP portal...</p>"
      "<p>SSID: <b>ESP32CAM-Setup-XXXXXX</b> | Pass: <b>12345678</b></p>"
      "</body></html>");
    delay(1000);
    wifiManager.resetSettings();
    delay(500);
    ESP.restart();
  });

  server.on("/info", HTTP_GET, []() {
    StaticJsonDocument<256> doc;
    doc["deviceId"] = CAMERA_ID;
    doc["alias"]    = DEVICE_ALIAS;
    doc["ip"]       = WiFi.localIP().toString();
    doc["mac"]      = WiFi.macAddress();
    doc["rssi"]     = WiFi.RSSI();
    doc["ssid"]     = WiFi.SSID();
    String json; serializeJson(doc, json);
    server.send(200, "application/json", json);
  });

  server.on("/alias", HTTP_GET, []() {
    StaticJsonDocument<128> doc;
    doc["deviceId"] = CAMERA_ID;
    doc["alias"]    = DEVICE_ALIAS;
    String json; serializeJson(doc, json);
    server.send(200, "application/json", json);
  });

  server.on("/alias", HTTP_POST, []() {
    if (!server.hasArg("plain")) { server.send(400, "text/plain", "Missing body"); return; }
    StaticJsonDocument<128> doc;
    if (deserializeJson(doc, server.arg("plain"))) { server.send(400, "text/plain", "Invalid JSON"); return; }
    if (!doc.containsKey("alias")) { server.send(400, "text/plain", "Missing alias"); return; }
    String newAlias = doc["alias"].as<String>();
    newAlias.trim();
    saveDeviceAlias(newAlias);
    doc.clear();
    doc["deviceId"] = CAMERA_ID;
    doc["alias"]    = DEVICE_ALIAS;
    doc["status"]   = "saved";
    String json; serializeJson(doc, json);
    server.send(200, "application/json", json);
  });

  server.begin();
  LOG_I("WEB", "local webserver started on port 80");
}

// ──────────────────────────────────────────────────────────────────────────────
// ███████╗██████╗ ███████╗███████╗██████╗ ████████╗ ██████╗ ███████╗
// ██╔════╝██╔══██╗██╔════╝██╔════╝██╔══██╗╚══██╔══╝██╔═══██╗██╔════╝
// █████╗  ██████╔╝█████╗  █████╗  ██████╔╝   ██║   ██║   ██║███████╗
// ██╔══╝  ██╔══██╗██╔══╝  ██╔══╝  ██╔══██╗   ██║   ██║   ██║╚════██║
// ██║     ██║  ██║███████╗███████╗██║  ██║   ██║   ╚██████╔╝███████║
// FREERTOS TASKS
// ──────────────────────────────────────────────────────────────────────────────

// ── TASK 1: Network Monitor (Core 1, Priority 2) ──────────────────────────────
// Cek koneksi WiFi setiap WIFI_CHECK_INTERVAL. Jika putus → reconnect atau buka portal.
void taskNetMonitor(void *pvParam) {
  LOG_I("NET", "task started on core %d", xPortGetCoreID());
  TickType_t lastCheck = xTaskGetTickCount();

  for (;;) {
    vTaskDelayUntil(&lastCheck, pdMS_TO_TICKS(WIFI_CHECK_INTERVAL));
    wifi_reconnect();
  }
}

// ── TASK 2: Status Poll + Upload (Core 1, Priority 3) ─────────────────────────
// Poll status VPS, upload JPEG (stream/thumb/gallery), kirim suhu.
void taskStatusUpload(void *pvParam) {
  LOG_I("STATUS", "task started on core %d", xPortGetCoreID());

  unsigned long lastStatusPoll  = 0;
  unsigned long lastStreamSent  = 0;
  unsigned long lastThumbSent   = 0;
  unsigned long lastGallerySent = 0;
  unsigned long lastTimeSync    = 0;

  // Initial poll
  vTaskDelay(pdMS_TO_TICKS(3000));  // tunggu WiFi settle
  pollStatus();
  lastStatusPoll = millis();

  for (;;) {
    unsigned long now = millis();

    // ── NTP sync berkala ──
    if (WiFi.status() == WL_CONNECTED && now - lastTimeSync >= TIME_SYNC_INTERVAL) {
      initTime();
      lastTimeSync = now;
    }

    // ── Poll status dari VPS ──
    if (now - lastStatusPoll >= STATUS_POLL_MS) {
      lastStatusPoll = now;
      pollStatus();
    }

    // ── Upload JPEG ke VPS ──
    if (serverRun) {
      unsigned long interval = msFromFps(streamFps);
      if (now - lastStreamSent >= interval) {
        lastStreamSent = now;
        captureAndUpload(URL_UPLOAD, "STREAM");
      }
    } else {
      if (now - lastThumbSent >= thumbIntervalMs) {
        lastThumbSent = now;
        captureAndUpload(URL_UPLOAD, "THUMB");
      }
    }

    if (now - lastGallerySent >= galleryIntervalMs) {
      lastGallerySent = now;
      captureAndUpload(URL_GALLERY, "GALLERY");
    }

    vTaskDelay(pdMS_TO_TICKS(10));
  }
}

// ── TASK 3: Local Web Server (Core 1, Priority 2) ─────────────────────────────
// Melayani request HTTP lokal: OTA upload, /jpg, /stream, /info, dll.
void taskLocalServer(void *pvParam) {
  LOG_I("WEB", "task started on core %d", xPortGetCoreID());
  for (;;) {
    server.handleClient();
    vTaskDelay(pdMS_TO_TICKS(5));
  }
}

// ── TASK 4: SD Card Recording (Core 0, Priority 1) ────────────────────────────
// Record video AVI ke SD card. Ambil camMutex sebelum capture, sdMutex sebelum write.
void taskSDRecord(void *pvParam) {
  LOG_I("REC", "task started on core %d", xPortGetCoreID());

  // Tunggu SD mounted
  unsigned long waitStart = millis();
  while (!sdMounted && millis() - waitStart < 30000UL) {
    vTaskDelay(pdMS_TO_TICKS(500));
  }
  if (!sdMounted) {
    LOG_W("REC", "SD not mounted after 30s, task exits");
    vTaskDelete(NULL);
    return;
  }

  for (;;) {
    // Ambil kedua mutex sekaligus — camMutex dulu, lalu sdMutex
    if (xSemaphoreTake(camMutex, pdMS_TO_TICKS(1000)) == pdTRUE) {
      if (xSemaphoreTake(sdMutex, pdMS_TO_TICKS(1000)) == pdTRUE) {
        handleRecordingLocked();
        xSemaphoreGive(sdMutex);
      }
      xSemaphoreGive(camMutex);
    } else {
      LOG_W("REC", "camMutex busy, skip frame");
    }

    // Delay sesuai RECORD_FPS
    vTaskDelay(pdMS_TO_TICKS(1000 / RECORD_FPS));
  }
}

// ── TASK 5: SD Info Sender (Core 1, Priority 1) ───────────────────────────────
// Kirim info SD card (total/used/free) ke VPS setiap SDINFO_INTERVAL.
void taskSdInfo(void *pvParam) {
  LOG_I("SDINFO", "task started on core %d", xPortGetCoreID());
  // Tunggu koneksi WiFi & SD mount
  vTaskDelay(pdMS_TO_TICKS(10000));
  for (;;) {
    sendSdInfo();
    vTaskDelay(pdMS_TO_TICKS(SDINFO_INTERVAL));
  }
}

// ──────────────────────────────────────────────────────────────────────────────
// SETUP
// ──────────────────────────────────────────────────────────────────────────────
void setup() {
  delay(2000); // power settle
  Serial.begin(115200);
  delay(200);

  esp_reset_reason_t reason = esp_reset_reason();
  Serial.printf("[BOOT] reset reason = %d\n", reason);

  // Hanya restart saat cold boot / brownout (sekali saja)
  if (reason == ESP_RST_POWERON || reason == ESP_RST_BROWNOUT) {
    static bool alreadyRestarted = false;
    if (!alreadyRestarted) {
      alreadyRestarted = true;
      Serial.println("[BOOT] cold boot detected, doing one clean restart");
      delay(200);
      ESP.restart();
    }
  }

  Serial.println(F("\n╔══════════════════════════════════════════════╗"));
  Serial.println(F(  "║   ESP32-CAM — FreeRTOS + WiFiManager        ║"));
  Serial.println(F(  "║   Stream + SD Record + VPS Upload + OTA     ║"));
  Serial.println(F(  "╚══════════════════════════════════════════════╝"));

  // ── SPIFFS ──
  if (!SPIFFS.begin(true)) LOG_W("SPIFFS", "mount failed");

  // ── Device ID (sebelum WiFi — MAC bisa dibaca tanpa connect) ──
  CAMERA_ID = generateDeviceID();
  loadDeviceAlias();
  Serial.printf("[DEVICE] ID: %s", CAMERA_ID.c_str());
  if (DEVICE_ALIAS.length() > 0) Serial.printf(" (Alias: %s)", DEVICE_ALIAS.c_str());
  Serial.println();
  constructURLs();

  // ── Buat FreeRTOS Mutex ──
  camMutex  = xSemaphoreCreateMutex();
  sdMutex   = xSemaphoreCreateMutex();
  wifiMutex = xSemaphoreCreateMutex();

  if (!camMutex || !sdMutex || !wifiMutex) {
    LOG_E("RTOS", "Mutex creation failed! Halting.");
    while (1) delay(1000);
  }
  LOG_I("RTOS", "Mutex created OK");

  // ── WiFi via WiFiManager ──
  setup_wifi();

  // ── NTP ──
  if (WiFi.status() == WL_CONNECTED) initTime();

  // ── Camera Init ──
  if (esp_camera_init(&camCfg) != ESP_OK) {
    LOG_E("CAM", "INIT FAILED — halting");
    while (1) delay(1000);
  }
  LOG_I("CAM", "INIT OK");
  sensor_t *s = esp_camera_sensor_get();
  s->set_vflip(s, 1);
  s->set_hmirror(s, 1);

  // ── Local Web Server ──
  setupWebServerRoutes();

  // ── Flash LED (GPIO4 = DAT1 pada mode 4-bit) ──
  // Matikan flash LED SEBELUM inisiasi SD agar tidak konflik pin
  // Di mode 1-bit, GPIO4 bebas → set OUTPUT LOW untuk memadamkan LED
  pinMode(4, OUTPUT);
  digitalWrite(4, LOW);

  // ── SD Card (mode 1-bit: parameter kedua = true) ──
  // Mode 1-bit membebaskan GPIO4 (DAT1) sehingga tidak menghidupkan flash LED
  sdMounted = SD_MMC.begin("/sdcard", true);
  if (sdMounted) {
    LOG_I("SD", "mounted OK (1-bit mode)");
  } else {
    LOG_W("SD", "not available");
  }

  // ── Create FreeRTOS Tasks ──
  //                             func              name             stack  param prio  handle  core
  xTaskCreatePinnedToCore(taskNetMonitor,    "NetMonitor",   4096, NULL, 2, NULL, 1);
  xTaskCreatePinnedToCore(taskStatusUpload,  "StatusUpload", 8192, NULL, 3, NULL, 1);
  xTaskCreatePinnedToCore(taskLocalServer,   "LocalServer",  8192, NULL, 2, NULL, 1);
  xTaskCreatePinnedToCore(taskSdInfo,        "SdInfo",       4096, NULL, 1, NULL, 1);
  xTaskCreatePinnedToCore(taskSDRecord,      "SDRecord",     4096, NULL, 1, NULL, 0);

  LOG_I("RTOS", "All tasks created. FreeRTOS scheduler running.");
  LOG_I("RTOS", "Free heap after setup: %u bytes", ESP.getFreeHeap());
  
  // Flash LED sudah dimatikan sebelum SD init (lihat di atas)
}

// ──────────────────────────────────────────────────────────────────────────────
// LOOP — kosong, semua logika ada di FreeRTOS tasks
// ──────────────────────────────────────────────────────────────────────────────
void loop() {
  // Semua pekerjaan dijalankan oleh FreeRTOS tasks.
  // loop() ditidurkan selamanya untuk membebaskan Core 1 bagi scheduler.
  vTaskDelay(portMAX_DELAY);
}
