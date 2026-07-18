/* ============================================================
 *  Travel Jellyfin CYD — ESP32-035 (3.5" resistive touch)
 *  Portrait 320x480, swipe left/right to change pages.
 *  Pages: 0=Status, 1=Now Playing, 2=Kodi Remote, 3=WiFi QR, 4=System
 * ============================================================ */

#include <Arduino.h>
#include <WiFi.h>
#include <HTTPClient.h>
#include <ArduinoJson.h>
#include <TFT_eSPI.h>
#include <SPI.h>

#include "config.h"

TFT_eSPI tft = TFT_eSPI();

uint16_t calData[] = {
  TOUCH_CAL_LEFT, TOUCH_CAL_RIGHT,
  TOUCH_CAL_TOP,  TOUCH_CAL_BOT,
  TOUCH_CAL_FLAG
};

// API state
String apiHost   = PI_API_HOST_HOTSPOT;
int    apiPort   = PI_API_PORT;
unsigned long lastPoll   = 0;
bool    apiOnline       = false;
bool    kodiWasRunning  = false;
int     apiFailCount    = 0;        // consecutive poll failures
String  lastSeenTime    = "";       // last successful poll time (HH:MM:SS)
bool    errorOverlay    = false;    // showing PI UNREACHABLE overlay

// Status JSON fields
String  s_mode = "car";
bool    s_hdmi = false, s_kodi = false, s_jellyfin = false, s_hotspot = false;
float   s_cpu_temp = 0, s_nvme_temp = 0, s_cpu_voltage = 0;
int     s_cpu_clock = 0;
float   s_mem_used = 0, s_mem_total = 0, s_load1 = 0;
String  s_uptime = "", s_eth0_ip = "", s_wlan0_ip = "";
int     s_nvme_used = 0, s_nvme_total = 0, s_sd_used = 0, s_sd_total = 0;
int     s_kodi_volume = -1;         // -1 = unknown/unreachable
int     s_now_playing_count = 0;

// Now Playing data (fetched separately every 3s)
#define MAX_STREAMS 4
struct NPStream {
  String title;
  String device;
  String user;
  int    position_sec;
  int    duration_sec;
  int    progress_pct;
  bool   paused;
  String type;
};
NPStream npStreams[MAX_STREAMS];
int      npCount = 0;
unsigned long lastNowPlayingPoll = 0;
bool         npNeedsRedraw = false;

// UI state
int     currentPage = 0;
const int numPages = 5;
bool    pageNeedsRedraw = true;
bool    needsFullRedraw = true;  // true on page change, false on data update

// Touch state
bool    touchWasActive = false;
int     touchStartX = 0, touchStartY = 0;
int     lastTouchX = 0, lastTouchY = 0;
unsigned long touchStartTime = 0;
unsigned long lastTouchTime = 0;   // for backlight dim
bool    backlightDimmed = false;
#define SWIPE_THRESHOLD  35
#define TAP_THRESHOLD    25
#define TAP_TIMEOUT       600

// System page confirmation state
String  sysConfirmAction = "";          // "reboot", "shutdown", or ""
unsigned long sysConfirmTime = 0;       // when first tap happened
#define SYS_CONFIRM_TIMEOUT 3000        // 3s to confirm

// Colors — matching web interface
#define COL_BG       TFT_BLACK
#define COL_HEADER   0x06A4E0
#define COL_LABEL    0x8C8C8C
#define COL_VALUE    TFT_WHITE
#define COL_OK       0x2EE670
#define COL_WARN     0xF3A012
#define COL_ERR      0xE74C3C
#define COL_CARD_BG  0x080810
#define COL_CARD_BD  0x1A1A2E
#define COL_MODE_CAR 0x229954
#define COL_MODE_HOT 0xC0392B
#define COL_CYAN     TFT_CYAN
#define COL_PURPLE   0xB04AEB

struct Btn { int x, y, w, h; const char* label; };

// ---- Helpers ----

void setLED(uint8_t r, uint8_t g, uint8_t b) {
  digitalWrite(LED_R, r ? LOW : HIGH);
  digitalWrite(LED_G, g ? LOW : HIGH);
  digitalWrite(LED_B, b ? LOW : HIGH);
}

uint16_t tempColor(float t) {
  if (t > 65) return COL_ERR;
  if (t > 50) return COL_WARN;
  return COL_OK;
}

void drawCard(int x, int y, int w, int h) {
  tft.fillRoundRect(x, y, w, h, 8, COL_CARD_BG);
  tft.drawRoundRect(x, y, w, h, 8, COL_CARD_BD);
}

void drawCardHeader(const char* title, int x, int y) {
  tft.setTextSize(1);
  tft.setTextColor(COL_HEADER, COL_CARD_BG);
  tft.setCursor(x + 12, y + 8);
  tft.print(title);
}

void drawRow(const char* label, const char* value, int x, int y, int w, uint16_t valColor) {
  tft.setTextSize(1);
  tft.setTextColor(COL_LABEL, COL_CARD_BG);
  tft.setCursor(x + 12, y);
  tft.print(label);
  tft.setTextColor(valColor, COL_CARD_BG);
  tft.setCursor(x + w - 12 - tft.textWidth(value), y);
  tft.print(value);
}

void drawRowBar(const char* label, const char* value, int pct, int x, int y, int w, uint16_t barColor) {
  drawRow(label, value, x, y, w, COL_VALUE);
  int barY = y + 12, barX = x + 12, barW = w - 24, barH = 4;
  tft.fillRoundRect(barX, barY, barW, barH, 2, 0x222222);
  if (pct > 0 && pct <= 100) {
    tft.fillRoundRect(barX, barY, barW * pct / 100, barH, 2, barColor);
  }
}

void drawBtn(const Btn& b, uint16_t border, uint16_t fill, uint16_t textCol) {
  tft.fillRoundRect(b.x, b.y, b.w, b.h, 6, fill);
  tft.drawRoundRect(b.x, b.y, b.w, b.h, 6, border);
  tft.setTextSize(2);
  tft.setTextColor(textCol, fill);
  uint16_t tw = tft.textWidth(b.label);
  uint16_t th = tft.fontHeight();
  tft.setCursor(b.x + (b.w - tw) / 2, b.y + (b.h - th) / 2 - 2);
  tft.print(b.label);
}

