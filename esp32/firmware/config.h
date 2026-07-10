#ifndef CONFIG_H
#define CONFIG_H

// WiFi — connect to the Travel Jellyfin hotspot
#define WIFI_SSID     "Travel-Jellyfin"
#define WIFI_PASS     "YOUR_HOTSPOT_PASSWORD"

// API — mode controller endpoint
#define API_BASE_URL  "http://10.42.0.1:9090"
#define API_KEY       "YOUR_API_KEY"

// Polling interval (ms)
#define POLL_INTERVAL 3000

// Display colors (RGB565)
#define COLOR_BG      0x1A1A  // dark blue
#define COLOR_TEXT    0xFFFF  // white
#define COLOR_OK      0x07E0  // green
#define COLOR_WARN    0xFD20  // orange
#define COLOR_ERR     0xF800  // red
#define COLOR_ACCENT  0x07FF  // cyan
#define COLOR_BTN     0x4A4A  // gray
#define COLOR_BTN_ACT 0x7B0D  // dark cyan

#endif // CONFIG_H