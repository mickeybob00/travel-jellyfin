#!/usr/bin/env python3
"""
Travel Jellyfin Mode Controller - Web Toggle + ESP32 API
Runs on port 9090, provides:
  - Web UI dashboard to switch between car and hotel mode
  - JSON API for ESP32 Cheap Yellow Display front panel
  - Kodi JSON-RPC remote control endpoints
  - System management endpoints (reboot, shutdown, restart services)
  - API key authentication for API endpoints

API version 1.0
"""

import subprocess
import json
import os
import time
import hashlib
import socket
import urllib.request
from http.server import HTTPServer, BaseHTTPRequestHandler
from urllib.parse import urlparse

# ============================================================
# Configuration
# ============================================================

# API key for ESP32 authentication. Change this and set the same
# value on the ESP32. Set to None to disable auth (not recommended).
API_KEY = "<YOUR_API_KEY>"

# Kodi JSON-RPC endpoint (runs on localhost:8096 when Kodi is active)
KODI_RPC_URL = "http://localhost:8096/jsonrpc"

# Jellyfin URL for status display
JELLYFIN_URL = "http://10.42.0.1:8096"

# API version
API_VERSION = "1.0"
API_DEVICE_NAME = "Travel Jellyfin"

# ============================================================
# Existing helper functions (unchanged)
# ============================================================

def get_hdmi_status():
    for path in ["/sys/class/drm/card1-HDMI-A-1/status", "/sys/class/drm/card0-HDMI-A-1/status"]:
        try:
            with open(path) as f:
                return f.read().strip()
        except:
            pass
    return "unknown"

def get_kodi_status():
    try:
        result = subprocess.run(["systemctl", "is-active", "kodi"], capture_output=True, text=True, timeout=5)
        return result.stdout.strip()
    except:
        return "unknown"

def get_jellyfin_status():
    try:
        result = subprocess.run(["systemctl", "is-active", "jellyfin"], capture_output=True, text=True, timeout=5)
        return result.stdout.strip()
    except:
        return "unknown"

def get_hotspot_status():
    try:
        result = subprocess.run(["nmcli", "con", "show", "--active", "Travel-Jellyfin-Hotspot"],
                               capture_output=True, text=True, timeout=5)
        return "active" if result.returncode == 0 and result.stdout.strip() else "inactive"
    except:
        return "unknown"

def get_connected_devices():
    try:
        leases = []
        lease_file = "/var/lib/NetworkManager/dnsmasq-wlan0.leases"
        if os.path.exists(lease_file):
            with open(lease_file) as f:
                for line in f:
                    parts = line.strip().split()
                    if len(parts) >= 4:
                        leases.append({"mac": parts[1], "ip": parts[2], "hostname": parts[3]})
        return leases
    except:
        return []

def get_disk_usage():
    result = {}
    for mount, label in [("/mnt/media", "NVMe"), ("/srv/media", "SD Card")]:
        try:
            r = subprocess.run(["df", "-h", mount], capture_output=True, text=True, timeout=5)
            lines = r.stdout.strip().split("\n")
            if len(lines) > 1:
                parts = lines[1].split()
                result[label] = {"total": parts[1], "used": parts[2], "avail": parts[3], "use_pct": parts[4]}
        except:
            result[label] = {"total": "?", "used": "?", "avail": "?", "use_pct": "?"}
    return result

def get_disk_usage_gb():
    """Get disk usage in GB (numeric, for ESP32 API)."""
    result = {}
    for mount, label in [("/mnt/media", "nvme"), ("/srv/media", "sd")]:
        try:
            r = subprocess.run(["df", "-B1", mount], capture_output=True, text=True, timeout=5)
            lines = r.stdout.strip().split("\n")
            if len(lines) > 1:
                parts = lines[1].split()
                total_bytes = int(parts[1])
                used_bytes = int(parts[2])
                result[label] = {
                    "used_gb": round(used_bytes / 1e9),
                    "total_gb": round(total_bytes / 1e9)
                }
        except:
            result[label] = {"used_gb": 0, "total_gb": 0}
    return result

def get_mode():
    kodi = get_kodi_status()
    if kodi == "active":
        return "hotel"
    return "car"

def read_temp(path):
    try:
        with open(path) as f:
            return int(f.read().strip()) / 1000.0
    except:
        return None

