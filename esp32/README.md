# ESP32 Cheap Yellow Display — Arduino/PlatformIO Code

This directory contains the firmware for the ESP32-2432S028R (Cheap Yellow Display)
touchscreen that serves as the Travel Jellyfin front panel.

## Hardware

- **Display:** ESP32-2432S028R (2.8" 320x240 ILI9341 touchscreen)
- **Cost:** ~$10-15
- **Connection:** WiFi to the Travel-Jellyfin hotspot (10.42.0.1)

## Features

- **Car Mode:** System status display (CPU temp, disk usage, uptime, network)
- **Hotel Mode:** Kodi remote control (directional pad, play/pause, volume)
- **System Panel:** Mode switching, reboot, shutdown (with confirmation)

## API Reference

The ESP32 communicates with the mode controller at `http://10.42.0.1:9090`.

### Status Polling
```
GET /api/status
Authorization: Bearer <API_KEY>
```

### Mode Switching
```
POST /api/mode/car
POST /api/mode/hotel
Authorization: Bearer <API_KEY>
```

### Kodi Remote
```
POST /api/kodi/{up,down,left,right,select,back,home}
POST /api/kodi/playpause
POST /api/kodi/stop
POST /api/kodi/volumeup
POST /api/kodi/volumedown
Authorization: Bearer <API_KEY>
```

### System Management
```
POST /api/system/reboot      (requires confirmation)
POST /api/system/shutdown    (requires confirmation)
POST /api/system/restart-jellyfin
POST /api/system/restart-kodi
Authorization: Bearer <API_KEY>
```

### Dangerous Action Confirmation Flow
1. First POST returns a confirmation token (valid 30 seconds):
```json
{"status": "confirm_required", "confirm_token": "abc123...", "expires_in": 30}
```
2. Second POST with token to actually execute:
```
POST /api/system/reboot?confirm=1&token=abc123...
```

## Firmware

The firmware code is in `firmware/`. See `firmware/README.md` for build instructions.

## Pinout (ESP32-2432S028R)

| Function | GPIO |
|----------|------|
| TFT MOSI | 13  |
| TFT MISO | 12  |
| TFT SCK  | 14  |
| TFT CS   | 15  |
| TFT DC   | 2   |
| TFT RST  | -1  |
| TFT BL   | 21  |
| Touch CS | 5   |
| Touch IRQ| 16  |