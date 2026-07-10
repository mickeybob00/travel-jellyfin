#!/usr/bin/env python3
"""
Travel Jellyfin Media Manager v2
Scans NAS for movies/shows, shows codec info, lets user pick what to transfer.
Compatible files are copied directly, incompatible ones are transcoded.
Runs on port 9091.

v2: Lazy codec scanning - lists everything instantly with file sizes,
    probes codecs on demand when user expands a title.
"""

import os
import sys
import json
import subprocess
import shutil
import threading
import time
import hashlib
import re
from http.server import HTTPServer, BaseHTTPRequestHandler
from urllib.parse import urlparse, parse_qs, unquote
from pathlib import Path

# Config - NAS mounted at /mnt/nas-media (<NAS_IP>:/path/to/your/media)
NAS_MOVIES_PATHS = [
    "/mnt/nas-media/media/movies",
    "/mnt/nas-media/torrents/movies",
]
NAS_TV_PATHS = [
    "/mnt/nas-media/media/tv shows",
    "/mnt/nas-media/torrents/tv shows",
]
LOCAL_MOVIES_NVME = "/mnt/media/Movies"
LOCAL_MOVIES_SD = "/srv/media/Movies"
LOCAL_TV_NVME = "/mnt/media/TV Shows"
LOCAL_TV_SD = "/srv/media/TV Shows"
FFPROBE = "/usr/lib/jellyfin-ffmpeg/ffprobe"
FFMPEG = "/usr/lib/jellyfin-ffmpeg/ffmpeg"
PORT = 9091

# Direct-play compatible: H.264 or HEVC video, AAC/AC3/EAC3/MP3/Opus audio, MKV/MP4 container, 1080p or below
COMPATIBLE_VIDEO = {"h264", "hevc", "mpeg4"}
COMPATIBLE_AUDIO = {"aac", "ac3", "eac3", "mp3", "opus"}
COMPATIBLE_CONTAINERS = {"mkv", "mp4", "m4v", "mov"}
MAX_RESOLUTION = 1920  # 1080p max for tablets

# Progress tracking
transfers = {}
transfer_lock = threading.Lock()

# Persist transfer state to disk so it survives restarts
TRANSFER_STATE_FILE = "/tmp/media-manager-transfers.json"

def save_transfers():
    try:
        with transfer_lock:
            with open(TRANSFER_STATE_FILE, "w") as f:
                json.dump(transfers, f)
    except:
        pass

def load_transfers():
    try:
        if os.path.exists(TRANSFER_STATE_FILE):
            with open(TRANSFER_STATE_FILE, "r") as f:
                loaded = json.load(f)
                with transfer_lock:
                    for tid, tdata in loaded.items():
                        # Mark any "running" transfers as "interrupted" on restart
                        if tdata.get("status") in ("starting", "copying", "transcoding", "queued"):
                            tdata["status"] = "interrupted"
                            tdata["message"] = "Interrupted by service restart - re-transfer to resume"
                        transfers[tid] = tdata
    except:
        pass

# Load any saved transfer state on startup
load_transfers()

# Cache for media listing (fast - just names and sizes, no ffprobe)
listing_cache = None
listing_cache_time = 0
listing_lock = threading.Lock()

# Codec probe cache: path -> info dict
codec_cache = {}
codec_cache_lock = threading.Lock()


def format_size(size_bytes):
    for unit in ["B", "KB", "MB", "GB", "TB"]:
        if size_bytes < 1024:
            return f"{size_bytes:.1f}{unit}"
        size_bytes /= 1024
    return f"{size_bytes:.1f}PB"


def get_dir_size(path):
    """Get total size of a directory"""
    total = 0
    for root, dirs, files in os.walk(path):
        for f in files:
            try:
                total += os.path.getsize(os.path.join(root, f))
            except:
                pass
    return total


def find_video_files(directory):
    """Find all video files in a directory (recursively for TV shows)"""
    video_exts = {".mkv", ".mp4", ".m4v", ".mov", ".avi", ".m2ts", ".iso", ".ts"}
    files = []
    for root, dirs, fnames in os.walk(directory):
        for f in fnames:
            if Path(f).suffix.lower() in video_exts:
                files.append(os.path.join(root, f))
    return files


def find_main_video_file(directory):
    """Find the main video file in a movie directory"""
    for ext in [".mkv", ".mp4", ".m4v", ".mov"]:
        files = list(Path(directory).glob(f"*{ext}"))
        if files:
            return str(files[0])
    for ext in [".m2ts", ".iso", ".ts", ".avi"]:
        files = list(Path(directory).glob(f"*{ext}"))
        if files:
            return str(files[0])
    vids = find_video_files(directory)
    return vids[0] if vids else None