bool hitBtn(const Btn& b, int tx, int ty) {
  return tx >= b.x && tx < b.x + b.w && ty >= b.y && ty < b.y + b.h;
}

// Format seconds as M:SS or H:MM:SS
String formatTime(int seconds) {
  if (seconds < 0) seconds = 0;
  int h = seconds / 3600;
  int m = (seconds % 3600) / 60;
  int s = seconds % 60;
  char buf[16];
  if (h > 0) snprintf(buf, sizeof(buf), "%d:%02d:%02d", h, m, s);
  else       snprintf(buf, sizeof(buf), "%d:%02d", m, s);
  return String(buf);
}

String clockString() {
  // HH:MM:SS from millis() uptime (rough, for "last seen" display)
  unsigned long t = millis() / 1000;
  int h = (t / 3600) % 24;
  int m = (t / 60) % 60;
  int s = t % 60;
  char buf[16];
  snprintf(buf, sizeof(buf), "%02d:%02d:%02d", h, m, s);
  return String(buf);
}

// ---- WiFi ----

void connectWiFi() {
  const char* ssids[]  = {WIFI_SSID_HOTSPOT, WIFI_SSID_HOME};
  const char* passes[] = {WIFI_PASS_HOTSPOT, WIFI_PASS_HOME};

  for (int i = 0; i < 2; i++) {
    if (strlen(ssids[i]) == 0) continue;
    Serial.printf("Connecting to \"%%s\" ...\n", ssids[i]);
    WiFi.mode(WIFI_STA);
    WiFi.begin(ssids[i], passes[i]);
    int attempts = 0;
    while (WiFi.status() != WL_CONNECTED && attempts < 20) {
      delay(500); attempts++; setLED(0, 0, 1);
    }
    if (WiFi.status() == WL_CONNECTED) {
      Serial.printf("Connected! IP: %s\n", WiFi.localIP().toString().c_str());
      apiHost = (strcmp(ssids[i], WIFI_SSID_HOTSPOT) == 0) ? PI_API_HOST_HOTSPOT : PI_API_HOST_HOME;
      setLED(0, 1, 0);
      return;
    }
  }
  Serial.println("WiFi connect failed");
  setLED(1, 0, 0);
}

// ---- API ----

bool fetchStatus() {
  WiFiClient client;
  HTTPClient http;
  String url = String("http://") + apiHost + ":" + apiPort + "/api/status";
  http.begin(client, url);
  http.addHeader("X-API-Key", PI_API_KEY);
  http.setTimeout(4000);
  int code = http.GET();
  if (code != 200) { http.end(); return false; }
  String body = http.getString();
  http.end();
  JsonDocument doc;
  if (deserializeJson(doc, body)) { return false; }
  s_mode = doc["mode"] | "car";
  s_hdmi = doc["hdmi_connected"] | false;
  s_kodi = doc["kodi_running"] | false;
  s_jellyfin = doc["jellyfin_running"] | false;
  s_hotspot = doc["hotspot_running"] | false;
  s_cpu_temp = doc["cpu_temp"] | 0.0f;
  s_nvme_temp = doc["nvme_temp"] | 0.0f;
  s_cpu_clock = doc["cpu_clock"] | 0;
  s_cpu_voltage = doc["cpu_voltage"] | 0.0f;
  s_mem_used = doc["memory_used_gb"] | 0.0f;
  s_mem_total = doc["memory_total_gb"] | 0.0f;
  s_load1 = doc["load_1"] | 0.0f;
  s_uptime = doc["uptime"] | "?";
  s_eth0_ip = doc["eth0_ip"] | "";
  s_wlan0_ip = doc["wlan0_ip"] | "";
  s_nvme_used = doc["nvme_used_gb"] | 0;
  s_nvme_total = doc["nvme_total_gb"] | 0;
  s_sd_used = doc["sd_used_gb"] | 0;
  s_sd_total = doc["sd_total_gb"] | 0;
  // New fields (may be absent on older API versions)
  if (doc["kodi_volume"].is<int>()) {
    s_kodi_volume = doc["kodi_volume"].as<int>();
  } else if (doc["kodi_volume"].is<bool>() && !doc["kodi_volume"].as<bool>()) {
    s_kodi_volume = -1;
  } else if (doc["kodi_volume"].isNull()) {
    s_kodi_volume = -1;
  } else {
    s_kodi_volume = -1;
  }
  s_now_playing_count = doc["now_playing_count"] | 0;
  return true;
}

bool fetchNowPlaying() {
  WiFiClient client;
  HTTPClient http;
  String url = String("http://") + apiHost + ":" + apiPort + "/api/nowplaying";
  http.begin(client, url);
  http.addHeader("X-API-Key", PI_API_KEY);
  http.setTimeout(4000);
  int code = http.GET();
  if (code != 200) { http.end(); return false; }
  String body = http.getString();
  http.end();
  JsonDocument doc;
  if (deserializeJson(doc, body)) return false;
  JsonArray arr = doc["streams"].as<JsonArray>();
  npCount = min((int)arr.size(), MAX_STREAMS);
  for (int i = 0; i < npCount; i++) {
    JsonObject s = arr[i];
    npStreams[i].title        = s["title"] | "?";
    npStreams[i].device       = s["device"] | "?";
    npStreams[i].user         = s["user"] | "?";
    npStreams[i].position_sec = s["position_sec"] | 0;
    npStreams[i].duration_sec = s["duration_sec"] | 0;
    npStreams[i].progress_pct = s["progress_pct"] | 0;
    npStreams[i].paused       = s["paused"] | false;
    npStreams[i].type         = s["type"] | "Unknown";
  }
  return true;
}

String apiPostResponse(const char* path) {
  WiFiClient client;
  HTTPClient http;
  String url = String("http://") + apiHost + ":" + apiPort + path;
  http.begin(client, url);
  http.addHeader("X-API-Key", PI_API_KEY);
  http.setTimeout(4000);
  int code = http.POST("");
  String body = "";
  if (code > 0) body = http.getString();
  http.end();
  return body;
}

