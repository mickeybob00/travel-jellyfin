/* ============================================================
 *  Travel Jellyfin CYD — ESP32-3248S035R (3.5" resistive)
 *  Front-panel touchscreen for the Travel Pi media server.
 *
 *  3 pages:
 *    1. Status dashboard (mode, temps, storage, uptime)
 *    2. Kodi remote (D-pad, play/pause, back, home, volume)
 *    3. WiFi QR code (join the hotspot by scanning)
 *
 *  Polls /api/status every 3 s. Auto-switches to Kodi remote
 *  when kodi_running == true. Backlight sleeps after 60 s idle.
 * ============================================================ */

#include <Arduino.h>
#include <WiFi.h>
#include <HTTPClient.h>
#include <ArduinoJson.h>
#include <TFT_eSPI.h>
#include <SPI.h>

#include "config.h"

// ---- Globals ----
TFT_eSPI tft = TFT_eSPI();

// Touch calibration data (landscape, rotation=1)
uint16_t calData[] = {
  TOUCH_CAL_LEFT, TOUCH_CAL_RIGHT,
  TOUCH_CAL_TOP,  TOUCH_CAL_BOT,
  1               // 1 = landscape orientation
};

// API state
String apiHost   = PI_API_HOST_HOTSPOT;
int    apiPort   = PI_API_PORT;
unsigned long lastPoll   = 0;
bool    apiOnline       = false;
bool    kodiWasRunning  = false;

// Status JSON fields (kept as flat vars to avoid heavy allocation)
String  s_mode           = "car";
bool    s_hdmi           = false;
bool    s_kodi           = false;
bool    s_jellyfin       = false;
bool    s_hotspot        = false;
float   s_cpu_temp       = 0;
float   s_nvme_temp      = 0;
int     s_cpu_clock      = 0;
float   s_cpu_voltage    = 0;
float   s_mem_used       = 0;
float   s_mem_total      = 0;
float   s_load1          = 0;
String  s_uptime         = "";
String  s_eth0_ip        = "";
String  s_wlan0_ip       = "";
int     s_nvme_used      = 0;
int     s_nvme_total     = 0;
int     s_sd_used        = 0;
int     s_sd_total       = 0;

// UI state
int     currentPage      = 0;       // 0=status, 1=kodi, 2=wifi
int     numPages         = 3;
unsigned long lastTouch  = 0;
bool    backlightOn      = true;
bool    pageNeedsRedraw  = true;

// Button hit-test helper
struct Btn {
  int x, y, w, h;
  const char* label;
};

// ---- Small utility helpers ----

void setBacklight(bool on) {
  digitalWrite(BL_PIN, on ? HIGH : LOW);
  backlightOn = on;
}

void setLED(uint8_t r, uint8_t g, uint8_t b) {
  // Active-low: HIGH = off, LOW = on
  digitalWrite(LED_R, r ? LOW : HIGH);
  digitalWrite(LED_G, g ? LOW : HIGH);
  digitalWrite(LED_B, b ? LOW : HIGH);
}

void drawCenteredText(const char* s, int y, int size, uint16_t color) {
  tft.setTextSize(size);
  tft.setTextColor(color, TFT_BLACK);
  int16_t x1, y1;
  uint16_t tw, th;
  tft.getTextBounds(s, 0, 0, &x1, &y1, &tw, &th);
  tft.setCursor((tft.width() - tw) / 2, y);
  tft.print(s);
}

void drawRightText(const char* s, int y, int size, uint16_t color) {
  tft.setTextSize(size);
  tft.setTextColor(color, TFT_BLACK);
  int16_t x1, y1;
  uint16_t tw, th;
  tft.getTextBounds(s, 0, 0, &x1, &y1, &tw, &th);
  tft.setCursor(tft.width() - tw - 5, y);
  tft.print(s);
}

// Draw a touch button with rounded rect
void drawBtn(const Btn& b, uint16_t borderCol, uint16_t fillCol, uint16_t textCol) {
  tft.fillRoundRect(b.x, b.y, b.w, b.h, 6, fillCol);
  tft.drawRoundRect(b.x, b.y, b.w, b.h, 6, borderCol);
  // Center the label
  tft.setTextSize(2);
  tft.setTextColor(textCol, fillCol);
  int16_t x1, y1;
  uint16_t tw, th;
  tft.getTextBounds(b.label, 0, 0, &x1, &y1, &tw, &th);
  tft.setCursor(b.x + (b.w - tw) / 2, b.y + (b.h - th) / 2 - 2);
  tft.print(b.label);
}