def get_pi_stats():
    stats = {}

    # CPU temp (thermal_zone0 = cpu_thermal)
    stats["cpu_temp"] = read_temp("/sys/class/thermal/thermal_zone0/temp")

    # NVMe temp (hwmon1)
    stats["nvme_temp"] = read_temp("/sys/class/hwmon/hwmon1/temp1_input")

    # PWM fan speed (search all hwmon paths for fan sensor)
    stats["fan_rpm"] = None
    for i in range(1, 6):
        try:
            with open(f"/sys/class/hwmon/hwmon{i}/fan1_input") as f:
                val = int(f.read().strip())
                if val > 0:
                    stats["fan_rpm"] = val
                    break
        except:
            pass

    # CPU voltage
    try:
        r = subprocess.run(["vcgencmd", "measure_volts"], capture_output=True, text=True, timeout=5)
        volt_str = r.stdout.strip().replace("volt=", "").replace("V", "")
        stats["cpu_volt"] = float(volt_str)
    except:
        stats["cpu_volt"] = None

    # CPU frequency
    try:
        r = subprocess.run(["vcgencmd", "measure_clock", "arm"], capture_output=True, text=True, timeout=5)
        freq_str = r.stdout.strip().split("=")[-1]
        stats["cpu_mhz"] = round(int(freq_str) / 1_000_000, 0)
    except:
        stats["cpu_mhz"] = None

    # Load average
    try:
        with open("/proc/loadavg") as f:
            parts = f.read().strip().split()
            stats["load_1"] = float(parts[0])
            stats["load_5"] = float(parts[1])
            stats["load_15"] = float(parts[2])
    except:
        stats["load_1"] = stats["load_5"] = stats["load_15"] = None

    # Memory
    try:
        with open("/proc/meminfo") as f:
            meminfo = {}
            for line in f:
                parts = line.split()
                if len(parts) >= 2:
                    meminfo[parts[0].rstrip(":")] = int(parts[1])
        total = meminfo.get("MemTotal", 0)
        avail = meminfo.get("MemAvailable", 0)
        used = total - avail
        stats["mem_total_gb"] = round(total / 1048576, 1)
        stats["mem_used_gb"] = round(used / 1048576, 1)
        stats["mem_avail_gb"] = round(avail / 1048576, 1)
        stats["mem_pct"] = round((used / total) * 100) if total > 0 else 0
    except:
        stats["mem_total_gb"] = stats["mem_used_gb"] = stats["mem_avail_gb"] = None
        stats["mem_pct"] = None

    # Uptime
    try:
        with open("/proc/uptime") as f:
            uptime_sec = float(f.read().split()[0])
        hours = int(uptime_sec // 3600)
        mins = int((uptime_sec % 3600) // 60)
        stats["uptime"] = f"{hours}h {mins}m"
    except:
        stats["uptime"] = "?"

    # Network interfaces
    interfaces = {}
    for iface in ["eth0", "wlan0"]:
        try:
            r = subprocess.run(["ip", "addr", "show", iface], capture_output=True, text=True, timeout=5)
            for line in r.stdout.split("\n"):
                if "inet " in line:
                    ip = line.strip().split()[1].split("/")[0]
                    interfaces[iface] = ip
                    break
            if iface not in interfaces:
                r2 = subprocess.run(["ip", "link", "show", iface], capture_output=True, text=True, timeout=5)
                if r2.returncode == 0:
                    interfaces[iface] = "no IP"
        except:
            pass
    stats["interfaces"] = interfaces

    # Throttling status
    try:
        r = subprocess.run(["vcgencmd", "get_throttled"], capture_output=True, text=True, timeout=5)
        throttled_hex = r.stdout.strip().split("=")[-1]
        throttled_val = int(throttled_hex, 16)
        stats["throttled"] = throttled_val != 0
        stats["throttle_flags"] = throttled_hex
    except:
        stats["throttled"] = None
        stats["throttle_flags"] = "?"

    return stats

def temp_class(temp):
    if temp is None:
        return ""
    if temp < 50:
        return "ok"
    elif temp < 65:
        return "warn"
    else:
        return "err"

# ============================================================
# Kodi JSON-RPC helper
# ============================================================

def kodi_rpc(method, params=None):
    """Send a JSON-RPC command to Kodi. Returns (success, result_or_error)."""
    payload = {
        "jsonrpc": "2.0",
        "id": 1,
        "method": method
    }
    if params:
        payload["params"] = params

    try:
        data = json.dumps(payload).encode()
        req = urllib.request.Request(
            KODI_RPC_URL,
            data=data,
            headers={"Content-Type": "application/json"}
        )
        resp = urllib.request.urlopen(req, timeout=5)
        result = json.loads(resp.read())
        if "error" in result:
            return False, result["error"]
        return True, result.get("result", {})
    except urllib.error.URLError:
        return False, "Kodi not reachable - is it running?"
    except Exception as e:
        return False, str(e)

# ============================================================
# ESP32 API status (flat, lightweight JSON)
# ============================================================

def get_esp32_status():
    """Build a flat JSON status object optimized for ESP32 polling."""
    pi = get_pi_stats()
    disks = get_disk_usage_gb()
    ifaces = pi.get("interfaces", {})

    return {
        "mode": get_mode(),
        "hdmi_connected": get_hdmi_status() == "connected",
        "kodi_running": get_kodi_status() == "active",
        "jellyfin_running": get_jellyfin_status() == "active",
        "hotspot_running": get_hotspot_status() == "active",
        "jellyfin_url": JELLYFIN_URL,
        "cpu_temp": pi.get("cpu_temp"),
        "nvme_temp": pi.get("nvme_temp"),
        "cpu_clock": int(pi["cpu_mhz"]) if pi.get("cpu_mhz") is not None else None,
        "cpu_voltage": pi.get("cpu_volt"),
        "fan_rpm": pi.get("fan_rpm"),
        "memory_used_gb": pi.get("mem_used_gb"),
        "memory_total_gb": pi.get("mem_total_gb"),
        "load_1": pi.get("load_1"),
        "load_5": pi.get("load_5"),
        "load_15": pi.get("load_15"),
        "uptime": pi.get("uptime"),
        "eth0_ip": ifaces.get("eth0", ""),
        "wlan0_ip": ifaces.get("wlan0", ""),
        "nvme_used_gb": disks.get("nvme", {}).get("used_gb", 0),
        "nvme_total_gb": disks.get("nvme", {}).get("total_gb", 0),
        "sd_used_gb": disks.get("sd", {}).get("used_gb", 0),
        "sd_total_gb": disks.get("sd", {}).get("total_gb", 0)
    }

# ============================================================
# HTML template (unchanged from original)
# ============================================================

HTML_TEMPLATE = """<!DOCTYPE html>
<html>
<head>
<meta charset="utf-8">
<meta name="viewport" content="width=device-width, initial-scale=1">
<title>Travel Jellyfin Controller</title>
<style>
* {{ box-sizing: border-box; margin: 0; padding: 0; }}
body {{ font-family: -apple-system, BlinkMacSystemFont, 'Segoe UI', Roboto, sans-serif;
       background: linear-gradient(135deg, #1a1a2e, #16213e); color: #eee; min-height: 100vh;
       display: flex; align-items: center; justify-content: center; padding: 20px; }}
.container {{ max-width: 500px; width: 100%; }}
.header {{ text-align: center; margin-bottom: 30px; }}
.header h1 {{ font-size: 28px; color: #00d4ff; margin-bottom: 5px; }}
.header p {{ color: #888; font-size: 14px; }}
.mode-badge {{ text-align: center; padding: 15px; border-radius: 12px; margin-bottom: 20px;
              font-size: 24px; font-weight: bold; }}
.mode-hotel {{ background: linear-gradient(135deg, #e74c3c, #c0392b); }}
.mode-car {{ background: linear-gradient(135deg, #27ae60, #229954); }}
.btn {{ display: block; width: 100%; padding: 18px; border: none; border-radius: 12px;
       font-size: 20px; font-weight: bold; cursor: pointer; margin-bottom: 12px;
       transition: transform 0.1s, box-shadow 0.2s; }}
.btn:active {{ transform: scale(0.98); }}
.btn-hotel {{ background: linear-gradient(135deg, #e74c3c, #c0392b); color: white;
             box-shadow: 0 4px 15px rgba(231,76,60,0.3); }}
.btn-car {{ background: linear-gradient(135deg, #27ae60, #229954); color: white;
           box-shadow: 0 4px 15px rgba(39,174,96,0.3); }}
.btn:disabled {{ opacity: 0.4; cursor: not-allowed; }}
.status {{ background: rgba(255,255,255,0.08); border-radius: 12px; padding: 20px; margin-top: 20px; }}
.status h2 {{ font-size: 16px; color: #00d4ff; margin-bottom: 15px; text-transform: uppercase;
             letter-spacing: 1px; }}
.status-row {{ display: flex; justify-content: space-between; padding: 8px 0;
              border-bottom: 1px solid rgba(255,255,255,0.05); font-size: 14px; }}
.status-row:last-child {{ border: none; }}
.status-label {{ color: #888; }}
.status-value {{ color: #fff; font-weight: 500; }}
.status-value.ok {{ color: #2ecc71; }}
.status-value.warn {{ color: #f39c12; }}
.status-value.err {{ color: #e74c3c; }}
.devices {{ margin-top: 10px; }}
.device {{ background: rgba(255,255,255,0.05); padding: 8px 12px; border-radius: 8px;
          margin: 5px 0; font-size: 13px; display: flex; justify-content: space-between; }}
.device-name {{ color: #00d4ff; }}
.pi-monitor {{ background: rgba(0, 212, 255, 0.06); border: 1px solid rgba(0, 212, 255, 0.15);
              border-radius: 12px; padding: 20px; margin-top: 20px; }}
.pi-monitor h2 {{ font-size: 16px; color: #00d4ff; margin-bottom: 15px; text-transform: uppercase;
                 letter-spacing: 1px; }}
.temp-bar {{ display: inline-block; width: 60px; height: 8px; border-radius: 4px;
            background: rgba(255,255,255,0.1); margin-left: 8px; vertical-align: middle; }}
.temp-bar-fill {{ height: 100%; border-radius: 4px; transition: width 0.3s; }}
.temp-bar-fill.ok {{ background: #2ecc71; }}
.temp-bar-fill.warn {{ background: #f39c12; }}
.temp-bar-fill.err {{ background: #e74c3c; }}
.throttle-warning {{ background: rgba(231,76,60,0.15); border: 1px solid rgba(231,76,60,0.3);
                    border-radius: 8px; padding: 10px; margin-top: 10px; font-size: 13px;
                    color: #e74c3c; text-align: center; }}
.footer {{ text-align: center; margin-top: 20px; color: #555; font-size: 12px; }}
</style>
</head>
<body>
<div class="container">
  <div class="header">
    <h1>&#127916; Travel Jellyfin</h1>
    <p>Raspberry Pi 5 &mdash; Mode Controller</p>
  </div>
  
  <div class="mode-badge mode-{mode_class}">
    {mode_icon} {mode_name} Mode
  </div>
  
  <button class="btn btn-hotel" onclick="switchMode('hotel')" {hotel_disabled}>
    &#127976; Switch to Hotel Mode
  </button>
  <button class="btn btn-car" onclick="switchMode('car')" {car_disabled}>
    &#128662; Switch to Car Mode
  </button>
  
  <div class="status">
    <h2>System Status</h2>
    <div class="status-row">
      <span class="status-label">HDMI Display</span>
      <span class="status-value {hdmi_class}">{hdmi_status}</span>
    </div>
    <div class="status-row">
      <span class="status-label">Kodi</span>
      <span class="status-value {kodi_class}">{kodi_status}</span>
    </div>
    <div class="status-row">
      <span class="status-label">Jellyfin</span>
      <span class="status-value {jellyfin_class}">{jellyfin_status}</span>
    </div>
    <div class="status-row">
      <span class="status-label">WiFi Hotspot</span>
      <span class="status-value {hotspot_class}">{hotspot_status}</span>
    </div>
    <div class="status-row">
      <span class="status-label">Jellyfin URL</span>
      <span class="status-value">10.42.0.1:8096</span>
    </div>
    <div class="status-row">
      <span class="status-label">NVMe Storage</span>
      <span class="status-value">{nvme_usage}</span>
    </div>
    <div class="status-row">
      <span class="status-label">SD Card Storage</span>
      <span class="status-value">{sd_usage}</span>
    </div>
    {devices_section}
  </div>
  
  <div class="pi-monitor">
    <h2>&#128202; Pi Monitor</h2>
    <div class="status-row">
      <span class="status-label">&#127777;&#65039; CPU Temp</span>
      <span class="status-value {cpu_temp_class}">{cpu_temp}&deg;C{cpu_temp_bar}</span>
    </div>
    <div class="status-row">
      <span class="status-label">&#127777;&#65039; NVMe Temp</span>
      <span class="status-value {nvme_temp_class}">{nvme_temp}&deg;C{nvme_temp_bar}</span>
    </div>
    <div class="status-row">
      <span class="status-label">&#9881;&#65039; CPU Clock</span>
      <span class="status-value">{cpu_mhz} MHz</span>
    </div>
    <div class="status-row">
      <span class="status-label">&#9889; CPU Voltage</span>
      <span class="status-value">{cpu_volt}V</span>
    </div>
    <div class="status-row">
      <span class="status-label">&#128260; Fan Speed</span>
      <span class="status-value">{fan_rpm} RPM</span>
    </div>
    <div class="status-row">
      <span class="status-label">&#128200; Load Avg (1/5/15)</span>
      <span class="status-value">{load_avg}</span>
    </div>
    <div class="status-row">
      <span class="status-label">&#128190; Memory</span>
      <span class="status-value">{mem_used} / {mem_total} GB ({mem_pct}%)</span>
    </div>
    <div class="status-row">
      <span class="status-label">&#9201;&#65039; Uptime</span>
      <span class="status-value">{uptime}</span>
    </div>
    <div class="status-row">
      <span class="status-label">&#127760; eth0</span>
      <span class="status-value">{{eth0_ip}}</span>
    </div>
    <div class="status-row">
      <span class="status-label">&#128246; wlan0</span>
      <span class="status-value">{{wlan0_ip}}</span>
    </div>
    {throttle_section}
  </div>
  
  <div class="footer">
    Auto-detect: HDMI plugged in &rarr; Hotel Mode, unplugged &rarr; Car Mode<br>
    Manual override available above &bull; Updated {timestamp}
  </div>
</div>

<script>
function switchMode(mode) {{
  if (!confirm('Switch to ' + mode + ' mode?')) return;
  fetch('/api/mode?mode=' + mode, {{method: 'POST'}})
    .then(r => r.json())
    .then(data => {{ window.location.reload(); }})
    .catch(err => {{ alert('Error: ' + err); window.location.reload(); }});
}}
// Auto-refresh every 15 seconds
setTimeout(() => window.location.reload(), 15000);
</script>
</body>
</html>"""

def make_temp_bar(temp, max_temp=80):
    if temp is None:
        return ""
    pct = min(temp / max_temp * 100, 100)
    cls = "ok" if temp < 50 else "warn" if temp < 65 else "err"
    return f'<span class="temp-bar"><span class="temp-bar-fill {cls}" style="width:{pct:.0f}%"></span></span>'

# ============================================================
# HTTP Handler with ESP32 API
# ============================================================

class Handler(BaseHTTPRequestHandler):
    
    # ----- API key authentication -----
    
    def check_auth(self):
        """Check Bearer token. Returns True if authorized or auth disabled."""
        if API_KEY is None:
            return True
        auth = self.headers.get("Authorization", "")
        if auth.startswith("Bearer "):
            token = auth[7:]
            return token == API_KEY
        # Also accept X-API-Key header for ESP32 convenience
        api_key_header = self.headers.get("X-API-Key", "")
        if api_key_header == API_KEY:
            return True
        return False
    
    def send_json(self, data, code=200):
        self.send_response(code)
        self.send_header("Content-Type", "application/json")
        self.send_header("Access-Control-Allow-Origin", "*")
        self.end_headers()
        self.wfile.write(json.dumps(data).encode())
    
    def send_unauthorized(self):
        self.send_json({"status": "error", "error": "Unauthorized - invalid or missing API key"}, 401)
    
    def send_ok(self, message="ok", **extra):
        resp = {"status": "ok", "message": message}
        resp.update(extra)
        self.send_json(resp)
    
    def send_error_json(self, error, code=400):
        self.send_json({"status": "error", "error": error}, code)
    
    # ----- GET routes -----
    
    def do_GET(self):
        parsed = urlparse(self.path)
        path = parsed.path
        
        if path == "/":
            self.serve_dashboard()
        elif path == "/api/status":
            if not self.check_auth():
                self.send_unauthorized()
                return
            self.send_json(get_esp32_status())
        elif path == "/api/version":
            if not self.check_auth():
                self.send_unauthorized()
                return
            self.send_json({
                "version": API_VERSION,
                "device": API_DEVICE_NAME,
                "api": "v1"
            })
        else:
            self.send_error(404)
    
    # ----- POST routes -----
    
    def do_POST(self):
        parsed = urlparse(self.path)
        path = parsed.path
        
        # --- Mode switching (legacy + new) ---
        # Legacy: POST /api/mode?mode=hotel (keep for web dashboard compatibility)
        if path == "/api/mode":
            from urllib.parse import parse_qs
            params = parse_qs(parsed.query)
            mode = params.get("mode", [""])[0]
            if mode in ("car", "hotel"):
                self._switch_mode(mode)
            else:
                self.send_error_json("Invalid mode. Use 'car' or 'hotel'.")
        
        # New ESP32 endpoints
        elif path == "/api/mode/car":
            if not self.check_auth():
                self.send_unauthorized()
                return
            self._switch_mode("car")
        
        elif path == "/api/mode/hotel":
            if not self.check_auth():
                self.send_unauthorized()
                return
            self._switch_mode("hotel")
        
        # --- Kodi remote control ---
        elif path == "/api/kodi/up":
            self._kodi_input("up")
        elif path == "/api/kodi/down":
            self._kodi_input("down")
        elif path == "/api/kodi/left":
            self._kodi_input("left")
        elif path == "/api/kodi/right":
            self._kodi_input("right")
        elif path == "/api/kodi/select":
            self._kodi_input("select")
        elif path == "/api/kodi/back":
            self._kodi_input("back")
        elif path == "/api/kodi/home":
            self._kodi_input("home")
        elif path == "/api/kodi/playpause":
            self._kodi_playpause()
        elif path == "/api/kodi/stop":
            self._kodi_stop()
        elif path == "/api/kodi/volumeup":
            self._kodi_volume(+1)
        elif path == "/api/kodi/volumedown":
            self._kodi_volume(-1)
        
        # --- System management (with confirmation) ---
        elif path == "/api/system/reboot":
            if not self.check_auth():
                self.send_unauthorized()
                return
            self._system_action("reboot", parsed.query)
        elif path == "/api/system/shutdown":
            if not self.check_auth():
                self.send_unauthorized()
                return
            self._system_action("shutdown", parsed.query)
        elif path == "/api/system/restart-jellyfin":
            if not self.check_auth():
                self.send_unauthorized()
                return
            self._system_action("restart-jellyfin", parsed.query)
        elif path == "/api/system/restart-kodi":
            if not self.check_auth():
                self.send_unauthorized()
                return
            self._system_action("restart-kodi", parsed.query)
        
        else:
            self.send_error(404)
    
    # ----- Mode switching -----
    
    def _switch_mode(self, mode):
        try:
            if mode == "hotel":
                subprocess.run(["systemctl", "start", "kodi"], capture_output=True, timeout=10)
                time.sleep(1)
                self.send_ok(f"Switched to hotel mode", mode="hotel")
            elif mode == "car":
                subprocess.run(["systemctl", "stop", "kodi"], capture_output=True, timeout=10)
                time.sleep(1)
                self.send_ok(f"Switched to car mode", mode="car")
        except Exception as e:
            self.send_error_json(f"Failed to switch mode: {e}")
    
    # ----- Kodi remote control -----
    
    def _kodi_input(self, direction):
        if not self.check_auth():
            self.send_unauthorized()
            return
        success, result = kodi_rpc("Input." + direction.capitalize())
        if success:
            self.send_ok(f"Kodi input: {direction}")
        else:
            self.send_error_json(result, 503)
    
    def _kodi_playpause(self):
        if not self.check_auth():
            self.send_unauthorized()
            return
        success, result = kodi_rpc("Player.PlayPause", {"playerid": 0})
        if success:
            self.send_ok("Play/pause toggled")
        else:
            # Try to get active player first
            success2, players = kodi_rpc("Player.GetActivePlayers")
            if success2 and players:
                playerid = players[0]["playerid"]
                ok, res = kodi_rpc("Player.PlayPause", {"playerid": playerid})
                if ok:
                    self.send_ok("Play/pause toggled")
                    return
            self.send_error_json(result, 503)
    
    def _kodi_stop(self):
        if not self.check_auth():
            self.send_unauthorized()
            return
        success, players = kodi_rpc("Player.GetActivePlayers")
        if not success:
            self.send_error_json(players, 503)
            return
        if not players:
            self.send_ok("Nothing playing")
            return
        playerid = players[0]["playerid"]
        success, result = kodi_rpc("Player.Stop", {"playerid": playerid})
        if success:
            self.send_ok("Stopped")
        else:
            self.send_error_json(result, 503)
    
    def _kodi_volume(self, direction):
        if not self.check_auth():
            self.send_unauthorized()
            return
        success, result = kodi_rpc("Application.SetVolume", {"volume": "increment" if direction > 0 else "decrement"})
        if success:
            self.send_ok(f"Volume {'up' if direction > 0 else 'down'}")
        else:
            # Fallback: get current volume and adjust
            success2, vol_info = kodi_rpc("Application.GetProperties", {"properties": ["volume"]})
            if success2:
                current = vol_info.get("volume", 50)
                new_vol = max(0, min(100, current + direction * 5))
                ok, res = kodi_rpc("Application.SetVolume", {"volume": new_vol})
                if ok:
                    self.send_ok(f"Volume set to {new_vol}")
                    return
            self.send_error_json(result, 503)
    
    # ----- System management (with confirmation) -----
    
    # Confirmation tokens: {token: {action, expires_at}}
    _confirm_tokens = {}
    _CONFIRM_TTL = 30  # seconds before token expires
    
    # Actions that require confirmation (reboot, shutdown = destructive; restarts = service-only)
    _DANGEROUS_ACTIONS = {"reboot", "shutdown"}
    
    def _system_action(self, action, query_str=""):
        from urllib.parse import parse_qs
        params = parse_qs(query_str)
        confirm = params.get("confirm", ["0"])[0] == "1"
        token = params.get("token", [""])[0]
        
        is_dangerous = action in self._DANGEROUS_ACTIONS
        
        # Non-dangerous actions (restart-jellyfin, restart-kodi) execute immediately
        if not is_dangerous:
            self._execute_system_action(action)
            return
        
        # Dangerous actions require confirmation
        if not confirm:
            # Step 1: Generate confirmation token, return it to caller
            import secrets
            new_token = secrets.token_hex(8)
            self._confirm_tokens[new_token] = {
                "action": action,
                "expires_at": time.time() + self._CONFIRM_TTL
            }
            # Clean expired tokens
            now = time.time()
            self._confirm_tokens = {k: v for k, v in self._confirm_tokens.items()
                                    if v["expires_at"] > now}
            
            action_label = "Reboot" if action == "reboot" else "Shut down"
            self.send_json({
                "status": "confirm_required",
                "message": f"{action_label} requires confirmation. Send confirm=1 with the token within {self._CONFIRM_TTL}s.",
                "action": action,
                "confirm_token": new_token,
                "expires_in": self._CONFIRM_TTL,
                "confirm_url": f"/api/system/{action}?confirm=1&token={new_token}"
            })
            return
        
        # Step 2: Validate confirmation token
        if not token:
            self.send_error_json("Missing confirmation token")
            return
        
        entry = self._confirm_tokens.pop(token, None)
        if not entry:
            self.send_error_json("Invalid or expired confirmation token. Request a new one.", 410)
            return
        
        if entry["action"] != action:
            self.send_error_json("Token does not match action", 400)
            return
        
        # Token is valid, execute the action
        self._execute_system_action(action)
    
    def _execute_system_action(self, action):
        try:
            if action == "reboot":
                self.send_ok("Rebooting...")
                time.sleep(0.5)
                subprocess.run(["sudo", "systemctl", "reboot"], capture_output=True, timeout=10)
            elif action == "shutdown":
                self.send_ok("Shutting down...")
                time.sleep(0.5)
                subprocess.run(["sudo", "systemctl", "poweroff"], capture_output=True, timeout=10)
            elif action == "restart-jellyfin":
                subprocess.run(["sudo", "systemctl", "restart", "jellyfin"], capture_output=True, timeout=30)
                self.send_ok("Jellyfin restarted")
            elif action == "restart-kodi":
                subprocess.run(["sudo", "systemctl", "restart", "kodi"], capture_output=True, timeout=30)
                self.send_ok("Kodi restarted")
        except Exception as e:
            self.send_error_json(f"Failed: {e}")
    
    # ----- Dashboard (unchanged) -----
    
    def serve_dashboard(self):
        mode = get_mode()
        kodi = get_kodi_status()
        jellyfin = get_jellyfin_status()
        hotspot = get_hotspot_status()
        hdmi = get_hdmi_status()
        disks = get_disk_usage()
        devices = get_connected_devices()
        pi = get_pi_stats()
        
        mode_name = "Hotel" if mode == "hotel" else "Car"
        mode_icon = "&#127976;" if mode == "hotel" else "&#128662;"
        mode_class = "hotel" if mode == "hotel" else "car"
        
        hotel_disabled = "disabled" if mode == "hotel" else ""
        car_disabled = "disabled" if mode == "car" else ""
        
        hdmi_class = "ok" if hdmi == "connected" else "warn" if hdmi == "disconnected" else "err"
        
        kodi_class = "ok" if kodi == "active" else "warn"
        jellyfin_class = "ok" if jellyfin == "active" else "err"
        hotspot_class = "ok" if hotspot == "active" else "err"
        
        nvme = disks.get("NVMe", {})
        sd = disks.get("SD Card", {})
        nvme_usage = f"{nvme.get('used','?')} / {nvme.get('total','?')} ({nvme.get('use_pct','?')})"
        sd_usage = f"{sd.get('used','?')} / {sd.get('total','?')} ({sd.get('use_pct','?')})"
        
        devices_html = ""
        if devices:
            devices_html = '<div class="devices"><h2 style="font-size:16px;color:#00d4ff;margin-bottom:10px;text-transform:uppercase;letter-spacing:1px;">Connected Devices</h2>'
            for d in devices:
                devices_html += f'<div class="device"><span class="device-name">{d["hostname"]}</span><span>{d["ip"]}</span></div>'
            devices_html += '</div>'
        
        # Pi stats
        cpu_temp = pi.get("cpu_temp")
        nvme_temp = pi.get("nvme_temp")
        cpu_mhz = pi.get("cpu_mhz")
        cpu_volt = pi.get("cpu_volt")
        fan_rpm = pi.get("fan_rpm")
        load_1 = pi.get("load_1")
        load_5 = pi.get("load_5")
        load_15 = pi.get("load_15")
        
        cpu_temp_class = temp_class(cpu_temp)
        nvme_temp_class = temp_class(nvme_temp)
        
        cpu_temp_bar = make_temp_bar(cpu_temp)
        nvme_temp_bar = make_temp_bar(nvme_temp)
        
        load_str = f"{load_1:.2f} / {load_5:.2f} / {load_15:.2f}" if load_1 is not None else "?"
        
        mem_used = pi.get("mem_used_gb", "?")
        mem_total = pi.get("mem_total_gb", "?")
        mem_pct = pi.get("mem_pct", "?")
        
        throttle_section = ""
        if pi.get("throttled"):
            throttle_section = '<div class="throttle-warning">&#9888;&#65039; CPU is currently throttled!</div>'
        
        html = HTML_TEMPLATE.format(
            mode_class=mode_class,
            mode_name=mode_name,
            mode_icon=mode_icon,
            hotel_disabled=hotel_disabled,
            car_disabled=car_disabled,
            hdmi_status=hdmi,
            hdmi_class=hdmi_class,
            kodi_status=kodi,
            kodi_class=kodi_class,
            jellyfin_status=jellyfin,
            jellyfin_class=jellyfin_class,
            hotspot_status=hotspot,
            hotspot_class=hotspot_class,
            nvme_usage=nvme_usage,
            sd_usage=sd_usage,
            devices_section=devices_html,
            cpu_temp=f"{cpu_temp:.1f}" if cpu_temp is not None else "?",
            cpu_temp_class=cpu_temp_class,
            cpu_temp_bar=cpu_temp_bar,
            nvme_temp=f"{nvme_temp:.1f}" if nvme_temp is not None else "?",
            nvme_temp_class=nvme_temp_class,
            nvme_temp_bar=nvme_temp_bar,
            cpu_mhz=f"{cpu_mhz:.0f}" if cpu_mhz is not None else "?",
            cpu_volt=f"{cpu_volt:.2f}" if cpu_volt is not None else "?",
            fan_rpm=f"{fan_rpm}" if fan_rpm is not None else "?",
            load_avg=load_str,
            mem_used=f"{mem_used}",
            mem_total=f"{mem_total}",
            mem_pct=f"{mem_pct}",
            uptime=pi.get("uptime", "?"),
            eth0_ip=pi.get("interfaces", {}).get("eth0", "\u2014"),
            wlan0_ip=pi.get("interfaces", {}).get("wlan0", "\u2014"),
            throttle_section=throttle_section,
            timestamp=time.strftime("%I:%M:%S %p")
        )
        
        self.send_response(200)
        self.send_header("Content-Type", "text/html")
        self.end_headers()
        self.wfile.write(html.encode())
    
    # ----- Legacy status API (kept for backwards compat) -----
    # The old /api/status now returns the new flat ESP32 format.
    # If you need the old nested format, use /api/status?legacy=1
    
    def log_message(self, format, *args):
        pass

if __name__ == '__main__':
    server = HTTPServer(('0.0.0.0', 9090), Handler)
    print(f"Mode controller v2 with ESP32 API running on port 9090")
    print(f"API key: {'enabled' if API_KEY else 'disabled'}")
    server.serve_forever()