bool apiPost(const char* path) {
  WiFiClient client;
  HTTPClient http;
  String url = String("http://") + apiHost + ":" + apiPort + path;
  http.begin(client, url);
  http.addHeader("X-API-Key", PI_API_KEY);
  http.setTimeout(4000);
  int code = http.POST("");
  http.end();
  return code == 200;
}

// ---- Page 0: Status (web-style, no flash on update) ----

void drawStatusPage() {
  int W = tft.width();   // 320
  int cardX = 8, cardW = W - 16;
  int y = 0;
  char buf[48];

  if (needsFullRedraw) {
    tft.fillScreen(COL_BG);

    // Header (static, only draw once)
    tft.setTextSize(2);
    tft.setTextColor(COL_HEADER, COL_BG);
    tft.setCursor(30, 8);
    tft.print("Travel Jellyfin");
    tft.setTextSize(1);
    tft.setTextColor(COL_LABEL, COL_BG);
    tft.setCursor(60, 28);
    tft.print("Pi 5 - Mode Controller");
  }
  y = 44;

  // Mode badge — always redraw (color may change)
  tft.fillRect(cardX + 20, y, cardW - 40, 30, COL_BG);  // clear old badge
  const char* modeLabel = s_kodi ? "HOTEL MODE" : "CAR MODE";
  uint16_t modeCol = s_kodi ? COL_MODE_HOT : COL_MODE_CAR;
  tft.fillRoundRect(cardX + 30, y, cardW - 60, 28, 6, modeCol);
  tft.setTextSize(2);
  tft.setTextColor(TFT_WHITE, modeCol);
  uint16_t tw = tft.textWidth(modeLabel);
  tft.setCursor(cardX + 30 + (cardW - 60 - tw) / 2, y + 6);
  tft.print(modeLabel);
  y += 38;

  // System Status card
  int cardH = 112;
  drawCard(cardX, y, cardW, cardH);
  drawCardHeader("SYSTEM STATUS", cardX, y);
  int rowY = y + 24, rowH = 13;

  drawRow("HDMI", s_hdmi ? "connected" : "disconnected", cardX, rowY, cardW, s_hdmi ? COL_OK : COL_WARN);
  rowY += rowH;
  drawRow("Kodi", s_kodi ? "active" : "inactive", cardX, rowY, cardW, s_kodi ? COL_OK : COL_WARN);
  rowY += rowH;
  drawRow("Jellyfin", s_jellyfin ? "active" : "inactive", cardX, rowY, cardW, s_jellyfin ? COL_OK : COL_ERR);
  rowY += rowH;
  drawRow("Hotspot", s_hotspot ? "active" : "inactive", cardX, rowY, cardW, s_hotspot ? COL_OK : COL_ERR);
  rowY += rowH;
  snprintf(buf, sizeof(buf), "%dG / %dG (%d%%)", s_nvme_used, s_nvme_total,
           s_nvme_total > 0 ? (s_nvme_used * 100 / s_nvme_total) : 0);
  drawRowBar("NVMe", buf, s_nvme_total > 0 ? (s_nvme_used * 100 / s_nvme_total) : 0, cardX, rowY, cardW, TFT_BLUE);
  rowY += 18;
  snprintf(buf, sizeof(buf), "%dG / %dG (%d%%)", s_sd_used, s_sd_total,
           s_sd_total > 0 ? (s_sd_used * 100 / s_sd_total) : 0);
  drawRowBar("SD Card", buf, s_sd_total > 0 ? (s_sd_used * 100 / s_sd_total) : 0, cardX, rowY, cardW, TFT_ORANGE);
  y += cardH + 6;

  // Pi Monitor card
  cardH = 140;
  drawCard(cardX, y, cardW, cardH);
  drawCardHeader("PI MONITOR", cardX, y);
  rowY = y + 24;

  snprintf(buf, sizeof(buf), "%.1fC", s_cpu_temp);
  drawRowBar("CPU Temp", buf, (int)(s_cpu_temp), cardX, rowY, cardW, tempColor(s_cpu_temp));
  rowY += rowH + 6;
  snprintf(buf, sizeof(buf), "%.1fC", s_nvme_temp);
  drawRowBar("NVMe Temp", buf, (int)(s_nvme_temp), cardX, rowY, cardW, tempColor(s_nvme_temp));
  rowY += rowH + 6;
  snprintf(buf, sizeof(buf), "%d MHz", s_cpu_clock);
  drawRow("CPU Clock", buf, cardX, rowY, cardW, COL_VALUE); rowY += rowH;
  snprintf(buf, sizeof(buf), "%.3fV", s_cpu_voltage);
  drawRow("CPU Voltage", buf, cardX, rowY, cardW, COL_VALUE); rowY += rowH;
  snprintf(buf, sizeof(buf), "%.2f", s_load1);
  drawRow("Load (1m)", buf, cardX, rowY, cardW, COL_VALUE); rowY += rowH;
  snprintf(buf, sizeof(buf), "%.1f / %.1f GB", s_mem_used, s_mem_total);
  drawRow("Memory", buf, cardX, rowY, cardW, COL_VALUE); rowY += rowH;
  snprintf(buf, sizeof(buf), "%s", s_uptime.c_str());
  drawRow("Uptime", buf, cardX, rowY, cardW, COL_VALUE);
  y += cardH + 6;

  // Network card
  cardH = 44;
  drawCard(cardX, y, cardW, cardH);
  drawCardHeader("NETWORK", cardX, y);
  rowY = y + 24;
  snprintf(buf, sizeof(buf), "%s", s_eth0_ip.length() > 0 ? s_eth0_ip.c_str() : "-");
  drawRow("eth0", buf, cardX, rowY, cardW, COL_VALUE); rowY += rowH;
  snprintf(buf, sizeof(buf), "%s", s_wlan0_ip.length() > 0 ? s_wlan0_ip.c_str() : "-");
  drawRow("wlan0", buf, cardX, rowY, cardW, COL_VALUE);
  y += cardH + 6;

  // Footer
  if (needsFullRedraw) {
    tft.setTextSize(1);
    tft.setTextColor(COL_LABEL, COL_BG);
    tft.setCursor(cardX + 50, y);
    tft.print("Swipe <-> for pages");
    y += 12;
  }
  // Online/offline indicator
  tft.fillRect(cardX + 80, y, 160, 12, COL_BG);
  tft.setTextSize(1);
  tft.setTextColor(apiOnline ? COL_OK : COL_ERR, COL_BG);
  tft.setCursor(cardX + 90, y);
  tft.print(apiOnline ? "Online" : "Offline");
}

