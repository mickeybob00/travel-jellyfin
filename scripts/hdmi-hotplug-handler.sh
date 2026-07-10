#!/bin/bash
# HDMI hotplug handler - starts/stops Kodi based on HDMI connection
# Called by udev rule on DRM hotplug events

LOG_TAG="hdmi-hotplug"

# Get the connector status from DRM sysfs
HDMI_STATUS=$(cat /sys/class/drm/card1-HDMI-A-1/status 2>/dev/null || cat /sys/class/drm/card0-HDMI-A-1/status 2>/dev/null || echo "disconnected")

logger -t "$LOG_TAG" "HDMI hotplug event detected, status: $HDMI_STATUS"

# Prevent rapid toggle bouncing
LOCKFILE="/run/hdmi-hotplug.lock"
exec 200>"$LOCKFILE"
flock -n 200 || exit 0

if [ "$HDMI_STATUS" = "connected" ]; then
    logger -t "$LOG_TAG" "HDMI connected - starting hotel mode"
    # Only start if not already running
    if ! systemctl is-active --quiet kodi; then
        systemctl start kodi 2>/dev/null
        logger -t "$LOG_TAG" "Kodi started"
    fi
else
    logger -t "$LOG_TAG" "HDMI disconnected - stopping hotel mode"
    if systemctl is-active --quiet kodi; then
        systemctl stop kodi 2>/dev/null
        logger -t "$LOG_TAG" "Kodi stopped"
    fi
fi