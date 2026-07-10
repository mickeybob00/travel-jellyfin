# 3D Models — Custom Housing

This directory contains 3D model files for the custom housing designed to hold
the Travel Jellyfin system components.

## Components to House

| Component | Dimensions (mm) | Notes |
|-----------|-----------------|-------|
| Raspberry Pi 5 (case) | 86 × 56 × 20 | With active cooler |
| NVMe SSD enclosure (USB) | Varies | External USB-NVMe adapter |
| ESP32-2432S028R (CYD) | 85 × 55 × 15 | With touchscreen |
| USB-C / power cables | — | Cable management channels |
| HDMI cable (short) | — | For hotel mode TV connection |

## Design Goals

- **Portable:** Compact, rugged enough for car travel
- **Ventilated:** Pi 5 runs hot during transcoding — airflow is critical
- **Front panel:** ESP32 touchscreen visible/accessible on the front
- **Cable management:** Short internal cables, external ports accessible
- **Mountable:** Optional VESA or DIN rail mount for permanent install

## Files

| File | Description | Status |
|------|-------------|--------|
| `housing-main-body.stl` | Main enclosure body | 📝 Design phase |
| `housing-front-panel.stl` | Front panel with CYD cutout | 📝 Design phase |
| `housing-lid.stl` | Top/bottom lid with ventilation | 📝 Design phase |
| `pi5-mount.stl` | Pi 5 mounting bracket | 📝 Design phase |
| `esp32-mount.stl` | ESP32 CYD bezel/mount | 📝 Design phase |
| `cable-clip.stl` | Internal cable management clips | 📝 Design phase |
| `housing-full-assembly.step` | Full assembly (CAD reference) | 📝 Design phase |

> 📝 = Design phase — files are placeholders. Replace with your actual models.

## Recommended Print Settings

- **Material:** PETG or ABS (heat resistance — Pi 5 can hit 80°C)
- **Layer height:** 0.2mm
- **Infill:** 25-30%
- **Perimeters:** 3-4 (strength for travel)
- **Supports:** Yes (for overhangs in housing body)

## Software

Designed in [FreeCAD](https://www.freecadweb.org/) / [Fusion 360] / [Onshape].
STEP files provided for editing in any CAD software.