// ---- Page 1: Now Playing ----

void drawNowPlayingPage() {
  tft.fillScreen(COL_BG);
  tft.setTextSize(2);
  tft.setTextColor(COL_HEADER, COL_BG);
  tft.setCursor(60, 12);
  tft.print("Now Playing");
  tft.setTextSize(1);
  tft.setTextColor(COL_LABEL, COL_BG);
  tft.setCursor(80, 34);
  tft.print("Swipe <-> for pages");

  if (npCount == 0) {
    tft.setTextSize(2);
    tft.setTextColor(COL_LABEL, COL_BG);
    const char* msg = "No active streams";
    tft.setCursor((tft.width() - tft.textWidth(msg)) / 2, 200);
    tft.print(msg);
    return;
  }

  int y = 50;
  int cardX = 8, cardW = tft.width() - 16;
  // Each stream card: ~95px tall
  for (int i = 0; i < npCount && y < tft.height() - 20; i++) {
    int cardH = 95;
    drawCard(cardX, y, cardW, cardH);
    drawCardHeader(npStreams[i].type.c_str(), cardX, y);

    int rowY = y + 24;
    // Title (white, size 1) — truncate if too long
    tft.setTextSize(1);
    tft.setTextColor(TFT_WHITE, COL_CARD_BG);
    String title = npStreams[i].title;
    if (title.length() > 38) title = title.substring(0, 37) + "~";
    tft.setCursor(cardX + 12, rowY);
    tft.print(title);
    rowY += 14;

    // Device + user (cyan, size 1)
    tft.setTextColor(COL_CYAN, COL_CARD_BG);
    String devUser = npStreams[i].device + " / " + npStreams[i].user;
    if (devUser.length() > 38) devUser = devUser.substring(0, 37) + "~";
    tft.setCursor(cardX + 12, rowY);
    tft.print(devUser);
    rowY += 14;

    // Progress bar
    int barX = cardX + 12, barW = cardW - 24, barH = 6;
    int barY = rowY;
    tft.fillRoundRect(barX, barY, barW, barH, 3, 0x222222);
    if (npStreams[i].progress_pct > 0 && npStreams[i].progress_pct <= 100) {
      tft.fillRoundRect(barX, barY, barW * npStreams[i].progress_pct / 100, barH, 3, COL_HEADER);
    }
    rowY += barH + 4;

    // Time: position / duration
    tft.setTextSize(1);
    tft.setTextColor(COL_LABEL, COL_CARD_BG);
    String timeStr = formatTime(npStreams[i].position_sec) + " / " + formatTime(npStreams[i].duration_sec);
    tft.setCursor(cardX + 12, rowY);
    tft.print(timeStr);

    // Paused indicator
    if (npStreams[i].paused) {
      tft.setTextColor(COL_WARN, COL_CARD_BG);
      const char* pausedTxt = "[PAUSED]";
      tft.setCursor(cardX + cardW - 12 - tft.textWidth(pausedTxt), rowY);
      tft.print(pausedTxt);
    }

    y += cardH + 4;
  }
}

// ---- Page 2: Kodi remote ----

const Btn kodiBtns[] = {
  {120, 100, 80, 50, "UP"    },
  { 35, 155, 80, 50, "LEFT"  },
  {205, 155, 80, 50, "RIGHT" },
  {120, 155, 80, 50, "OK"    },
  {120, 210, 80, 50, "DN"    },
  { 35, 280, 80, 45, "BACK"  },
  {120, 280, 80, 45, "HOME"  },
  {205, 280, 80, 45, "PLAY"  },
  { 35, 340, 80, 45, "STOP"  },
  {120, 340, 80, 45, "VOL-"  },
  {205, 340, 80, 45, "VOL+"  },
  { 35, 400, 80, 45, "SUB"   },
  {120, 400, 80, 45, "MODE"  },
  {205, 400, 80, 45, "MODE2" },
};
const int numKodiBtns = 14;

void drawKodiVolumeBar() {
  // Volume bar below header
  int barX = 35, barW = 250, barY = 55, barH = 12;
  tft.fillRoundRect(barX, barY, barW, barH, 3, 0x222222);
  if (s_kodi_volume >= 0 && s_kodi_volume <= 100) {
    tft.fillRoundRect(barX, barY, barW * s_kodi_volume / 100, barH, 3, COL_HEADER);
  }
  tft.setTextSize(1);
  tft.setTextColor(COL_LABEL, COL_BG);
  char buf[24];
  if (s_kodi_volume >= 0) snprintf(buf, sizeof(buf), "VOL %d", s_kodi_volume);
  else                     snprintf(buf, sizeof(buf), "VOL --");
  tft.setCursor(barX, barY - 12);
  tft.print(buf);
}