def probe_codec(filepath):
    """Probe a single file for codec info (cached)"""
    with codec_cache_lock:
        if filepath in codec_cache:
            return codec_cache[filepath]
    
    try:
        video_cmd = [FFPROBE, "-v", "error", "-select_streams", "v:0",
                     "-show_entries", "stream=codec_name,profile,width,height",
                     "-of", "csv=p=0", filepath]
        video_result = subprocess.run(video_cmd, capture_output=True, text=True, timeout=10)
        vparts = video_result.stdout.strip().split(",")
        
        audio_cmd = [FFPROBE, "-v", "error", "-select_streams", "a:0",
                     "-show_entries", "stream=codec_name",
                     "-of", "csv=p=0", filepath]
        audio_result = subprocess.run(audio_cmd, capture_output=True, text=True, timeout=10)
        
        info = {
            "video_codec": vparts[0] if len(vparts) > 0 and vparts[0] else "unknown",
            "video_profile": vparts[1] if len(vparts) > 1 else "",
            "width": int(vparts[2]) if len(vparts) > 2 and vparts[2].isdigit() else 0,
            "height": int(vparts[3]) if len(vparts) > 3 and vparts[3].isdigit() else 0,
            "audio_codec": audio_result.stdout.strip() or "none",
            "container": Path(filepath).suffix.lstrip(".").lower()
        }
    except Exception as e:
        info = {
            "video_codec": "error", "video_profile": "",
            "width": 0, "height": 0,
            "audio_codec": "error", "container": Path(filepath).suffix.lstrip(".").lower()
        }
    
    with codec_cache_lock:
        codec_cache[filepath] = info
    return info


def check_compatibility(info):
    """Check if a video is direct-play compatible for Android tablets"""
    issues = []
    needs_downscale = False
    if info["video_codec"] not in COMPATIBLE_VIDEO:
        issues.append(f"Video: {info['video_codec']}")
    if info["width"] > MAX_RESOLUTION:
        needs_downscale = True
        issues.append(f"Res: {info['width']}p > 1080p")
    if info["audio_codec"] not in COMPATIBLE_AUDIO:
        issues.append(f"Audio: {info['audio_codec']}")
    if info["container"] not in COMPATIBLE_CONTAINERS:
        issues.append(f"Container: {info['container']}")
    # Compatible if codec+audio+container are OK (even if resolution needs downscaling)
    codec_ok = info["video_codec"] in COMPATIBLE_VIDEO
    audio_ok = info["audio_codec"] in COMPATIBLE_AUDIO
    container_ok = info["container"] in COMPATIBLE_CONTAINERS
    direct_play = codec_ok and audio_ok and container_ok and not needs_downscale
    return {"compatible": direct_play, "issues": issues, "needs_downscale": needs_downscale, "needs_transcode": len(issues) > 0}


def scan_listing():
    """Fast scan - just list all movies and TV shows with sizes, no ffprobe"""
    global listing_cache, listing_cache_time
    
    with listing_lock:
        if listing_cache and (time.time() - listing_cache_time) < 300:
            return listing_cache
        
        media = {"movies": [], "tv": []}
        seen_movie_names = set()
        
        # Scan movies
        for nas_path in NAS_MOVIES_PATHS:
            if not os.path.exists(nas_path):
                continue
            for item in sorted(os.listdir(nas_path), key=lambda x: re.sub(r'^(The|A|An)\s+', '', x, flags=re.IGNORECASE)):
                item_path = os.path.join(nas_path, item)
                if not os.path.isdir(item_path):
                    continue
                if item in seen_movie_names:
                    continue
                seen_movie_names.add(item)
                
                size = get_dir_size(item_path)
                on_nvme = os.path.exists(os.path.join(LOCAL_MOVIES_NVME, item))
                on_sd = os.path.exists(os.path.join(LOCAL_MOVIES_SD, item))
                
                media["movies"].append({
                    "name": item,
                    "path": item_path,
                    "size": size,
                    "size_str": format_size(size),
                    "on_nvme": on_nvme,
                    "on_sd": on_sd
                })
        
        # Scan TV shows
        seen_tv_names = set()
        for nas_path in NAS_TV_PATHS:
            if not os.path.exists(nas_path):
                continue
            for item in sorted(os.listdir(nas_path), key=lambda x: re.sub(r'^(The|A|An)\s+', '', x, flags=re.IGNORECASE)):
                item_path = os.path.join(nas_path, item)
                if not os.path.isdir(item_path):
                    continue
                if item in seen_tv_names:
                    continue
                seen_tv_names.add(item)
                
                # Count episodes
                episodes = find_video_files(item_path)
                size = sum(os.path.getsize(e) for e in episodes if os.path.exists(e))
                on_nvme = os.path.exists(os.path.join(LOCAL_TV_NVME, item))
                on_sd = os.path.exists(os.path.join(LOCAL_TV_SD, item))
                
                media["tv"].append({
                    "name": item,
                    "path": item_path,
                    "size": size,
                    "size_str": format_size(size),
                    "episode_count": len(episodes),
                    "episode_paths": episodes,
                    "on_nvme": on_nvme,
                    "on_sd": on_sd
                })
        
        listing_cache = media
        listing_cache_time = time.time()
        return media