bool hitBtn(const Btn& b, int tx, int ty) {
  return tx >= b.x && tx < b.x + b.w && ty >= b.y && ty < b.y + b.h;
}

// ---- WiFi ----

void connectWiFi() {
  // Try hotspot first, then home if configured
  const char* ssids[]  = {WIFI_SSID_HOTSPOT, WIFI_SSID_HOME};
  const char* passes[] = {WIFI_PASS_HOTSPOT, WIFI_PASS_HOME};
  int num = sizeof(ssids) / sizeof(ssids[0]);

  for (int i = 0; i < num; i++) {
    if (strlen(ssids[i]) == 0) continue;

    Serial.printf("Connecting to \"%s\" …\n", ssids[i]);
    WiFi.mode(WIFI_STA);
    WiFi.begin(ssids[i], passes[i]);

    int attempts = 0;
    while (WiFi.status() != WL_CONNECTED && attempts < 20) {
      delay(500);
      attempts++;
      setLED(0, 0, 1); // blue = connecting
    }

    if (WiFi.status() == WL_CONNECTED) {
      Serial.printf("Connected! IP: %s\n", WiFi.localIP().toString().c_str());
      // Pick API host based on which network we joined
      if (strcmp(ssids[i], WIFI_SSID_HOTSPOT) == 0) {
        apiHost = PI_API_HOST_HOTSPOT;
      } else {
        apiHost = PI_API_HOST_HOME;
      }
      setLED(0, 1, 0); // green = connected
      return;
    }
  }

  Serial.println("WiFi connect failed, will retry…");
  setLED(1, 0, 0); // red = failed
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
  if (code != 200) {
    http.end();
    apiOnline = false;
    return false;
  }

  String body = http.getString();
  http.end();

  JsonDocument doc;
  DeserializationError err = deserializeJson(doc, body);
  if (err) {
    apiOnline = false;
    return false;
  }

  apiOnline = true;
  s_mode        = doc["mode"]            | "car";
  s_hdmi        = doc["hdmi_connected"]  | false;
  s_kodi        = doc["kodi_running"]    | false;
  s_jellyfin    = doc["jellyfin_running"]| false;
  s_hotspot     = doc["hotspot_running"] | false;
  s_cpu_temp    = doc["cpu_temp"]        | 0.0f;
  s_nvme_temp   = doc["nvme_temp"]       | 0.0f;
  s_cpu_clock   = doc["cpu_clock"]       | 0;
  s_cpu_voltage = doc["cpu_voltage"]     | 0.0f;
  s_mem_used    = doc["memory_used_gb"]  | 0.0f;
  s_mem_total   = doc["memory_total_gb"] | 0.0f;
  s_load1       = doc["load_1"]          | 0.0f;
  s_uptime      = doc["uptime"]          | "?";
  s_eth0_ip     = doc["eth0_ip"]         | "";
  s_wlan0_ip    = doc["wlan0_ip"]        | "";
  s_nvme_used   = doc["nvme_used_gb"]    | 0;
  s_nvme_total  = doc["nvme_total_gb"]   | 0;
  s_sd_used     = doc["sd_used_gb"]      | 0;
  s_sd_total    = doc["sd_total_gb"]     | 0;

  return true;
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

// ---- Page drawing ----

void drawHeader(const char* title) {
  tft.fillScreen(TFT_BLACK);
  tft.fillRect(0, 0, tft.width(), 28, TFT_NAVY);
  tft.setTextSize(2);
  tft.setTextColor(TFT_WHITE, TFT_NAVY);
  tft.setCursor(8, 6);
  tft.print(title);

  // Page indicator dots (top-right)
  int dotY = 11;
  int dotX = tft.width() - 55;
  for (int i = 0; i < numPages; i++) {
    if (i == currentPage)
      tft.fillCircle(dotX + i * 18, dotY, 4, TFT_CYAN);
    else
      tft.drawCircle(dotX + i * 18, dotY, 4, TFT_DARKGREY);
  }

  // Connection status (right of title)
  const char* connLabel = apiOnline ? "●" : "✕";
  uint16_t connCol = apiOnline ? TFT_GREEN : TFT_RED;
  tft.setTextSize(2);
  tft.setTextColor(connCol, TFT_NAVY);
  tft.setCursor(tft.width() - 75, 6);
  tft.print(connLabel);
}

// --- Page 0: Status dashboard ---

void drawStatusPage() {
  drawHeader("Pi Status");

  int y = 38;
  int lineH = 24;

  // Mode badge
  const char* modeLabel = s_kodi ? "HOTEL (Kodi)" : "CAR (Jellyfin)";
  uint16_t modeCol = s_kodi ? TFT_PURPLE : TFT_GREEN;
  tft.fillRoundRect(8, y, 200, 26, 5, modeCol);
  tft.setTextSize(2);
  tft.setTextColor(TFT_WHITE, modeCol);
  tft.setCursor(18, y + 5);
  tft.print(modeLabel);
  y += 36;

  // Services row
  tft.setTextSize(1);
  tft.setTextColor(TFT_CYAN, TFT_BLACK);
  tft.setCursor(8, y); tft.print("Jellyfin:");
  tft.setTextColor(s_jellyfin ? TFT_GREEN : TFT_RED, TFT_BLACK);
  tft.setCursor(75, y); tft.print(s_jellyfin ? "UP" : "DOWN");

  tft.setTextColor(TFT_CYAN, TFT_BLACK);
  tft.setCursor(160, y); tft.print("Hotspot:");
  tft.setTextColor(s_hotspot ? TFT_GREEN : TFT_RED, TFT_BLACK);
  tft.setCursor(230, y); tft.print(s_hotspot ? "UP" : "DOWN");
  y += lineH;

  tft.setTextColor(TFT_CYAN, TFT_BLACK);
  tft.setCursor(8, y); tft.print("HDMI:");
  tft.setTextColor(s_hdmi ? TFT_GREEN : TFT_DARKGREY, TFT_BLACK);
  tft.setCursor(75, y); tft.print(s_hdmi ? "Connected" : "—");
  y += lineH;

  // Temps
  tft.setTextColor(TFT_CYAN, TFT_BLACK);
  tft.setCursor(8, y); tft.print("CPU:");
  uint16_t tCol = s_cpu_temp > 65 ? TFT_RED : (s_cpu_temp > 50 ? TFT_YELLOW : TFT_GREEN);
  tft.setTextColor(tCol, TFT_BLACK);
  tft.setCursor(75, y); tft.printf("%.1fC", s_cpu_temp);

  tft.setTextColor(TFT_CYAN, TFT_BLACK);
  tft.setCursor(170, y); tft.print("NVMe:");
  tft.setTextColor(TFT_WHITE, TFT_BLACK);
  tft.setCursor(230, y); tft.printf("%.1fC", s_nvme_temp);
  y += lineH;

  // CPU clock / voltage
  tft.setTextColor(TFT_CYAN, TFT_BLACK);
  tft.setCursor(8, y); tft.print("Clock:");
  tft.setTextColor(TFT_WHITE, TFT_BLACK);
  tft.setCursor(75, y); tft.printf("%d MHz", s_cpu_clock);

  tft.setTextColor(TFT_CYAN, TFT_BLACK);
  tft.setCursor(170, y); tft.print("Volt:");
  tft.setTextColor(TFT_WHITE, TFT_BLACK);
  tft.setCursor(230, y); tft.printf("%.3fV", s_cpu_voltage);
  y += lineH;

  // Memory + load
  tft.setTextColor(TFT_CYAN, TFT_BLACK);
  tft.setCursor(8, y); tft.print("RAM:");
  tft.setTextColor(TFT_WHITE, TFT_BLACK);
  tft.setCursor(75, y); tft.printf("%.1f / %.1f GB", s_mem_used, s_mem_total);

  tft.setTextColor(TFT_CYAN, TFT_BLACK);
  tft.setCursor(170, y); tft.print("Load:");
  tft.setTextColor(TFT_WHITE, TFT_BLACK);
  tft.setCursor(230, y); tft.printf("%.2f", s_load1);
  y += lineH;

  // Uptime
  tft.setTextColor(TFT_CYAN, TFT_BLACK);
  tft.setCursor(8, y); tft.print("Uptime:");
  tft.setTextColor(TFT_WHITE, TFT_BLACK);
  tft.setCursor(75, y); tft.print(s_uptime);
  y += lineH;

  // Storage bars
  tft.setTextColor(TFT_CYAN, TFT_BLACK);
  tft.setCursor(8, y); tft.print("NVMe:");
  tft.setTextColor(TFT_WHITE, TFT_BLACK);
  tft.setCursor(75, y); tft.printf("%d / %d GB", s_nvme_used, s_nvme_total);
  y += 14;
  // Bar
  int barX = 8, barW = tft.width() - 16, barH = 8;
  tft.drawRoundRect(barX, y, barW, barH, 2, TFT_DARKGREY);
  if (s_nvme_total > 0) {
    int fillW = (barW - 2) * s_nvme_used / s_nvme_total;
    tft.fillRoundRect(barX + 1, y + 1, fillW, barH - 2, 2, TFT_BLUE);
  }
  y += 18;

  tft.setTextColor(TFT_CYAN, TFT_BLACK);
  tft.setCursor(8, y); tft.print("SD:");
  tft.setTextColor(TFT_WHITE, TFT_BLACK);
  tft.setCursor(75, y); tft.printf("%d / %d GB", s_sd_used, s_sd_total);
  y += 14;
  tft.drawRoundRect(barX, y, barW, barH, 2, TFT_DARKGREY);
  if (s_sd_total > 0) {
    int fillW = (barW - 2) * s_sd_used / s_sd_total;
    tft.fillRoundRect(barX + 1, y + 1, fillW, barH - 2, 2, TFT_ORANGE);
  }
  y += 20;

  // IPs
  tft.setTextColor(TFT_CYAN, TFT_BLACK);
  tft.setCursor(8, y); tft.print("eth0:");
  tft.setTextColor(TFT_WHITE, TFT_BLACK);
  tft.setCursor(50, y); tft.print(s_eth0_ip);
  y += 14;
  tft.setTextColor(TFT_CYAN, TFT_BLACK);
  tft.setCursor(8, y); tft.print("wlan:");
  tft.setTextColor(TFT_WHITE, TFT_BLACK);
  tft.setCursor(50, y); tft.print(s_wlan0_ip);

  // Bottom hint
  tft.setTextSize(1);
  tft.setTextColor(TFT_DARKGREY, TFT_BLACK);
  tft.setCursor(8, tft.height() - 12);
  tft.print("Tap anywhere to cycle pages");
}

// --- Page 1: Kodi remote ---

// D-pad + controls layout (landscape 480x320)
// We define buttons once so hit-test works
const Btn kodiBtns[] = {
  // D-pad (left side)
  { 30,  70, 80, 50, "UP"    },
  { 30, 125, 80, 50, "DN"    },
  { 30, 180, 80, 50, "BACK"  },
  // Center column
  {130,  70, 80, 50, "LEFT"  },
  {130, 125, 80, 50, "OK"    },
  {130, 180, 80, 50, "HOME"  },
  // Right column
  {230,  70, 80, 50, "RIGHT" },
  {230, 125, 80, 50, "PLAY"  },
  {230, 180, 80, 50, "STOP"  },
  // Volume row (bottom)
  { 30, 245, 100, 40, "VOL-" },
  {140, 245, 100, 40, "VOL+" },
  {250, 245, 100, 40, "MODE" },
};
const int numKodiBtns = 12;

void drawKodiPage() {
  drawHeader("Kodi Remote");

  if (!s_kodi) {
    drawCenteredText("Kodi not running", 140, 2, TFT_YELLOW);
    drawCenteredText("Plug in HDMI or", 170, 1, TFT_DARKGREY);
    drawCenteredText("switch to Hotel mode", 185, 1, TFT_DARKGREY);

    // Draw a MODE button to start Kodi
    Btn modeBtn = {140, 220, 200, 40, "START KODI"};
    drawBtn(modeBtn, TFT_PURPLE, TFT_DARKGREY, TFT_WHITE);
    return;
  }

  // Draw all remote buttons
  for (int i = 0; i < numKodiBtns; i++) {
    uint16_t border = TFT_NAVY;
    uint16_t fill   = TFT_DARKGREY;
    uint16_t text   = TFT_WHITE;

    // Highlight key buttons
    if (strcmp(kodiBtns[i].label, "OK") == 0) {
      border = TFT_CYAN; fill = TFT_BLUE;
    } else if (strcmp(kodiBtns[i].label, "PLAY") == 0) {
      border = TFT_GREEN; fill = TFT_DARKGREEN;
    } else if (strcmp(kodiBtns[i].label, "STOP") == 0) {
      border = TFT_RED; fill = TFT_MAROON;
    } else if (strcmp(kodiBtns[i].label, "MODE") == 0) {
      border = TFT_PURPLE; fill = TFT_DARKGREY;
    }

    drawBtn(kodiBtns[i], border, fill, text);
  }

  // Bottom hint
  tft.setTextSize(1);
  tft.setTextColor(TFT_DARKGREY, TFT_BLACK);
  tft.setCursor(8, tft.height() - 12);
  tft.print("MODE = toggle car/hotel");
}

void handleKodiTouch(int tx, int ty) {
  if (!s_kodi) {
    // Only the START KODI button is active
    Btn modeBtn = {140, 220, 200, 40, "START KODI"};
    if (hitBtn(modeBtn, tx, ty)) {
      apiPost("/api/mode/hotel");
      pageNeedsRedraw = true;
    }
    return;
  }

  for (int i = 0; i < numKodiBtns; i++) {
    if (!hitBtn(kodiBtns[i], tx, ty)) continue;

    // Brief visual feedback
    drawBtn(kodiBtns[i], TFT_WHITE, TFT_LIGHTGREY, TFT_BLACK);

    if (strcmp(kodiBtns[i].label, "UP") == 0)    apiPost("/api/kodi/up");
    else if (strcmp(kodiBtns[i].label, "DN") == 0)    apiPost("/api/kodi/down");
    else if (strcmp(kodiBtns[i].label, "LEFT") == 0)  apiPost("/api/kodi/left");
    else if (strcmp(kodiBtns[i].label, "RIGHT") == 0) apiPost("/api/kodi/right");
    else if (strcmp(kodiBtns[i].label, "OK") == 0)   apiPost("/api/kodi/select");
    else if (strcmp(kodiBtns[i].label, "BACK") == 0) apiPost("/api/kodi/back");
    else if (strcmp(kodiBtns[i].label, "HOME") == 0) apiPost("/api/kodi/home");
    else if (strcmp(kodiBtns[i].label, "PLAY") == 0) apiPost("/api/kodi/playpause");
    else if (strcmp(kodiBtns[i].label, "STOP") == 0) apiPost("/api/kodi/stop");
    else if (strcmp(kodiBtns[i].label, "VOL-") == 0) apiPost("/api/kodi/volumedown");
    else if (strcmp(kodiBtns[i].label, "VOL+") == 0) apiPost("/api/kodi/volumeup");
    else if (strcmp(kodiBtns[i].label, "MODE") == 0) {
      // Toggle mode
      if (s_kodi) apiPost("/api/mode/car");
      else        apiPost("/api/mode/hotel");
      pageNeedsRedraw = true;
    }

    delay(80); // brief flash
    pageNeedsRedraw = true;
    break;
  }
}

// --- Page 2: WiFi QR code ---
// We draw a simple QR code for the hotspot credentials.
// QR rendering is done manually with rect primitives — no
// external QR library to keep the build lean.

// QR data: WIFI:T:WPA;S:Travel-Jellyfin;P:!IvyLee2015;;
// Pre-computed QR matrix (version 4, 33x33 modules, ECC level M)
// Generated with Python qrcode library. To regenerate after changing
// the WiFi string, run:
//   pip install qrcode && python3 -c "
//   import qrcode; q=qrcode.QRCode(4,'M',8,4)
//   q.add_data('WIFI:T:WPA;S:Travel-Jellyfin;P:!IvyLee2015;;')
//   q.make()
//   for r in q.modules: print('  '+','.join(str(int(m)) for m in r)+',')
//   "
const uint8_t qrModuleSize = 6;  // pixels per module (33*6=198px, fits 480 wide)
const uint8_t qrSize = 33;       // 33x33 modules

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
  drawHeader("WiFi Hotspot");

  // QR code centered
  int qrPixelSize = qrSize * qrModuleSize;  // 29 * 8 = 232px
  int qrX = (tft.width() - qrPixelSize) / 2;
  int qrY = 40;

  // White background for QR
  tft.fillRect(qrX - 8, qrY - 8, qrPixelSize + 16, qrPixelSize + 16, TFT_WHITE);

  // Draw modules
  for (int r = 0; r < qrSize; r++) {
    for (int c = 0; c < qrSize; c++) {
      uint8_t mod = qrMatrix[r * qrSize + c];
      uint16_t col = mod ? TFT_BLACK : TFT_WHITE;
      tft.fillRect(qrX + c * qrModuleSize, qrY + r * qrModuleSize,
                   qrModuleSize, qrModuleSize, col);
    }
  }

  // SSID + password text below QR
  int textY = qrY + qrPixelSize + 14;
  drawCenteredText("Travel-Jellyfin", textY, 2, TFT_WHITE);
  drawCenteredText("Password: !IvyLee2015", textY + 22, 1, TFT_CYAN);
  drawCenteredText("Scan to join hotspot", textY + 40, 1, TFT_DARKGREY);
}