void drawKodiPage() {
  tft.fillScreen(COL_BG);
  tft.setTextSize(2);
  tft.setTextColor(COL_HEADER, COL_BG);
  tft.setCursor(60, 12);
  tft.print("Kodi Remote");
  tft.setTextSize(1);
  tft.setTextColor(COL_LABEL, COL_BG);
  tft.setCursor(80, 34);
  tft.print("Swipe <-> for pages");

  if (!s_kodi) {
    tft.setTextSize(2);
    tft.setTextColor(COL_WARN, COL_BG);
    tft.setCursor(50, 160);
    tft.print("Kodi not running");
    tft.setTextSize(1);
    tft.setTextColor(COL_LABEL, COL_BG);
    tft.setCursor(55, 190);
    tft.print("Plug in HDMI or switch");
    tft.setCursor(70, 205);
    tft.print("to Hotel mode");
    Btn modeBtn = {60, 240, 200, 45, "START KODI"};
    drawBtn(modeBtn, COL_MODE_HOT, COL_CARD_BG, TFT_WHITE);
    return;
  }

  // Volume bar
  drawKodiVolumeBar();

  for (int i = 0; i < numKodiBtns; i++) {
    // Skip the placeholder MODE2 button (we only draw one MODE)
    if (strcmp(kodiBtns[i].label, "MODE2") == 0) continue;
    uint16_t border = TFT_NAVY, fill = COL_CARD_BG;
    if (strcmp(kodiBtns[i].label, "OK") == 0) { border = COL_HEADER; fill = 0x0640A0; }
    else if (strcmp(kodiBtns[i].label, "PLAY") == 0) { border = COL_OK; fill = 0x0A4A20; }
    else if (strcmp(kodiBtns[i].label, "STOP") == 0) { border = COL_ERR; fill = 0x4A0A0A; }
    else if (strcmp(kodiBtns[i].label, "MODE") == 0) { border = COL_MODE_HOT; fill = COL_CARD_BG; }
    else if (strcmp(kodiBtns[i].label, "SUB") == 0)  { border = TFT_YELLOW; fill = 0x2A2A00; }
    drawBtn(kodiBtns[i], border, fill, TFT_WHITE);
  }
  tft.setTextSize(1);
  tft.setTextColor(COL_LABEL, COL_BG);
  tft.setCursor(60, 450);
  tft.print("SUB=toggle  MODE=car/hotel");
}

void handleKodiTouch(int tx, int ty) {
  if (!s_kodi) {
    Btn modeBtn = {60, 240, 200, 45, "START KODI"};
    if (hitBtn(modeBtn, tx, ty)) { apiPost("/api/mode/hotel"); pageNeedsRedraw = true; }
    return;
  }
  for (int i = 0; i < numKodiBtns; i++) {
    if (strcmp(kodiBtns[i].label, "MODE2") == 0) continue;  // skip placeholder
    if (!hitBtn(kodiBtns[i], tx, ty)) continue;
    drawBtn(kodiBtns[i], TFT_WHITE, TFT_LIGHTGREY, TFT_BLACK);
    if (strcmp(kodiBtns[i].label, "UP") == 0)         apiPost("/api/kodi/up");
    else if (strcmp(kodiBtns[i].label, "DN") == 0)     apiPost("/api/kodi/down");
    else if (strcmp(kodiBtns[i].label, "LEFT") == 0)  apiPost("/api/kodi/left");
    else if (strcmp(kodiBtns[i].label, "RIGHT") == 0) apiPost("/api/kodi/right");
    else if (strcmp(kodiBtns[i].label, "OK") == 0)    apiPost("/api/kodi/select");
    else if (strcmp(kodiBtns[i].label, "BACK") == 0)  apiPost("/api/kodi/back");
    else if (strcmp(kodiBtns[i].label, "HOME") == 0)  apiPost("/api/kodi/home");
    else if (strcmp(kodiBtns[i].label, "PLAY") == 0)  apiPost("/api/kodi/playpause");
    else if (strcmp(kodiBtns[i].label, "STOP") == 0)  apiPost("/api/kodi/stop");
    else if (strcmp(kodiBtns[i].label, "VOL-") == 0)  { apiPost("/api/kodi/volumedown"); s_kodi_volume = max(0, s_kodi_volume - 5); }
    else if (strcmp(kodiBtns[i].label, "VOL+") == 0)  { apiPost("/api/kodi/volumeup"); s_kodi_volume = min(100, s_kodi_volume + 5); }
    else if (strcmp(kodiBtns[i].label, "SUB") == 0)   apiPost("/api/kodi/subtitles");
    else if (strcmp(kodiBtns[i].label, "MODE") == 0) {
      apiPost(s_kodi ? "/api/mode/car" : "/api/mode/hotel");
      pageNeedsRedraw = true;
    }
    delay(80);
    pageNeedsRedraw = true;
    break;
  }
}

// ---- Page 3: WiFi QR ----

