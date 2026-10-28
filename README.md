# Clone Hero PS4 (Native OpenOrbis Homebrew)

[![PS4 Pro](https://img.shields.io/badge/Tested%20On-PS4%20Pro%20%28FW%2013.52%29-003791?logo=playstation&logoColor=white)](https://github.com/rieuxmin/Clone-Hero-PS4)
[![Controller](https://img.shields.io/badge/Tested%20Controller-DualShock%204-blue)](https://github.com/rieuxmin/Clone-Hero-PS4)
[![Language](https://img.shields.io/badge/Language-C%2B%2B17%20%2F%20Orbis%20OS-00599C?logo=cplusplus&logoColor=white)](https://github.com/rieuxmin/Clone-Hero-PS4)
[![Performance](https://img.shields.io/badge/Performance-1080p60%20Locked-22c55e)](https://github.com/rieuxmin/Clone-Hero-PS4)
[![Audio](https://img.shields.io/badge/Audio-libopus%201.4%20%7C%20stb__vorbis%20%7C%20dr__libs-f59e0b)](https://github.com/rieuxmin/Clone-Hero-PS4)
[![License](https://img.shields.io/badge/License-GPL--3.0-blue.svg)](LICENSE)

Native PlayStation 4 homebrew port of the **Clone Hero / Guitar Hero** rhythm engine built from the ground up for jailbroken PS4 consoles.

Created & maintained by **[rieuxmin](https://github.com/rieuxmin)**.

---

> ### ⚠️ Testing & Hardware Notice
> * **Firmware & Console**: This build has been **tested exclusively on a PS4 Pro running 13.52 Firmware**.
> * **Controllers**: Author testing has been conducted **strictly with a standard DualShock 4 controller**. USB Guitar Hero / Rock Band controllers and custom adapters (Santroller, Ardwiino, Pi Pico) are implemented according to Orbis OS hardware specifications, but have not yet been physical-hardware tested by the author. **Community testing, reports, and pull requests are warmly encouraged!**

---

## 📑 Table of Contents
1. [Project Overview](#project-overview)
2. [Step-by-Step: Installing Songs via FTP](#step-by-step-installing-songs-via-ftp)
3. [Step-by-Step: Generating Background MP4 Videos](#step-by-step-generating-background-mp4-videos)
4. [Controller & USB Guitar Setup](#controller--usb-guitar-setup)
5. [Detailed File Breakdown](#detailed-file-breakdown)
6. [Bundled Highway Skins (21 Included)](#bundled-highway-skins-21-included)
7. [Architecture & Technical Highlights](#architecture--technical-highlights)
8. [Building from Source](#building-from-source)
9. [Publishing Releases to GitHub](#publishing-releases-to-github)
10. [License](#license)

---

## Project Overview

Clone Hero PS4 is a standalone native C++17 game engine that renders directly to the PS4 display hardware without emulators or heavy runtime wrappers. It delivers:
* **True 1080p 60 FPS Locked Output** with double-buffered hardware scanout via `libSceVideoOut`.
* **Zero-Stall Audio Playback**: Entire audio files are buffered in unified GDDR5 RAM, eliminating 5,400 RPM mechanical HDD and USB 2.0 latency skips.
* **Native Multi-Stem Audio**: Supports `.opus`, `.ogg`, `.mp3`, `.flac`, and `.wav` with real-time guitar stem muting on misses.
* **Authentic Guitar Hero 3 Mechanics**: Physical synthesis of tube-overdriven miss flubs, HOPO transitions, 4x combo multipliers, animated odometer, and star power lightning overdrive.
* **MP4 Video Backgrounds**: Play high-definition concert and music videos behind transparent highways.

---

## Step-by-Step: Installing Songs via FTP

Songs can be transferred wirelessly over your local home network using the PS4's built-in GoldHEN FTP server:

### 1. Enable FTP on your PS4
1. Turn on your PS4 and ensure GoldHEN is loaded.
2. Go to **Settings > GoldHEN > Server Settings**.
3. Enable **FTP Server** (default port is **`2121`**).
4. Note your PS4's Local IP address by checking **Settings > Network > View Connection Status** (e.g. `192.168.1.145`).

### 2. Prepare Song Folders
Every song folder on your PC must contain:
* `notes.chart` (or `notes.mid`)
* `song.ini` (song title, artist, charter, delay offset)
* Audio file: `song.opus`, `song.ogg`, `song.mp3`, or multi-stem files (`guitar.opus`, `drums.opus`, `song.opus`, etc.)
* *(Optional)* `album.png` (512×512 album cover art displayed in song select)
* *(Optional)* `video.mp4` (custom background concert video for this specific song)

### 3. Transfer the Songs

#### Method A: Using the Automated Script (Fastest)
From your Linux or WSL2 terminal, run:
```bash
./upload_songs_to_ps4.sh <PS4_IP_ADDRESS> /path/to/my_songs
