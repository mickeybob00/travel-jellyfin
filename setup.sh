#!/bin/bash
# Travel Jellyfin — Setup Script
# Run this on a fresh Raspberry Pi 5 with Debian/Bookworm/Trixie
# Usage: sudo bash setup.sh

set -e

echo "========================================"
echo "  Travel Jellyfin — Setup Script"
echo "========================================"

# Check root
if [ "$EUID" -ne 0 ]; then
  echo "Please run as root: sudo bash setup.sh"
  exit 1
fi

INSTALL_USER="${1:-$(ls /home | head -1)}"
echo "Installing for user: $INSTALL_USER"

# --- 1. Install packages ---
echo ""
echo "[1/8] Installing packages..."
apt-get update -qq
DEBIAN_FRONTEND=noninteractive apt-get install -y -qq \
  jellyfin jellyfin-ffmpeg kodi kodi-bin kodi-data kodi-repository-kodi \
  kodi-inputstream-adaptive nfs-common python3 python3-yaml git 2>&1 | tail -3

# --- 2. Create directories ---
echo ""
echo "[2/8] Creating directories..."
mkdir -p /mnt/media/Movies "/mnt/media/TV Shows"
mkdir -p /srv/media/Movies "/srv/media/TV Shows"
mkdir -p /mnt/nas-media

# --- 3. Install mode controller ---
echo ""
echo "[3/8] Installing mode controller..."
cp scripts/mode-controller.py /usr/local/bin/mode-controller.py
chmod +x /usr/local/bin/mode-controller.py
cp services/mode-controller.service /etc/systemd/system/mode-controller.service

# --- 4. Install media manager ---
echo ""
echo "[4/8] Installing media manager..."
cp media-manager/media-manager.py /usr/local/bin/media-manager.py
chmod +x /usr/local/bin/media-manager.py
cp services/media-manager.service /etc/systemd/system/media-manager.service

# --- 5. Install Kodi service ---
echo ""
echo "[5/8] Installing Kodi service..."
cp services/kodi.service /etc/systemd/system/kodi.service

# --- 6. Install HDMI hotplug ---
echo ""
echo "[6/8] Installing HDMI hotplug detection..."
cp scripts/hdmi-hotplug-handler.sh /usr/local/bin/hdmi-hotplug-handler.sh
chmod +x /usr/local/bin/hdmi-hotplug-handler.sh
cp udev/99-hdmi-hotplug.rules /etc/udev/rules.d/99-hdmi-hotplug.rules

# --- 7. User groups ---
echo ""
echo "[7/8] Adding user to required groups..."
usermod -aG video,render,audio,input,tty "$INSTALL_USER"

# --- 8. Enable services ---
echo ""
echo "[8/8] Enabling services..."
systemctl daemon-reload
systemctl enable --now jellyfin.service
systemctl enable --now mode-controller.service
systemctl enable --now media-manager.service
# Do NOT enable kodi.service — it starts on demand via HDMI detect
udevadm control --reload-rules

echo ""
echo "========================================"
echo "  Setup complete!"
echo "========================================"
echo ""
echo "Next steps:"
echo "  1. Configure WiFi hotspot in NetworkManager"
echo "  2. Mount NAS NFS share (see config.example.yaml)"
echo "  3. Set API key in /usr/local/bin/mode-controller.py"
echo "  4. Add Jellyfin library paths for /mnt/media/Movies and /mnt/media/TV Shows"
echo "  5. Build & install Jellyfin for Kodi addon (see docs/kodi-addon-setup.md)"
echo ""
echo "Dashboards:"
echo "  Mode Controller: http://<PI_IP>:9090"
echo "  Media Manager:   http://<PI_IP>:9091"
echo "  Jellyfin:        http://<PI_IP>:8096"