const uint8_t qrModuleSize = 5;
const uint8_t qrSize = 33;
static const uint8_t qrMatrix[] = {
  1,1,1,1,1,1,1,0,0,0,1,1,0,1,1,1,1,1,1,1,1,0,1,0,0,0,1,1,1,1,1,1,1,
  1,0,0,0,0,0,1,0,0,0,0,0,0,1,1,1,1,1,0,0,0,0,1,0,1,0,1,0,0,0,0,0,1,
  1,0,1,1,1,0,1,0,1,1,1,1,0,1,0,0,0,1,1,1,0,0,0,1,1,0,1,0,1,1,1,0,1,
  1,0,1,1,1,0,1,0,1,0,0,0,1,0,1,0,1,0,0,1,1,1,1,1,1,0,1,0,1,1,1,0,1,
  1,0,1,1,1,0,1,0,1,0,0,0,1,0,0,1,0,0,1,0,0,1,0,0,1,0,1,0,1,1,1,0,1,
  1,0,0,0,0,0,1,0,1,0,1,0,1,0,0,1,1,0,0,0,1,1,1,0,0,0,1,0,0,0,0,0,1,
  1,1,1,1,1,1,1,0,1,0,1,0,1,0,1,0,1,0,1,0,1,0,1,0,1,0,1,1,1,1,1,1,1,
  0,0,0,0,0,0,0,0,1,0,1,1,1,0,0,1,0,0,0,0,0,1,0,1,1,0,0,0,0,0,0,0,0,
  1,0,1,1,1,1,1,0,0,1,1,1,0,1,0,1,1,1,0,0,0,0,1,1,0,0,1,1,1,1,1,0,0,
  0,1,1,1,0,0,0,1,0,1,0,0,1,0,1,0,0,0,1,0,1,1,1,1,1,1,0,0,0,0,0,1,1,
  0,1,1,1,0,1,1,1,1,0,1,0,1,0,0,1,0,0,1,0,1,1,0,0,0,0,0,1,1,1,1,1,0,
  1,0,1,0,0,1,0,1,1,0,1,0,1,0,1,0,1,0,0,0,1,1,1,1,1,0,1,1,1,1,1,1,0,
  1,0,1,0,1,1,1,0,0,1,0,1,1,0,0,0,0,0,1,1,0,1,0,0,0,0,0,0,1,0,1,0,1,
  1,1,1,0,0,0,0,1,1,0,0,1,1,0,1,1,1,1,0,0,1,1,1,1,1,1,1,1,0,1,1,0,0,
  0,1,1,1,1,1,1,0,0,1,1,0,0,0,1,0,0,0,0,1,0,0,0,0,0,1,0,0,0,0,0,1,0,
  0,0,1,1,1,1,0,0,1,1,1,0,0,1,1,0,1,0,1,1,1,1,0,1,1,1,1,0,0,1,1,1,1,
  1,0,0,1,1,0,1,1,1,1,0,1,1,1,0,0,0,1,0,1,0,0,1,1,0,1,0,0,1,1,0,1,0,
  1,1,1,1,1,1,0,1,1,1,1,1,0,0,1,0,1,1,1,1,0,0,0,0,0,1,0,1,0,1,1,0,0,
  1,0,1,0,0,0,1,0,0,0,0,0,0,1,0,1,1,1,0,0,0,0,1,0,0,0,0,1,0,0,0,1,0,
  1,1,1,1,1,0,0,0,1,0,1,1,0,1,1,1,0,1,1,0,1,0,0,1,1,0,0,0,0,1,1,1,0,
  1,1,1,1,1,0,1,0,1,1,0,1,1,1,0,0,1,0,0,1,1,1,1,1,0,0,0,1,1,0,1,0,0,
  1,1,1,1,1,0,0,1,0,0,0,1,0,1,1,1,0,1,1,0,0,1,0,1,1,1,1,0,0,0,0,1,0,
  1,0,1,0,1,0,1,0,0,1,1,0,0,1,1,1,1,0,0,0,1,1,0,0,0,0,0,0,0,1,1,1,0,
  1,0,1,1,0,1,0,0,1,0,1,0,0,1,1,1,0,0,1,0,0,1,1,0,0,0,0,0,0,1,1,0,0,
  1,0,1,0,1,1,1,1,1,0,1,0,1,0,1,1,0,1,0,0,0,0,1,0,1,1,1,1,1,0,1,1,1,
  0,0,0,0,0,0,0,0,1,1,0,1,1,0,0,0,1,0,0,1,1,1,1,1,1,0,0,0,1,0,0,0,1,
  1,1,1,1,1,1,1,0,0,0,0,1,0,1,0,1,0,0,1,0,1,1,0,1,1,0,1,0,1,1,1,0,0,
  1,0,0,0,0,0,1,0,1,0,1,0,0,0,1,0,1,0,0,0,1,1,1,0,1,0,0,0,1,0,1,0,1,
  1,0,1,1,1,0,1,0,1,0,1,0,0,0,1,1,0,0,1,0,0,1,0,0,1,1,1,1,1,1,0,0,1,
  1,0,1,1,1,0,1,0,1,0,0,0,1,0,1,1,1,1,0,0,1,1,1,1,1,1,0,0,0,1,0,0,1,
  1,0,1,1,1,0,1,0,1,0,1,0,0,1,0,0,0,0,1,1,0,0,1,0,1,0,0,0,0,0,1,0,0,
  1,0,0,0,0,0,1,0,0,0,1,0,0,1,1,0,1,0,0,1,1,1,0,1,0,0,0,1,1,1,1,0,0,
  1,1,1,1,1,1,1,0,1,0,0,1,1,0,1,0,0,1,0,1,0,0,1,0,1,1,0,1,1,0,0,1,0,
};

void drawWiFiPage() {
  tft.fillScreen(COL_BG);
  tft.setTextSize(2);
  tft.setTextColor(COL_HEADER, COL_BG);
  tft.setCursor(75, 12);
  tft.print("WiFi Hotspot");
  tft.setTextSize(1);
  tft.setTextColor(COL_LABEL, COL_BG);
  tft.setCursor(80, 34);
  tft.print("Swipe <-> for pages");

  int qrPix = qrSize * qrModuleSize;
  int qrX = (tft.width() - qrPix) / 2;
  int qrY = 60;
  tft.fillRect(qrX - 10, qrY - 10, qrPix + 20, qrPix + 20, TFT_WHITE);
  for (int r = 0; r < qrSize; r++)
    for (int c = 0; c < qrSize; c++)
      tft.fillRect(qrX + c * qrModuleSize, qrY + r * qrModuleSize,
                   qrModuleSize, qrModuleSize,
                   qrMatrix[r * qrSize + c] ? TFT_BLACK : TFT_WHITE);

  int textY = qrY + qrPix + 20;
  tft.setTextSize(2);
  tft.setTextColor(TFT_WHITE, COL_BG);
  tft.setCursor((tft.width() - tft.textWidth("Travel-Jellyfin")) / 2, textY);
  tft.print("Travel-Jellyfin");
  tft.setTextSize(1);
  tft.setTextColor(COL_HEADER, COL_BG);
  // Only SSID + "Scan to join" — NO password text
  tft.setCursor((tft.width() - tft.textWidth("Scan to join")) / 2, textY + 24);
  tft.print("Scan to join");
}

// ---- Page 4: System Control ----

const Btn sysBtns[] = {
  { 40,  60, 240, 50, "REBOOT PI"       },
  { 40, 120, 240, 50, "SHUTDOWN PI"     },
  { 40, 180, 240, 50, "RESTART JELLYFIN"},
  { 40, 240, 240, 50, "RESTART KODI"    },
  { 40, 300, 240, 45, "BACK TO STATUS"  },
};
const int numSysBtns = 5;

