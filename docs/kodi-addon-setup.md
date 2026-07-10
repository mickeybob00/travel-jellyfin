# Kodi Addon Setup — Jellyfin for Kodi

The Jellyfin for Kodi addon is not available in Debian repos or as a pre-built zip.
It must be built from source.

## Prerequisites

```bash
sudo apt-get install -y python3-yaml git
```

## Build the Addon

```bash
cd /tmp
git clone --depth 1 --branch v2.1.0 https://github.com/jellyfin/jellyfin-kodi.git
cd jellyfin-kodi
python3 build.py --version py3
# Output: plugin.video.jellyfin+py3.zip
```

## Install Addon + Dependencies

```bash
cd ~/.kodi/addons
unzip /tmp/jellyfin-kodi/plugin.video.jellyfin+py3.zip
```

### Install Python dependencies (as Kodi addon modules)

Download from `https://mirrors.kodi.tv/addons/nexus/`:

```bash
cd ~/.kodi/addons

# script.module.requests
wget https://mirrors.kodi.tv/addons/nexus/script.module.requests/script.module.requests-2.31.0.zip
unzip script.module.requests-2.31.0.zip

# script.module.dateutil
wget https://mirrors.kodi.tv/addons/nexus/script.module.dateutil/script.module.dateutil-2.8.1+matrix.1.zip
unzip script.module.dateutil-2.8.1+matrix.1.zip

# script.module.addon.signals
wget https://mirrors.kodi.tv/addons/nexus/script.module.addon.signals/script.module.addon.signals-0.0.6+matrix.1.zip
unzip script.module.addon.signals-0.0.6+matrix.1.zip

# script.module.websocket
wget https://mirrors.kodi.tv/addons/nexus/script.module.websocket/script.module.websocket-1.6.4.zip
unzip script.module.websocket-1.6.4.zip
```

### Rebuild addon registry

```bash
# Concatenate all addon.xml files into addons.xml
echo '<?xml version="1.0" encoding="UTF-8"?>' > ~/.kodi/userdata/addons.xml
echo '<addons>' >> ~/.kodi/userdata/addons.xml
for d in ~/.kodi/addons/*/; do
  if [ -f "$d/addon.xml" ]; then
    cat "$d/addon.xml" >> ~/.kodi/userdata/addons.xml
  fi
done
echo '</addons>' >> ~/.kodi/userdata/addons.xml
```

## Pre-configure Addon Settings

Create `~/.kodi/userdata/addon_data/plugin.video.jellyfin/settings.xml`:

```xml
<?xml version='1.0' encoding='utf-8'?>
<settings>
  <setting id="server_address" value="10.42.0.1" />
  <setting id="server_port" value="8096" />
  <setting id="https" value="false" />
</settings>
```

Use the hotspot IP (10.42.0.1), not the ethernet IP, so it works in car mode too.

## Fix Permissions

```bash
chown -R <your_user>:<your_group> ~/.kodi/
```

## Pitfalls

- `build.py` requires `pyyaml` — `pip3 install pyyaml` or `apt-get install python3-yaml`
- Some mirror URLs 404 — check directory listings at `https://mirrors.kodi.tv/addons/nexus/<module>/`
- Kodi's Python environment is separate from system Python — do NOT pip install dependencies
- The addon has no pre-built release zips — must always build from source
- Use the hotspot IP (10.42.0.1) in addon settings, not the ethernet IP