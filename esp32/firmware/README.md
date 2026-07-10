# ESP32 Firmware — Travel Jellyfin Front Panel

## Building

### Option 1: Arduino IDE
1. Install ESP32 board support (Boards Manager → "esp32")
2. Install libraries:
   - TFT_eSPI by Bodmer
   - XPT2046_Touchscreen by Paul Stoffregen
   - ArduinoJson by Benoit Blanchon
   - HTTPClient (built-in)
3. Configure TFT_eSPI for ESP32-2432S028R (ILI9341, 320x240)
4. Open `travel_jellyfin_esp32.ino`
5. Set WiFi SSID, password, and API key
6. Upload

### Option 2: PlatformIO
```bash
pip install platformio
cd firmware
platformio run --target upload
```

## Configuration

Edit `config.h` before building:

```cpp
#define WIFI_SSID     "Travel-Jellyfin"
#define WIFI_PASS     "YOUR_HOTSPOT_PASSWORD"
#define API_BASE_URL  "http://10.42.0.1:9090"
#define API_KEY       "YOUR_API_KEY"
```

## UI Layout

### Car Mode (Status Display)
```
┌─────────────────────┐
│ 🎬 Travel Jellyfin   │
│ ┌─────────────────┐  │
│ │ CAR MODE    ✅  │  │
│ └─────────────────┘  │
│ CPU:    49°C  ████░  │
│ NVMe:   40°C  ███░░  │
│ Disk:   373/503 GB   │
│ RAM:    0.7/15.8 GB  │
│ Uptime: 74h 30m     │
│ WiFi:   10.42.0.1    │
│ Hotspot: ✅ Active  │
│ Jellyfin: ✅ Active │
└─────────────────────┘
```

### Hotel Mode (Kodi Remote)
```
┌─────────────────────┐
│ 🏨 HOTEL MODE  ✅   │
│ ┌─────┐ ┌─────┐    │
│ │  ⬆  │ │ ⏯  │    │
│ └─────┘ └─────┘    │
│ ┌───┐┌───┐┌───┐  │
│ │ ⬅ ││ OK││ ➡ │  │
│ └───┘└───┘└───┘  │
│ ┌─────┐ ┌─────┐    │
│ │  ⬇  │ │ ⏹  │    │
│ └─────┘ └─────┘    │
│ 🔊─  [BACK] [HOME] │
│ ─🔊  ⏪ ⏩         │
└─────────────────────┘
```

### System Panel (Settings)
```
┌─────────────────────┐
│ ⚙️ SYSTEM           │
│ ┌─────────────────┐ │
│ │ 🚗 Car Mode     │ │
│ │ 🏨 Hotel Mode   │ │
│ │ 🔄 Restart JF   │ │
│ │ 🔄 Restart Kodi │ │
│ │ ♻️ Reboot Pi    │ │
│ │ ⏻ Shutdown Pi  │ │
│ └─────────────────┘ │
│ ⚠️ Reboot & Shutdown│
│ require confirm     │
└─────────────────────┘