void drawSystemPage() {
  tft.fillScreen(COL_BG);
  tft.setTextSize(2);
  tft.setTextColor(COL_HEADER, COL_BG);
  tft.setCursor(55, 12);
  tft.print("System Control");
  tft.setTextSize(1);
  tft.setTextColor(COL_LABEL, COL_BG);
  tft.setCursor(80, 34);
  tft.print("Swipe <-> for pages");

  for (int i = 0; i < numSysBtns; i++) {
    uint16_t border, fill;
    if (strcmp(sysBtns[i].label, "REBOOT PI") == 0)        { border = COL_ERR;   fill = 0x2A0808; }
    else if (strcmp(sysBtns[i].label, "SHUTDOWN PI") == 0) { border = COL_ERR;   fill = 0x180404; }
    else if (strcmp(sysBtns[i].label, "RESTART JELLYFIN") == 0) { border = COL_HEADER; fill = 0x041A2A; }
    else if (strcmp(sysBtns[i].label, "RESTART KODI") == 0)     { border = COL_PURPLE; fill = 0x1A082A; }
    else                                                   { border = COL_LABEL; fill = COL_CARD_BG; }
    drawBtn(sysBtns[i], border, fill, TFT_WHITE);
  }

  // Confirmation text if active
  tft.fillRect(20, 360, 280, 40, COL_BG);
  if (sysConfirmAction.length() > 0 && (millis() - sysConfirmTime) < SYS_CONFIRM_TIMEOUT) {
    tft.setTextSize(1);
    tft.setTextColor(COL_WARN, COL_BG);
    const char* msg = "Press again to confirm";
    tft.setCursor((tft.width() - tft.textWidth(msg)) / 2, 370);
    tft.print(msg);
  } else if (sysConfirmAction.length() > 0) {
    sysConfirmAction = "";  // expired
  }
}

void handleSystemTouch(int tx, int ty) {
  for (int i = 0; i < numSysBtns; i++) {
    if (!hitBtn(sysBtns[i], tx, ty)) continue;

    if (strcmp(sysBtns[i].label, "BACK TO STATUS") == 0) {
      currentPage = 0;
      needsFullRedraw = true;
      pageNeedsRedraw = true;
      sysConfirmAction = "";
      return;
    }

    // Dangerous actions: two-press confirmation
    if (strcmp(sysBtns[i].label, "REBOOT PI") == 0 || strcmp(sysBtns[i].label, "SHUTDOWN PI") == 0) {
      String action = (strcmp(sysBtns[i].label, "REBOOT PI") == 0) ? "reboot" : "shutdown";
      if (sysConfirmAction == action && (millis() - sysConfirmTime) < SYS_CONFIRM_TIMEOUT) {
        // Second press — execute. Need token from first POST.
        // First POST returned confirm_required with a token; we stored it.
        // Simpler: POST without confirm to get token, then POST with confirm+token.
        String resp1 = apiPostResponse(("/api/system/" + action).c_str());
        JsonDocument doc1;
        if (!deserializeJson(doc1, resp1) && doc1["confirm_token"].is<const char*>()) {
          String token = doc1["confirm_token"].as<String>();
          String url2 = "/api/system/" + action + "?confirm=1&token=" + token;
          apiPostResponse(url2.c_str());
        }
        sysConfirmAction = "";
        pageNeedsRedraw = true;
      } else {
        // First press — show confirmation
        sysConfirmAction = action;
        sysConfirmTime = millis();
        pageNeedsRedraw = true;
      }
      return;
    }

    // Non-dangerous: execute immediately
    if (strcmp(sysBtns[i].label, "RESTART JELLYFIN") == 0) {
      apiPost("/api/system/restart-jellyfin");
    } else if (strcmp(sysBtns[i].label, "RESTART KODI") == 0) {
      apiPost("/api/system/restart-kodi");
    }
    sysConfirmAction = "";
    pageNeedsRedraw = true;
    return;
  }
}

// ---- Error overlay (shown on any page when PI unreachable 2+ polls) ----

void drawErrorOverlay() {
  // Semi-transparent dark band + red text
  int bandY = tft.height() / 2 - 50;
  tft.fillRoundRect(20, bandY, tft.width() - 40, 100, 8, 0x1A0000);
  tft.drawRoundRect(20, bandY, tft.width() - 40, 100, 8, COL_ERR);

  tft.setTextSize(2);
  tft.setTextColor(COL_ERR, 0x1A0000);
  const char* title = "PI UNREACHABLE";
  tft.setCursor((tft.width() - tft.textWidth(title)) / 2, bandY + 12);
  tft.print(title);

  tft.setTextSize(1);
  tft.setTextColor(TFT_WHITE, 0x1A0000);
  String lastLine = "Last seen: " + lastSeenTime;
  tft.setCursor((tft.width() - tft.textWidth(lastLine.c_str())) / 2, bandY + 44);
  tft.print(lastLine);

  tft.setTextColor(COL_LABEL, 0x1A0000);
  const char* hint = "Swipe to browse cached pages";
  tft.setCursor((tft.width() - tft.textWidth(hint)) / 2, bandY + 66);
  tft.print(hint);
}

// ---- Page management ----

void drawCurrentPage() {
  switch (currentPage) {
    case 0: drawStatusPage();    break;
    case 1: drawNowPlayingPage(); break;
    case 2: drawKodiPage();      break;
    case 3: drawWiFiPage();      break;
    case 4: drawSystemPage();    break;
  }
  pageNeedsRedraw = false;
  needsFullRedraw = false;
}

void cyclePage(int dir) {
  currentPage = (currentPage + dir + numPages) % numPages;
  pageNeedsRedraw = true;
  needsFullRedraw = true;
  sysConfirmAction = "";  // clear confirmation on page change
  Serial.printf("Page -> %d\n", currentPage);
}

bool getTouchCoords(int &tx, int &ty) {
  uint16_t rx, ry;
  if (!tft.getTouch(&rx, &ry)) return false;
#if INVERT_TOUCH_X
  tx = tft.width() - 1 - rx;
#else
  tx = rx;
#endif
#if INVERT_TOUCH_Y
  ty = tft.height() - 1 - ry;
#else
  ty = ry;
#endif
  return true;
}