def get_disk_space():
    result = {"nvme": {}, "sd": {}}
    for mount, key in [(LOCAL_MOVIES_NVME, "nvme"), (LOCAL_MOVIES_SD, "sd")]:
        try:
            r = subprocess.run(["df", "-B1", mount], capture_output=True, text=True, timeout=5)
            lines = r.stdout.strip().split("\n")
            if len(lines) > 1:
                parts = lines[1].split()
                result[key] = {
                    "total": int(parts[1]), "used": int(parts[2]), "avail": int(parts[3]),
                    "total_str": format_size(int(parts[1])),
                    "used_str": format_size(int(parts[2])),
                    "avail_str": format_size(int(parts[3]))
                }
        except:
            pass
    return result


def transfer_worker(transfer_id, source_path, dest_path, media_name, media_type, needs_transcode, dest_disk):
    """Worker thread for copying or transcoding media"""
    with transfer_lock:
        transfers[transfer_id]["status"] = "starting"
    
    if needs_transcode:
        # For movies: transcode the main file
        # For TV: transcode each episode
        if media_type == "movies":
            dest_file = os.path.join(dest_path, media_name + ".mp4")
            os.makedirs(dest_path, exist_ok=True)
            
            cmd = [FFMPEG, "-i", source_path,
                   "-c:v", "libx264", "-preset", "fast", "-crf", "23",
                   "-vf", "scale='min(1920,iw)':'min(1080,ih)':force_original_aspect_ratio=decrease",
                   "-c:a", "aac", "-b:a", "192k",
                   "-movflags", "+faststart",
                   "-y", "-stats", "-progress", "pipe:1", dest_file]
            
            with transfer_lock:
                transfers[transfer_id]["status"] = "transcoding"
                transfers[transfer_id]["message"] = "Transcoding to H.264 1080p..."
            
            try:
                dur_cmd = [FFPROBE, "-v", "error", "-show_entries", "format=duration",
                           "-of", "csv=p=0", source_path]
                dur_result = subprocess.run(dur_cmd, capture_output=True, text=True, timeout=10)
                duration = float(dur_result.stdout.strip()) if dur_result.stdout.strip() else 0
                
                proc = subprocess.Popen(cmd, stdout=subprocess.PIPE, stderr=subprocess.PIPE, text=True, bufsize=1)
                for line in proc.stdout:
                    if "out_time_ms=" in line:
                        try:
                            time_ms = int(line.split("=")[1].strip())
                            progress = (time_ms / 1000000 / duration * 100) if duration > 0 else 0
                            with transfer_lock:
                                transfers[transfer_id]["progress"] = min(progress, 100)
                        except:
                            pass
                proc.wait()
                if proc.returncode == 0:
                    with transfer_lock:
                        transfers[transfer_id]["status"] = "complete"
                        transfers[transfer_id]["progress"] = 100
                        transfers[transfer_id]["message"] = "Transcoded successfully"
                else:
                    with transfer_lock:
                        transfers[transfer_id]["status"] = "error"
                        transfers[transfer_id]["message"] = "Transcode failed"
            except Exception as e:
                with transfer_lock:
                    transfers[transfer_id]["status"] = "error"
                    transfers[transfer_id]["message"] = f"Error: {str(e)}"
        else:
            # TV show - transcode each episode, preserving folder structure
            dest_dir = os.path.join(dest_path, media_name)
            os.makedirs(dest_dir, exist_ok=True)
            episodes = find_video_files(source_path)
            total = len(episodes)
            skipped = 0
            
            for i, ep in enumerate(episodes):
                ep_name = Path(ep).name
                # Preserve relative subfolder structure (e.g. Series 1/, Series 2/)
                rel_path = os.path.relpath(ep, source_path)
                dest_subdir = os.path.join(dest_dir, os.path.dirname(rel_path))
                os.makedirs(dest_subdir, exist_ok=True)
                dest_file = os.path.join(dest_subdir, Path(ep).stem + ".mp4")
                
                # Skip if already transcoded (resume support)
                if os.path.exists(dest_file) and os.path.getsize(dest_file) > 1000:
                    skipped += 1
                    with transfer_lock:
                        transfers[transfer_id]["message"] = f"Skipping {i+1}/{total} (already done): {ep_name}"
                        transfers[transfer_id]["progress"] = ((i + 1) / total * 100)
                    continue
                
                with transfer_lock:
                    transfers[transfer_id]["message"] = f"Transcoding episode {i+1}/{total}: {ep_name}"
                    transfers[transfer_id]["progress"] = (i / total * 100)
                    transfers[transfer_id]["dest_size"] = get_dir_size(dest_dir)
                
                cmd = [FFMPEG, "-i", ep,
                       "-c:v", "libx264", "-preset", "fast", "-crf", "23",
                       "-vf", "scale='min(1920,iw)':'min(1080,ih)':force_original_aspect_ratio=decrease",
                       "-c:a", "aac", "-b:a", "192k",
                       "-movflags", "+faststart", "-y", dest_file]
                try:
                    subprocess.run(cmd, capture_output=True, timeout=3600)
                except:
                    pass
                
                # Save state periodically
                save_transfers()
            
            dest_total = get_dir_size(dest_dir)
            with transfer_lock:
                transfers[transfer_id]["status"] = "complete"
                transfers[transfer_id]["progress"] = 100
                transfers[transfer_id]["dest_size"] = dest_total
                src_str = format_size(transfers[transfer_id].get("source_size", 0))
                dst_str = format_size(dest_total)
                transfers[transfer_id]["message"] = f"Transcoded {total - skipped} episodes ({skipped} skipped) - {src_str} -> {dst_str}"
            save_transfers()
    else:
        # Direct copy
        with transfer_lock:
            transfers[transfer_id]["status"] = "copying"
            transfers[transfer_id]["message"] = "Copying..."
        
        try:
            dest_dir = os.path.join(dest_path, media_name)
            os.makedirs(dest_dir, exist_ok=True)
            
            total_size = get_dir_size(source_path)
            with transfer_lock:
                transfers[transfer_id]["total"] = total_size
            
            copied = 0
            skipped = 0
            for root, dirs, files in os.walk(source_path):
                rel_path = os.path.relpath(root, source_path)
                dest_subdir = os.path.join(dest_dir, rel_path) if rel_path != "." else dest_dir
                os.makedirs(dest_subdir, exist_ok=True)
                for f in files:
                    src_file = os.path.join(root, f)
                    dst_file = os.path.join(dest_subdir, f)
                    # Skip if already exists with same size (resume support)
                    if os.path.exists(dst_file):
                        try:
                            if os.path.getsize(dst_file) == os.path.getsize(src_file):
                                skipped += 1
                                copied += os.path.getsize(src_file)
                                progress = (copied / total_size * 100) if total_size > 0 else 100
                                with transfer_lock:
                                    transfers[transfer_id]["progress"] = min(progress, 100)
                                    transfers[transfer_id]["copied"] = copied
                                continue
                        except:
                            pass
                    shutil.copy2(src_file, dst_file)
                    copied += os.path.getsize(src_file)
                    progress = (copied / total_size * 100) if total_size > 0 else 100
                    with transfer_lock:
                        transfers[transfer_id]["progress"] = min(progress, 100)
                        transfers[transfer_id]["copied"] = copied
            
            dest_total = get_dir_size(dest_dir)
            with transfer_lock:
                transfers[transfer_id]["status"] = "complete"
                transfers[transfer_id]["progress"] = 100
                transfers[transfer_id]["dest_size"] = dest_total
                transfers[transfer_id]["message"] = f"Copied ({skipped} skipped) - {format_size(total_size)} -> {format_size(dest_total)}"
            save_transfers()
        except Exception as e:
            with transfer_lock:
                transfers[transfer_id]["status"] = "error"
                transfers[transfer_id]["message"] = f"Error: {str(e)}"
            save_transfers()