// ---- Page management ----

void drawCurrentPage() {
  switch (currentPage) {
    case 0: drawStatusPage(); break;
    case 1: drawKodiPage();   break;
    case 2: drawWiFiPage();   break;
  }
  pageNeedsRedraw = false;
}

void cyclePage() {
  currentPage = (currentPage + 1) % numPages;
  pageNeedsRedraw = true;
}

// ---- Touch handling ----

void handleTouch() {
  uint16_t tx, ty;
  if (tft.getTouch(&tx, &ty)) {
    lastTouch = millis();

    // Wake backlight if sleeping
    if (!backlightOn) {
      setBacklight(true);
      delay(100); // let user see the screen before registering tap
      return;     // don't process this as a click
    }

    Serial.printf("Touch at (%d, %d)\n", tx, ty);

    switch (currentPage) {
      case 0: // Status — tap anywhere cycles pages
        cyclePage();
        break;
      case 1: // Kodi remote
        handleKodiTouch(tx, ty);
        break;
      case 2: // WiFi QR — tap anywhere cycles pages
        cyclePage();
        break;
    }

    // Small debounce
    delay(150);
  }
}

// ---- Setup & Loop ----

void setup() {
  Serial.begin(115200);
  delay(200);

  // Pin setup
  pinMode(BL_PIN, OUTPUT);
  pinMode(LED_R, OUTPUT);
  pinMode(LED_G, OUTPUT);
  pinMode(LED_B, OUTPUT);
  setBacklight(true);
  setLED(0, 0, 1); // blue = booting

  // Display
  tft.init();
  tft.setRotation(TFT_ROTATION);
  tft.fillScreen(TFT_BLACK);

  // Touch
  tft.setTouch(calData);

  // Boot screen
  drawCenteredText("Travel Jellyfin", 120, 3, TFT_CYAN);
  drawCenteredText("Connecting to WiFi…", 170, 1, TFT_DARKGREY);

  // WiFi
  connectWiFi();

  // First status fetch
  fetchStatus();

  // Auto-switch to Kodi page if Kodi is running
  if (s_kodi) {
    currentPage = 1;
  }

  pageNeedsRedraw = true;
  lastTouch = millis();
}