// ---- Backlight dim / wake ----

void updateBacklight() {
  unsigned long now = millis();
  if (!backlightDimmed && (now - lastTouchTime) > BACKLIGHT_DIM_MS) {
    analogWrite(BL_PIN, BACKLIGHT_DIM_LEVEL);
    backlightDimmed = true;
    Serial.println("Backlight dimmed");
  }
}

void wakeBacklight() {
  if (backlightDimmed) {
    analogWrite(BL_PIN, 255);
    backlightDimmed = false;
    Serial.println("Backlight woke");
  }
}

// ---- Touch with swipe + tap ----

void handleTouch() {
  int tx, ty;
  bool touched = getTouchCoords(tx, ty);

  if (touched) {
    // On any touch, record time and wake backlight
    lastTouchX = tx;
    lastTouchY = ty;
    lastTouchTime = millis();

    if (backlightDimmed) {
      // First touch when dimmed only wakes — don't register as tap/swipe
      wakeBacklight();
      touchWasActive = false;  // discard any in-progress gesture
      return;
    }

    if (!touchWasActive) {
      touchStartX = tx;
      touchStartY = ty;
      touchStartTime = millis();
      touchWasActive = true;
      Serial.printf("Touch start: (%d, %d)\n", tx, ty);
    }
  } else {
    if (touchWasActive) {
      int dx = lastTouchX - touchStartX;
      int dy = lastTouchY - touchStartY;
      unsigned long elapsed = millis() - touchStartTime;
      touchWasActive = false;

      Serial.printf("Touch end: dx=%d dy=%d elapsed=%lu\n", dx, dy, elapsed);

      if (abs(dx) > SWIPE_THRESHOLD && abs(dx) > abs(dy)) {
        // Swipe detected
        Serial.printf("SWIPE %s\n", dx > 0 ? "RIGHT" : "LEFT");
        cyclePage(dx > 0 ? 1 : -1);
      } else if (abs(dx) < TAP_THRESHOLD && abs(dy) < TAP_THRESHOLD && elapsed < TAP_TIMEOUT) {
        // Tap detected
        Serial.printf("TAP at (%d, %d)\n", touchStartX, touchStartY);
        if (currentPage == 2)      handleKodiTouch(touchStartX, touchStartY);
        else if (currentPage == 4) handleSystemTouch(touchStartX, touchStartY);
      }
    }
  }
}

// ---- Setup & Loop ----

void setup() {
  Serial.begin(115200);
  delay(200);
  pinMode(BL_PIN, OUTPUT);
  pinMode(LED_R, OUTPUT);
  pinMode(LED_G, OUTPUT);
  pinMode(LED_B, OUTPUT);
  digitalWrite(BL_PIN, HIGH);
  lastTouchTime = millis();
  setLED(0, 0, 1);

  tft.init();
  tft.setRotation(TFT_ROTATION);
  tft.fillScreen(COL_BG);
  tft.setTouch(calData);

  tft.setTextSize(2);
  tft.setTextColor(COL_HEADER, COL_BG);
  tft.setCursor((tft.width() - tft.textWidth("Travel Jellyfin")) / 2, 200);
  tft.print("Travel Jellyfin");
  tft.setTextSize(1);
  tft.setTextColor(COL_LABEL, COL_BG);
  tft.setCursor((tft.width() - tft.textWidth("Connecting to WiFi...")) / 2, 230);
  tft.print("Connecting to WiFi...");

  connectWiFi();
  fetchStatus();
  if (s_kodi) currentPage = 2;  // Kodi remote is now page 2
  pageNeedsRedraw = true;
  needsFullRedraw = true;
}

void loop() {
  unsigned long now = millis();

  handleTouch();
  updateBacklight();

  // Poll status every POLL_INTERVAL_MS
  if (now - lastPoll > POLL_INTERVAL_MS || lastPoll == 0) {
    lastPoll = now;
    if (WiFi.status() != WL_CONNECTED) { setLED(1, 0, 0); connectWiFi(); }
    bool ok = fetchStatus();
    setLED(ok ? 0 : 1, ok ? 1 : 0, 0);

    if (ok) {
      apiOnline = true;
      apiFailCount = 0;
      lastSeenTime = clockString();
      bool wasOverlay = errorOverlay;
      errorOverlay = false;
      if (wasOverlay) { needsFullRedraw = true; pageNeedsRedraw = true; }
    } else {
      apiOnline = false;
      apiFailCount++;
      if (apiFailCount >= 2) {
        bool wasOverlay = errorOverlay;
        errorOverlay = true;
        if (!wasOverlay) pageNeedsRedraw = true;  // trigger overlay draw
      }
    }

    // Auto page switching logic
    if (ok) {
      // Switch to Now Playing when streams > 0 (if on status page)
      if (s_now_playing_count > 0 && currentPage == 0) {
        currentPage = 1;
        needsFullRedraw = true;
        pageNeedsRedraw = true;
      }
      // Kodi start/stop auto-switch
      if (s_kodi && !kodiWasRunning && currentPage == 0) { currentPage = 2; needsFullRedraw = true; pageNeedsRedraw = true; }
      if (!s_kodi && kodiWasRunning && currentPage == 2) { currentPage = 0; needsFullRedraw = true; pageNeedsRedraw = true; }
      kodiWasRunning = s_kodi;
    }

    // Only redraw status page on data update (no full screen clear)
    if (currentPage == 0) pageNeedsRedraw = true;
  }

  // Poll Now Playing every 3s (separate from status poll)
  if (now - lastNowPlayingPoll > POLL_INTERVAL_MS || lastNowPlayingPoll == 0) {
    lastNowPlayingPoll = now;
    if (apiOnline && s_jellyfin) {
      fetchNowPlaying();
    } else {
      npCount = 0;
    }
    if (currentPage == 1) pageNeedsRedraw = true;
  }

  if (pageNeedsRedraw) {
    drawCurrentPage();
    // Draw error overlay on top after page draw
    if (errorOverlay) drawErrorOverlay();
  }
  delay(20);
}