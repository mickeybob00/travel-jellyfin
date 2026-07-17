#ifndef CONFIG_H
#define CONFIG_H

// ============================================================
// Travel Jellyfin CYD — Configuration
// ESP32-3248S035R (3.5" resistive touch, XPT2046, ST7796)
// ============================================================

// ---- WiFi ----
// The CYD joins the Pi's hotspot. On the road it connects to
// "Travel-Jellyfin" (10.42.0.1). At home it can join your house
// WiFi and reach the Pi at its eth0 IP.
#define WIFI_SSID_HOTSPOT   "Travel-Jellyfin"
#define WIFI_PASS_HOTSPOT   "!IvyLee2015"
#define WIFI_SSID_HOME      ""          // leave blank to skip home WiFi
#define WIFI_PASS_HOME      ""

// ---- Pi API ----
#define PI_API_HOST_HOTSPOT "10.42.0.1"
#define PI_API_HOST_HOME    "192.168.1.145"
#define PI_API_PORT         9090
#define PI_API_KEY          "tj-esp32-2024"

// ---- Polling ----
#define POLL_INTERVAL_MS    3000        // status poll every 3 s
#define BACKLIGHT_TIMEOUT_MS 60000      // screen sleep after 60 s no touch

// ---- Touch calibration (XPT2046 resistive) ----
// These are typical values for the 3.5" CYD in landscape (rotation=1).
// If touches are off, run the TFT_eSPI Touch_Calibrate sketch and
// paste the new numbers here.
#define TOUCH_CAL_LEFT      300
#define TOUCH_CAL_RIGHT     3700
#define TOUCH_CAL_TOP       300
#define TOUCH_CAL_BOT       3700

// ---- RGB LED (active-low) ----
#define LED_R   4
#define LED_G   17
#define LED_B   16

// ---- Backlight ----
#define BL_PIN  27

// ---- Display rotation ----
// 1 = landscape (480 wide x 320 tall)
#define TFT_ROTATION 1

#endif // CONFIG_H