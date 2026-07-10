# Travel Jellyfin

A portable, dual-mode entertainment system built on a Raspberry Pi 5, designed for
road trips and travel. Streams movies and TV shows to tablets in the car, or connects
to hotel TVs via HDMI and Kodi.

## Features

- **Car Mode:** Headless Jellyfin server + WiFi hotspot → tablets stream via Jellyfin app
- **Hotel Mode:** Kodi on HDMI → control with phone (Kore app) or ESP32 touchscreen
- **Auto-switching:** HDMI hotplug detection (udev) auto-starts/stops Kodi
- **Web Dashboard:** Mode controller on port 9090 with Pi system monitoring
- **Media Manager:** Web app on port 9091 to scan NAS, transfer, and transcode media
- **ESP32 Front Panel:** Cheap Yellow Display touchscreen for status & remote control
- **Hardware Acceleration:** V4L2M2M H.264/HEVC encoding on Pi 5

## Hardware

| Component | Specification |
|-----------|--------------|
| Raspberry Pi 5 | 16GB RAM |
| NVMe SSD | 500GB (via USB-NVMe adapter or HAT) |
| SD Card | 512GB (secondary storage) |
| WiFi | Built-in (hotspot mode) |
| HDMI | Auto-detected for hotel mode |
| ESP32-2432S028R | 2.8" touchscreen front panel (~$15) |

## Architecture

```
┌──────────────────────────────────────────────┐
│              Raspberry Pi 5                   │
│                                              │
│  ┌─────────┐  ┌──────────┐  ┌──────────────┐ │
│  │ Jellyfin │  │  Kodi    │  │ Mode Controller│ │
│  │  :8096   │  │ (on demand)│  │   :9090       │ │
│  └─────────┘  └──────────┘  └──────────────┘ │
│                                  │            │
│  ┌─────────┐         ┌──────────┐│            │
│  │ Media   │         │ HDMI     ││ JSON API    │
│  │ Manager │         │ Hotplug  ││            │
│  │  :9091  │         │ (udev)   ││            │
│  └─────────┘         └──────────┘│            │
│                                    ▼           │
│                          ┌────────────────┐    │
│  ┌─────────┐    WiFi     │  ESP32 CYD     │    │
│  │  NAS    │◄── NFS ───│  Touchscreen    │    │
│  │ (media) │    (read)  │  Front Panel    │    │
│  └─────────┘           └────────────────┘    │
│                                              │
│  ┌──────────────────────────────────────┐   │
│  │  WiFi Hotspot (10.42.0.1/24)        │   │
│  │  Tablets connect → Jellyfin app      │   │
│  └──────────────────────────────────────┘   │
└──────────────────────────────────────────────┘
```

## Quick Start

1. **Clone and run setup:**
   ```bash
   git clone https://github.com/mickeybob00/travel-jellyfin.git
   cd travel-jellyfin
   sudo bash setup.sh
   ```

2. **Configure** — copy `config.example.yaml` and fill in your values:
   - WiFi hotspot SSID/password
   - API key for ESP32
   - NAS NFS share path
   - Jellyfin credentials

3. **Set up WiFi hotspot** (NetworkManager):
   ```bash
   nmcli con add type wifi ifname wlan0 con-name Travel-Jellyfin-Hotspot \
     autoconnect yes ssid "Travel-Jellyfin"
   nmcli con modify Travel-Jellyfin-Hotspot 802-11-wireless.mode ap \
     802-11-wireless.band bg ipv4.method shared
   nmcli con modify Travel-Jellyfin-Hotspot wifi-sec.key-mgmt wpa-psk \
     wifi-sec.psk "YOUR_PASSWORD"
   nmcli con up Travel-Jellyfin-Hotspot
   ```

4. **Mount NAS media** (NFS, read-only):
   ```bash
   sudo mount -t nfs4 -o ro <NAS_IP>:/path/to/media /mnt/nas-media
   # Add to /etc/fstab for persistence
   ```

5. **Configure Jellyfin libraries:**
   - Movies: `/mnt/media/Movies`
   - TV Shows: `/mnt/media/TV Shows`

6. **Build ESP32 firmware** — see `esp32/firmware/README.md`

## ESP32 API

The mode controller exposes a lightweight JSON API for the ESP32 touchscreen:

| Endpoint | Method | Description |
|----------|--------|-------------|
| `/api/status` | GET | System status (temp, disk, network, mode) |
| `/api/version` | GET | API version info |
| `/api/mode/car` | POST | Switch to car mode (stop Kodi) |
| `/api/mode/hotel` | POST | Switch to hotel mode (start Kodi) |
| `/api/kodi/*` | POST | Kodi remote (up/down/left/right/select/back/home/playpause/stop/volume) |
| `/api/system/reboot` | POST | Reboot Pi (requires confirmation) |
| `/api/system/shutdown` | POST | Shutdown Pi (requires confirmation) |
| `/api/system/restart-jellyfin` | POST | Restart Jellyfin service |
| `/api/system/restart-kodi` | POST | Restart Kodi service |

Authentication: `Authorization: Bearer <API_KEY>` or `X-API-Key: <API_KEY>`

See `esp32/README.md` for full API documentation.

## Custom Housing

3D model files for a custom enclosure are in `models/`. The housing holds the
Pi 5, NVMe SSD, and ESP32 touchscreen with proper ventilation and cable management.

See `models/README.md` for component dimensions and print settings.

## Project Structure

```
travel-jellyfin/
├── setup.sh                    # One-command setup script
├── config.example.yaml         # Configuration template
├── README.md
├── scripts/
│   ├── mode-controller.py      # Web dashboard + ESP32 API (port 9090)
│   ├── media-manager.py        # NAS scan/transfer/transcode app (port 9091)
│   ├── hdmi-hotplug-handler.sh # udev HDMI detect → start/stop Kodi
│   ├── kodi-jellyfin-settings.xml # Pre-configured Kodi addon settings
│   └── fstab-nfs-example.txt   # NFS mount example
├── services/
│   ├── mode-controller.service # systemd service
│   ├── media-manager.service   # systemd service
│   └── kodi.service            # systemd service (on-demand only)
├── udev/
│   └── 99-hdmi-hotplug.rules   # HDMI hotplug udev rule
├── media-manager/
│   └── media-manager.py        # Media manager (also in scripts/)
├── esp32/
│   ├── README.md               # ESP32 API documentation
│   └── firmware/
│       ├── README.md           # Build instructions
│       └── config.h            # Firmware configuration template
├── models/
│   └── README.md               # 3D housing model documentation
└── docs/
    └── kodi-addon-setup.md     # Building Jellyfin for Kodi addon
```

## Usage

### Car Mode (default)
- Pi runs headless (Jellyfin + hotspot only)
- Connect tablets to WiFi "Travel-Jellyfin"
- Open Jellyfin app → `http://10.42.0.1:8096`

### Hotel Mode
- Plug HDMI into TV → Kodi auto-starts
- Or open `http://10.42.0.1:9090` on phone → tap "Hotel Mode"
- Control via Kore app or ESP32 touchscreen

### Back to Car Mode
- Unplug HDMI → Kodi auto-stops
- Or tap "Car Mode" on web dashboard / ESP32

## License

MIT — see LICENSE file.

## Acknowledgments

- [Jellyfin](https://jellyfin.org) — open source media server
- [Kodi](https://kodi.tv) — media center
- [ESP32-2432S028R](https://github.com/witnessmenow/ESP32-Cheap-Yellow-Display) — Cheap Yellow Display community