HTML_PAGE = """<!DOCTYPE html>
<html>
<head>
<meta charset="utf-8">
<meta name="viewport" content="width=device-width, initial-scale=1">
<title>Media Manager - Travel Jellyfin</title>
<style>
* { box-sizing: border-box; margin: 0; padding: 0; }
body { font-family: -apple-system, BlinkMacSystemFont, 'Segoe UI', Roboto, sans-serif;
       background: linear-gradient(135deg, #1a1a2e, #16213e); color: #eee; min-height: 100vh; padding: 15px; }
.header { text-align: center; margin-bottom: 20px; }
.header h1 { font-size: 24px; color: #00d4ff; margin-bottom: 5px; }
.header p { color: #888; font-size: 13px; }
a.back { color: #00d4ff; text-decoration: none; font-size: 13px; }
.tabs { display: flex; gap: 10px; justify-content: center; margin-bottom: 15px; }
.tab { padding: 10px 25px; border-radius: 8px; border: none; cursor: pointer;
       font-size: 15px; font-weight: bold; background: rgba(255,255,255,0.1); color: #aaa; }
.tab.active { background: #00d4ff; color: #1a1a2e; }
.disk-bar { display: flex; gap: 15px; justify-content: center; margin-bottom: 15px; }
.disk-card { background: rgba(255,255,255,0.08); padding: 10px 20px; border-radius: 8px; text-align: center; min-width: 200px; }
.disk-card h3 { font-size: 13px; color: #00d4ff; margin-bottom: 5px; }
.disk-bar-fill { background: rgba(255,255,255,0.1); border-radius: 4px; height: 8px; overflow: hidden; margin: 5px 0; }
.disk-bar-used { background: #00d4ff; height: 100%; }
.disk-info { font-size: 12px; color: #888; }
.controls { display: flex; gap: 10px; justify-content: center; margin-bottom: 15px; flex-wrap: wrap; align-items: center; }
.controls button { padding: 10px 20px; border-radius: 8px; border: none; cursor: pointer; font-size: 14px; font-weight: bold; }
.btn-scan { background: #3498db; color: white; }
.btn-transfer { background: #2ecc71; color: white; }
.btn-transfer:disabled { opacity: 0.4; }
.select-area { display: flex; gap: 10px; align-items: center; }
.select-area label { font-size: 13px; color: #888; }
select { padding: 5px 10px; border-radius: 5px; background: #2a2a4e; color: #fff; border: 1px solid #444; }
.media-list { max-width: 900px; margin: 0 auto; }
.media-item { background: rgba(255,255,255,0.06); border-radius: 10px; padding: 12px 15px;
              margin-bottom: 8px; display: flex; align-items: center; gap: 12px; }
.media-item input[type=checkbox] { width: 20px; height: 20px; cursor: pointer; flex-shrink: 0; }
.media-info { flex: 1; cursor: pointer; }
.media-name { font-size: 14px; font-weight: 500; }
.media-meta { font-size: 12px; color: #888; margin-top: 3px; }
.media-detail { font-size: 12px; color: #666; margin-top: 5px; padding: 8px; background: rgba(0,0,0,0.2); border-radius: 5px; display: none; }
.badge { padding: 3px 8px; border-radius: 5px; font-size: 11px; font-weight: bold; white-space: nowrap; }
.badge-ok { background: #2ecc71; color: #fff; }
.badge-transcode { background: #f39c12; color: #fff; }
.badge-on-pi { background: #9b59b6; color: #fff; }
.badge-loading { background: #555; color: #aaa; }
.media-size { text-align: right; font-size: 13px; color: #aaa; white-space: nowrap; }
.transfers { max-width: 900px; margin: 15px auto; }
.transfer-item { background: rgba(255,255,255,0.06); border-radius: 10px; padding: 12px 15px; margin-bottom: 8px; }
.transfer-header { display: flex; justify-content: space-between; align-items: center; }
.transfer-name { font-size: 14px; font-weight: 500; }
.transfer-status { font-size: 12px; padding: 3px 8px; border-radius: 5px; }
.transfer-progress-bar { background: rgba(255,255,255,0.1); border-radius: 4px; height: 12px; overflow: hidden; margin: 8px 0; }
.transfer-progress-fill { background: linear-gradient(90deg, #00d4ff, #2ecc71); height: 100%; transition: width 0.3s; }
.transfer-detail { font-size: 12px; color: #888; }
.filter-bar { display: flex; gap: 8px; justify-content: center; margin-bottom: 10px; flex-wrap: wrap; }
.filter-bar input { padding: 8px 12px; border-radius: 8px; background: rgba(255,255,255,0.1); color: #fff; border: 1px solid #444; font-size: 13px; width: 200px; }
select.filter-select { padding: 8px 12px; border-radius: 8px; background: rgba(255,255,255,0.1); color: #fff; border: 1px solid #444; font-size: 13px; }
.empty { text-align: center; color: #666; padding: 40px; font-size: 14px; }
.spinner { display: inline-block; width: 16px; height: 16px; border: 2px solid rgba(255,255,255,0.3); border-top: 2px solid #00d4ff; border-radius: 50%; animation: spin 1s linear infinite; }
@keyframes spin { to { transform: rotate(360deg); } }
.summary { text-align: center; margin-bottom: 15px; font-size: 13px; color: #888; }
.summary b { color: #00d4ff; }
</style>
</head>
<body>
<div class="header">
  <h1>📁 Media Manager</h1>
  <p>Pick movies & shows to transfer to the Pi</p>
  <p><a class="back" href="http://10.42.0.1:9090">← Back to Mode Controller</a></p>
</div>

<div class="tabs">
  <button class="tab active" onclick="switchTab('movies')">🎬 Movies</button>
  <button class="tab" onclick="switchTab('tv')">📺 TV Shows</button>
  <button class="tab" onclick="switchTab('transfers')">📤 Transfers</button>
</div>

<div class="disk-bar" id="diskBar"></div>
<div class="summary" id="summary"></div>

<div class="controls" id="controls">
  <button class="btn-scan" onclick="rescan()">🔄 Rescan NAS</button>
  <div class="select-area">
    <label>Destination:</label>
    <select id="destDisk">
      <option value="nvme">NVMe (/mnt/media)</option>
      <option value="sd">SD Card (/srv/media)</option>
    </select>
  </div>
  <button class="btn-transfer" id="transferBtn" onclick="startTransfer()">📤 Transfer Selected</button>
</div>

<div class="filter-bar" id="filterBar">
  <input type="text" id="searchBox" placeholder="Search..." oninput="filterMedia()">
  <select class="filter-select" id="filterCompat" onchange="filterMedia()">
    <option value="">All</option>
    <option value="compatible">Direct Play Ready</option>
    <option value="transcode">Needs Transcode</option>
    <option value="onpi">Already on Pi</option>
  </select>
</div>

<div class="media-list" id="mediaList"></div>
<div class="transfers" id="transfersList" style="display:none;"></div>

<script>
let currentTab = 'movies';
let mediaData = null;
let selectedItems = new Set();
let codecData = {};  // cache: name -> {info, compat}

async function loadData() {
  document.getElementById('mediaList').innerHTML = '<div class="empty"><span class="spinner"></span> Scanning NAS...</div>';
  const res = await fetch('/api/scan');
  mediaData = await res.json();
  renderDiskBar(mediaData.disk);
  renderSummary();
  renderMedia();
}

function renderSummary() {
  const movies = mediaData.movies || [];
  const tv = mediaData.tv || [];
  const totalMovieSize = movies.reduce((a, m) => a + m.size, 0);
  const totalTvSize = tv.reduce((a, t) => a + t.size, 0);
  document.getElementById('summary').innerHTML = 
    `<b>${movies.length}</b> movies (${formatSize(totalMovieSize)}) · <b>${tv.length}</b> TV shows · <b>${tv.reduce((a,t) => a+t.episode_count, 0)}</b> episodes (${formatSize(totalTvSize)})`;
}

function formatSize(b) {
  const u = ['B','KB','MB','GB','TB'];
  for (let i of u) { if (b < 1024) return b.toFixed(1)+i; b /= 1024; }
  return b.toFixed(1)+'PB';
}

function renderDiskBar(disk) {
  const bar = document.getElementById('diskBar');
  if (!disk) return;
  let html = '';
  for (const [key, d] of Object.entries(disk)) {
    const pct = d.total > 0 ? (d.used / d.total * 100).toFixed(0) : 0;
    html += `<div class="disk-card"><h3>${key.toUpperCase()}</h3>
      <div class="disk-bar-fill"><div class="disk-bar-used" style="width:${pct}%"></div></div>
      <div class="disk-info">${d.used_str} / ${d.total_str} (${pct}%)</div>
      <div class="disk-info"><b>${d.avail_str} free</b></div></div>`;
  }
  bar.innerHTML = html;
}

function renderMedia() {
  const list = document.getElementById('mediaList');
  
  if (currentTab === 'transfers') {
    list.innerHTML = '';
    loadTransfers();
    return;
  }
  
  const search = document.getElementById('searchBox').value.toLowerCase();
  const filter = document.getElementById('filterCompat').value;
  const items = currentTab === 'movies' ? (mediaData.movies || []) : (mediaData.tv || []);
  let html = '';
  let count = 0;
  
  for (const item of items) {
    if (search && !item.name.toLowerCase().includes(search)) continue;
    const onPi = item.on_nvme || item.on_sd;
    if (filter === 'onpi' && !onPi) continue;
    
    // Check if we have codec data
    const codec = codecData[item.name];
    let badgeClass = 'badge-loading';
    let badgeText = '⚡ Probe';
    let meta = '';
    
    if (codec) {
      if (codec.compat.compatible) { badgeClass = 'badge-ok'; badgeText = '✓ Direct Play'; }
      else { badgeClass = 'badge-transcode'; badgeText = '⚠ Transcode'; }
      if (currentTab === 'movies') {
        const i = codec.info;
        meta = `${i.video_codec} · ${i.width}x${i.height} · ${i.audio_codec} · ${i.container}`;
        if (codec.compat.issues.length) meta += ` · ${codec.compat.issues.join(', ')}`;
      } else {
        meta = codec.compat.compatible ? 'All episodes compatible' : 'Some episodes need transcode';
      }
    } else if (currentTab === 'tv') {
      meta = `${item.episode_count} episodes`;
    }
    
    if (onPi) { badgeClass = 'badge-on-pi'; badgeText = 'On Pi'; }
    if (filter === 'compatible' && (!codec || !codec.compat.compatible)) continue;
    if (filter === 'transcode' && (!codec || codec.compat.compatible)) continue;
    
    const id = currentTab + ':' + item.name;
    const checked = selectedItems.has(id) ? 'checked' : '';
    
    html += `<div class="media-item">
      <input type="checkbox" id="sel_${count}" ${checked} onchange="toggleSelect('${id}', this.checked)" ${onPi ? 'disabled' : ''}>
      <div class="media-info" onclick="probeCodec('${item.name}', '${currentTab}')">
        <div class="media-name">${item.name}</div>
        <div class="media-meta" id="meta_${count}">${meta}</div>
      </div>
      <span class="badge ${badgeClass}" id="badge_${count}">${badgeText}</span>
      <div class="media-size">${item.size_str}</div>
    </div>`;
    count++;
  }
  
  if (count === 0) html = '<div class="empty">No media found matching filters</div>';
  
  list.innerHTML = html;
  document.getElementById('transfersList').style.display = 'none';
  document.getElementById('filterBar').style.display = currentTab === 'transfers' ? 'none' : 'flex';
  document.getElementById('controls').style.display = currentTab === 'transfers' ? 'none' : 'flex';
}

async function probeCodec(name, type) {
  if (codecData[name]) return; // already probed
  const items = type === 'movies' ? mediaData.movies : mediaData.tv;
  const item = items.find(m => m.name === name);
  if (!item) return;
  
  // Show loading
  const idx = items.indexOf(item);
  const metaEl = document.getElementById('meta_' + idx);
  if (metaEl) metaEl.innerHTML = '<span class="spinner"></span> Probing...';
  
  const res = await fetch('/api/probe', {
    method: 'POST',
    headers: {'Content-Type': 'application/json'},
    body: JSON.stringify({path: item.path, type: type})
  });
  const data = await res.json();
  codecData[name] = data;
  renderMedia();
}

function toggleSelect(id, checked) {
  if (checked) selectedItems.add(id);
  else selectedItems.delete(id);
}

function filterMedia() { renderMedia(); }

function switchTab(tab) {
  currentTab = tab;
  document.querySelectorAll('.tab').forEach(t => t.classList.remove('active'));
  event.target.classList.add('active');
  renderMedia();
}

async function rescan() {
  codecData = {};
  document.getElementById('mediaList').innerHTML = '<div class="empty"><span class="spinner"></span> Scanning NAS...</div>';
  await fetch('/api/scan?rescan=1');
  await loadData();
}

async function startTransfer() {
  if (selectedItems.size === 0) { alert('Select at least one item'); return; }
  const dest = document.getElementById('destDisk').value;
  const items = Array.from(selectedItems);
  
  const res = await fetch('/api/transfer', {
    method: 'POST',
    headers: {'Content-Type': 'application/json'},
    body: JSON.stringify({items: items, destination: dest})
  });
  const result = await res.json();
  
  if (result.started > 0) {
    alert(`Started ${result.started} transfers. Check the Transfers tab.`);
    selectedItems.clear();
    document.querySelectorAll('.tab').forEach(t => t.classList.remove('active'));
    document.querySelectorAll('.tab')[2].classList.add('active');
    currentTab = 'transfers';
    renderMedia();
  }
}

async function loadTransfers() {
  const res = await fetch('/api/transfers');
  const data = await res.json();
  const list = document.getElementById('transfersList');
  list.style.display = 'block';
  let html = '';
  for (const t of data.transfers) {
    const pct = t.progress || 0;
    let statusClass = t.status === 'complete' ? 'badge-ok' : t.status === 'error' ? 'badge-transcode' : 'badge-loading';
    let sizeInfo = '';
    if (t.source_size > 0) {
      const srcStr = formatSize(t.source_size);
      const dstStr = formatSize(t.dest_size || 0);
      const ratio = t.source_size > 0 ? Math.round((1 - (t.dest_size || 0) / t.source_size) * 100) : 0;
      if (t.status === 'complete' && t.dest_size > 0)
        sizeInfo = ` · ${srcStr} → ${dstStr} (${ratio}% smaller)`;
      else if (t.dest_size > 0)
        sizeInfo = ` · ${srcStr} → ${dstStr} so far`;
      else
        sizeInfo = ` · Source: ${srcStr}`;
    }
    html += `<div class="transfer-item">
      <div class="transfer-header"><span class="transfer-name">${t.name}</span>
      <span class="transfer-status ${statusClass}">${t.status}</span></div>
      <div class="transfer-progress-bar"><div class="transfer-progress-fill" style="width:${pct}%"></div></div>
      <div class="transfer-detail">${t.message || ''} ${t.speed ? '· '+t.speed : ''} ${t.progress ? '· '+t.progress.toFixed(1)+'%' : ''}${sizeInfo}</div></div>`;
  }
  if (!html) html = '<div class="empty">No transfers yet</div>';
  list.innerHTML = html;
}

setInterval(() => { if (currentTab === 'transfers') loadTransfers(); }, 2000);
loadData();
</script>
</body>
</html>"""