void loop() {
  unsigned long now = millis();

  // ---- Touch ----
  handleTouch();

  // ---- Backlight sleep ----
  if (backlightOn && (now - lastTouch) > BACKLIGHT_TIMEOUT_MS) {
    setBacklight(false);
    Serial.println("Backlight sleep");
  }

  // ---- Poll API ----
  if (now - lastPoll > POLL_INTERVAL_MS || lastPoll == 0) {
    lastPoll = now;

    // Reconnect WiFi if dropped
    if (WiFi.status() != WL_CONNECTED) {
      setLED(1, 0, 0);
      connectWiFi();
    }

    bool ok = fetchStatus();

    // LED: green=online, red=offline
    if (ok) setLED(0, 1, 0);
    else    setLED(1, 0, 0);

    // Auto-switch to Kodi page when Kodi starts
    if (s_kodi && !kodiWasRunning && currentPage == 0) {
      currentPage = 1;
      pageNeedsRedraw = true;
    }
    // Auto-switch back to status when Kodi stops
    if (!s_kodi && kodiWasRunning && currentPage == 1) {
      currentPage = 0;
      pageNeedsRedraw = true;
    }
    kodiWasRunning = s_kodi;

    // Always redraw status page to update numbers
    if (currentPage == 0) {
      pageNeedsRedraw = true;
    }
    // Redraw Kodi page if running state changed
    if (currentPage == 1 && s_kodi != kodiWasRunning) {
      pageNeedsRedraw = true;
    }
  }

  // ---- Redraw if needed ----
  if (pageNeedsRedraw && backlightOn) {
    drawCurrentPage();
  }

  delay(20); // light yield
}