class Handler(BaseHTTPRequestHandler):
    def do_GET(self):
        parsed = urlparse(self.path)
        
        if parsed.path == '/':
            self.send_response(200)
            self.send_header('Content-Type', 'text/html')
            self.end_headers()
            self.wfile.write(HTML_PAGE.encode())
        elif parsed.path == '/api/scan':
            params = parse_qs(parsed.query)
            if params.get('rescan'):
                global listing_cache, listing_cache_time
                listing_cache = None
                listing_cache_time = 0
            media = scan_listing()
            disk = get_disk_space()
            # Strip episode_paths (too large for JSON)
            movies_out = [{k: v for k, v in m.items() if k != "episode_paths"} for m in media["movies"]]
            tv_out = []
            for t in media["tv"]:
                tv_out.append({
                    "name": t["name"], "path": t["path"], "size": t["size"],
                    "size_str": t["size_str"], "episode_count": t["episode_count"],
                    "on_nvme": t["on_nvme"], "on_sd": t["on_sd"]
                })
            self.send_response(200)
            self.send_header('Content-Type', 'application/json')
            self.end_headers()
            self.wfile.write(json.dumps({"movies": movies_out, "tv": tv_out, "disk": disk}).encode())
        elif parsed.path == '/api/transfers':
            with transfer_lock:
                data = list(transfers.values())
            self.send_response(200)
            self.send_header('Content-Type', 'application/json')
            self.end_headers()
            self.wfile.write(json.dumps({"transfers": data}, default=str).encode())
        elif parsed.path == '/api/disk':
            disk = get_disk_space()
            self.send_response(200)
            self.send_header('Content-Type', 'application/json')
            self.end_headers()
            self.wfile.write(json.dumps(disk).encode())
        else:
            self.send_error(404)
    
    def do_POST(self):
        parsed = urlparse(self.path)
        content_length = int(self.headers['Content-Length'])
        
        if parsed.path == '/api/probe':
            body = json.loads(self.rfile.read(content_length))
            path = body.get('path', '')
            media_type = body.get('type', 'movies')
            
            if media_type == 'movies':
                video_file = find_main_video_file(path)
                if video_file:
                    info = probe_codec(video_file)
                    compat = check_compatibility(info)
                else:
                    info = {"video_codec": "none", "width": 0, "height": 0, "audio_codec": "none", "container": "none"}
                    compat = {"compatible": False, "issues": ["No video file found"]}
            else:
                # TV show - probe first episode
                episodes = find_video_files(path)
                if episodes:
                    info = probe_codec(episodes[0])
                    compat = check_compatibility(info)
                else:
                    info = {"video_codec": "none", "width": 0, "height": 0, "audio_codec": "none", "container": "none"}
                    compat = {"compatible": False, "issues": ["No episodes found"]}
            
            self.send_response(200)
            self.send_header('Content-Type', 'application/json')
            self.end_headers()
            self.wfile.write(json.dumps({"info": info, "compat": compat}).encode())
        
        elif parsed.path == '/api/transfer':
            body = json.loads(self.rfile.read(content_length))
            items = body.get('items', [])
            dest = body.get('destination', 'nvme')
            started = 0
            
            for item_id in items:
                media_type, name = item_id.split(':', 1)
                media = scan_listing()
                source_list = media.get(media_type, [])
                source_item = next((m for m in source_list if m['name'] == name), None)
                if not source_item:
                    continue
                
                if media_type == 'movies':
                    dest_path = LOCAL_MOVIES_NVME if dest == 'nvme' else LOCAL_MOVIES_SD
                    video_file = find_main_video_file(source_item['path'])
                    # Check compatibility
                    if video_file:
                        info = probe_codec(video_file)
                        compat = check_compatibility(info)
                        needs_transcode = compat.get('needs_transcode', False)
                    else:
                        needs_transcode = True
                    source_for_transfer = video_file or source_item['path']
                else:
                    dest_path = LOCAL_TV_NVME if dest == 'nvme' else LOCAL_TV_SD
                    # Check first episode
                    episodes = find_video_files(source_item['path'])
                    needs_transcode = False
                    if episodes:
                        info = probe_codec(episodes[0])
                        compat = check_compatibility(info)
                        needs_transcode = compat.get('needs_transcode', False)
                    source_for_transfer = source_item['path']
                
                transfer_id = hashlib.md5(f"{item_id}_{time.time()}".encode()).hexdigest()[:8]
                with transfer_lock:
                    source_size = get_dir_size(source_for_transfer) if media_type == "tv" else (os.path.getsize(source_for_transfer) if source_for_transfer and os.path.isfile(source_for_transfer) else 0)
                    transfers[transfer_id] = {
                        "id": transfer_id, "name": name, "type": media_type,
                        "status": "queued", "progress": 0, "message": "Queued...",
                        "destination": dest, "speed": "", "total": 0, "copied": 0,
                        "source_size": source_size, "dest_size": 0
                    }
                
                save_transfers()
                t = threading.Thread(target=transfer_worker, args=(
                    transfer_id, source_for_transfer, dest_path, name,
                    media_type, needs_transcode, dest
                ))
                t.daemon = True
                t.start()
                started += 1
            
            self.send_response(200)
            self.send_header('Content-Type', 'application/json')
            self.end_headers()
            self.wfile.write(json.dumps({"started": started}).encode())
        else:
            self.send_error(404)
    
    def log_message(self, format, *args):
        pass


if __name__ == '__main__':
    print(f"Media Manager v2 running on port {PORT}")
    server = HTTPServer(('0.0.0.0', PORT), Handler)
    server.serve_forever()