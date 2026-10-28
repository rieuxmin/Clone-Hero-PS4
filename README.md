# Clone Hero PS4 (Native OpenOrbis Homebrew)

[![PS4 Pro](https://img.shields.io/badge/Tested%20On-PS4%20Pro%20%28FW%2013.52%29-003791?logo=playstation&logoColor=white)](https://github.com/rieuxmin/Clone-Hero-PS4)
[![Controller](https://img.shields.io/badge/Tested%20Controller-DualShock%204-blue)](https://github.com/rieuxmin/Clone-Hero-PS4)
[![Language](https://img.shields.io/badge/Language-C%2B%2B17%20%2F%20Orbis%20OS-00599C?logo=cplusplus&logoColor=white)](https://github.com/rieuxmin/Clone-Hero-PS4)
[![Performance](https://img.shields.io/badge/Performance-1080p60%20Locked-22c55e)](https://github.com/rieuxmin/Clone-Hero-PS4)
[![Audio](https://img.shields.io/badge/Audio-libopus%201.4%20%7C%20stb__vorbis%20%7C%20dr__libs-f59e0b)](https://github.com/rieuxmin/Clone-Hero-PS4)
[![License](https://img.shields.io/badge/License-GPL--3.0-blue.svg)](LICENSE)

Native PlayStation 4 homebrew port of the **Clone Hero / Guitar Hero** rhythm engine built from the ground up for jailbroken PS4 consoles.

Created and maintained by **[rieuxmin](https://github.com/rieuxmin)**.

---

## Table of Contents
1. [Hardware and Testing Notice](#hardware-and-testing-notice)
2. [Project Overview](#project-overview)
3. [Installing Songs via FTP](#installing-songs-via-ftp)
4. [Generating Background MP4 Videos](#generating-background-mp4-videos)
5. [Controller and USB Guitar Setup](#controller-and-usb-guitar-setup)
6. [Detailed File Breakdown](#detailed-file-breakdown)
7. [Bundled Highway Skins](#bundled-highway-skins)
8. [Architecture and Technical Highlights](#architecture-and-technical-highlights)
9. [Building from Source](#building-from-source)
10. [Publishing Releases to GitHub](#publishing-releases-to-github)
11. [License](#license)

---

## Hardware and Testing Notice

* **Tested Firmware and Console**: This engine has been **tested exclusively on a PS4 Pro running 13.52 Firmware**.
* **Tested Controller**: Author testing has been conducted **strictly with a standard DualShock 4 controller**.
* **USB Guitars and Adapters**: USB Guitar Hero / Rock Band controllers and custom adapters (Santroller, Ardwiino, Pi Pico) are implemented according to Orbis OS hardware input specifications, but have not yet been physical-hardware tested by the author. Community feedback, compatibility reports, and pull requests are warmly encouraged!

---

## Project Overview

Clone Hero PS4 is a standalone native C++17 game engine that renders directly to the PS4 display hardware without emulators or heavy runtime wrappers. It delivers:

* **True 1080p 60 FPS Locked Output** with double-buffered hardware scanout via `libSceVideoOut`.
* **Zero-Stall Audio Playback**: Entire audio files are buffered in unified GDDR5 RAM, eliminating 5,400 RPM mechanical HDD and USB 2.0 latency skips.
* **Native Multi-Stem Audio**: Supports `.opus`, `.ogg`, `.mp3`, `.flac`, and `.wav` with real-time guitar stem muting on misses.
* **Authentic Guitar Hero 3 Mechanics**: Physical synthesis of tube-overdriven miss flubs, HOPO transitions, 4x combo multipliers, animated odometer, and star power lightning overdrive.
* **MP4 Video Backgrounds**: Play high-definition concert and music videos behind transparent highways.

---

## Installing Songs via FTP

Songs can be transferred wirelessly over your local home network using the PS4's built-in GoldHEN FTP server:

### 1. Enable FTP on your PS4
1. Turn on your PS4 and ensure GoldHEN is loaded.
2. Go to **Settings > GoldHEN > Server Settings**.
3. Enable **FTP Server** (default port is `2121`).
4. Note your PS4's Local IP address by checking **Settings > Network > View Connection Status** (e.g. `192.168.1.145`).

### 2. Prepare Song Folders
Every song folder on your computer must contain:
* `notes.chart` (or `notes.mid`)
* `song.ini` (song title, artist, charter, delay offset)
* Audio file: `song.opus`, `song.ogg`, `song.mp3`, or multi-stem files (`guitar.opus`, `drums.opus`, `song.opus`, etc.)
* *(Optional)* `album.png` (512x512 album cover art displayed in song select)
* *(Optional)* `video.mp4` (custom background concert video for this specific song)

### 3. Transfer the Songs

#### Method A: Using the Automated Script
From your Linux or WSL2 terminal, run:
```bash
./upload_songs_to_ps4.sh 192.168.1.145 /path/to/my_songs
```

#### Method B: Using FileZilla or WinSCP (GUI)
1. Open FileZilla or WinSCP.
2. **Host**: Enter your PS4 IP (e.g. `192.168.1.145`).
3. **Port**: `2121`.
4. **Logon Type**: `Anonymous` (no password needed).
5. Navigate on the PS4 remote side to: `/data/GH/` (create the folder if it does not exist).
6. Drag and drop your song folders into `/data/GH/`.

### 4. Scan in Game
Launch Clone Hero on your PS4. On the Tracklist screen, press **TRIANGLE** to rescan the library. Your songs will appear immediately.

---

## Generating Background MP4 Videos

Clone Hero PS4 features hardware MP4 background video playback. You can have a single universal pool of videos or individual music videos for each song.

### 1. Video Requirements for PS4
To ensure smooth 60 FPS gameplay without audio or video stutter, videos should be encoded in **H.264 High Profile @ Level 4.1**, 1080p or 720p, 30 or 60 FPS, with audio stripped (or AAC).

### 2. Using the Automated Converter Script
We provide a dedicated batch script `convert_video_backgrounds.sh`:
```bash
./convert_video_backgrounds.sh /path/to/input_videos /path/to/output_videos
```

### 3. Manual ffmpeg Command
If you want to convert videos manually using `ffmpeg`, use this command:
```bash
ffmpeg -i input_video.mp4 -vf "scale=1920:1080:force_original_aspect_ratio=decrease,pad=1920:1080:(ow-iw)/2:(oh-ih)/2" -c:v libx264 -profile:v high -level:v 4.1 -preset fast -crf 22 -pix_fmt yuv420p -r 30 -an -movflags +faststart video.mp4
```

Explanation of flags:
* `-vf scale...pad`: Centers and scales any video cleanly to 1920x1080.
* `-an`: Strips audio so it does not interfere with the song's audio stems.
* `-movflags +faststart`: Places the MP4 moov atom at the front of the file for instant streaming on Orbis OS.

### 4. Where to Place the Videos on PS4
* **Per-Song Video**: Place `video.mp4` directly inside the song's folder (e.g. `/data/GH/Metallica - One/video.mp4`).
* **Random Background Pool**: Upload videos to `/data/GH/backgrounds/`. The engine will pick random backgrounds from this folder.

### 5. Enabling in Game
1. In the Main Menu, go to **Settings > Sub-Menu 1 (Theme and Colors)**.
2. Select **Highway 5: Transparent** so the background video is fully visible behind the note gems.
3. Ensure **Menu Background Video** is set to **ON**.

---

## Controller and USB Guitar Setup

Press **OPTIONS** on the main menu or navigate to **Settings > Sub-Menu 4 (Controls)**:

### Preset 0: Standard DualShock 4 (Tested and Verified)
* **Green (Lane 1)**: `Cross (X)`
* **Red (Lane 2)**: `Circle (O)`
* **Yellow (Lane 3)**: `Triangle (Δ)`
* **Blue (Lane 4)**: `Square (□)`
* **Orange (Lane 5)**: `R1` (or `L1`)
* **Strum**: `D-Pad Up` / `D-Pad Down` (or enable Tap Mode in Settings for no-strum gamepad play)
* **Star Power**: `R2 Trigger`

### Preset 3: USB Guitar (GH and Rock Band PS3/PS4)
*Implemented for community testing:*
* **Green Fret**: `R2` or `L2`
* **Red Fret**: `Circle`
* **Yellow Fret**: `Triangle`
* **Blue Fret**: `Cross`
* **Orange Fret**: `Square` or `R1`
* **Strum Up / Down**: `D-Pad Up` / `D-Pad Down`
* **Star Power**: `Touchpad`, `Tilt`, or `Options`
* **Whammy**: `Right Analog Stick Y-Axis`

---

## Detailed File Breakdown

| File | Purpose and Description |
|:---|:---|
| **`ps4_main.cpp`** | Main Orbis OS game loop, 1080p double-buffered renderer, audio mixer, UI states, and HSV color square. |
| **`game.h`** | Rhythm engine state machine, millisecond hit windows, multipliers (1x-4x), streaks, and scoring. |
| **`midi.h`** | Dual chart parser supporting both `.chart` files and standard Guitar Hero / Rock Band `notes.mid` MIDI. |
| **`ps4_audio_decoder.h`** | In-memory zero-stall audio streamer supporting `.opus` (libopus 1.4), `.ogg`, `.mp3`, `.flac`, `.wav`. |
| **`mp4_player.h`** | Video player streaming MP4 background concert videos behind transparent highways. |
| **`font_hd.h` / `font8x16.h`** | Vectorized TrueType glyph contours and fallback bitmap fonts for crisp 1080p text rendering. |
| **`stb_image.h`** | Single-header image loader decoding JPEG and PNG textures directly to 32-bit ARGB buffers. |
| **`icon0.png`** | 512x512 official PS4 dashboard icon with metallic forward-slanted "CLONE HERO PS4" lettering. |
| **`package.gp4`** | OpenOrbis PKG project configuration defining `TITLE_ID=BREW00100` and staged assets. |
| **`.build_version`** | Version tracking file automatically bumped during compilation (e.g. `1.22`). |
| **`highways/`** | Directory containing 21 custom high-resolution highway skin textures. |
| **`build_pkg.sh`** | Automated 7-stage script: compiles C++17, signs `eboot.bin`, and outputs `hero_v1.22.pkg`. |
| **`update_and_build.sh`** | Quick-update utility extracting newest archives from `~/Downloads` and triggering a clean rebuild. |
| **`upload_songs_to_ps4.sh`** | Automated FTP uploader transferring songs, album covers, and videos directly to `/data/GH/`. |
| **`convert_video_backgrounds.sh`** | ffmpeg script optimizing concert videos for smooth 1080p PS4 background playback. |
| **`convert_opus_songs.sh`** | Audio conversion tool encoding large songs to high-efficiency, lightweight `.opus` stems. |
| **`convert_enchor_song.sh`** | Chart importer utility for Enchor and Chorus community setlists. |
| **`push_to_github.sh`** | Automated Git publisher script committing, tagging, and pushing releases to GitHub. |
| **`upload_to_github.py`** | Zero-dependency Python 3 script uploading repository and `.pkg` binary via GitHub REST API. |
| **`RELEASE_NOTES_v1.22.md`** | Pre-formatted markdown release notes template for GitHub Releases. |
| **`LICENSE`** | GNU General Public License v3.0 (GPL-3.0). |
| **`.gitignore`** | Clean exclusion rules filtering compiler caches (`build/`, `*.o`, `*.elf`, `*.prx`). |

---

## Bundled Highway Skins

The `highways/` folder includes 21 high-resolution textures accessible in-game:

1. `Velocity`
2. `Metallic`
3. `Overdose`
4. `Electric`
5. `Cracked`
6. `Transparent` (for video backgrounds)
7. `Cosmic-1`
8. `Cosmic-2`
9. `Cosmic-3`
10. `FlowerPower`
11. `Floral-1`
12. `Floral-2`
13. `FatTony`
14. `PinkLaser`
15. `Pink_Border`
16. `Rainbow_Border`
17. `RoyalCarpet`
18. `SmokeHero`
19. `VelocityColor`
20. `Christmas06`
21. `mA.I.nia`

---

## Architecture and Technical Highlights

```
+------------------------------------------------------------------------+
|                        PS4 Orbis OS Userland                           |
+----------------------------------+-------------------------------------+
|        Render Thread (60 Hz)     |       Audio Thread (Real-Time)      |
|  - 1080p Double-Buffered Scanout |  - Real-Time SCHED_FIFO (Pri 90)    |
|  - DirectMemory Tiled A8R8G8B8   |  - In-Memory Zero-Stall Streaming   |
|  - Perspective Highway 3D Math   |  - 256-sample grain (10.66ms DMA)   |
|  - 1D Scanline AA Ellipse Gems   |  - Native Multi-Stem Mixing (.opus) |
|  - Receptors & Hit Sparks (260ms)|  - Real-Time Guitar Stem Muting     |
|  - 21 Custom Highway Textures    |  - Marshall JCM Tube Overdrive SFX  |
+----------------------------------+-------------------------------------+
```

---

## Building from Source

### Prerequisites
* Linux (Ubuntu 20.04/22.04, Debian, Arch Linux) or WSL2.
* [OpenOrbis PS4 Toolchain](https://github.com/OpenOrbis/OpenOrbis-PS4-Toolchain) installed and exported:
  ```bash
  export OO_PS4_TOOLCHAIN="/home/username/Documents/OpenOrbis/PS4Toolchain"
  ```
* Host packages:
  ```bash
  sudo apt update && sudo apt install -y build-essential clang llvm lld git python3 curl unzip
  ```

### Build Command
```bash
chmod +x *.sh
./build_pkg.sh
```
Output package: **`hero_v1.22.pkg`** (installable directly via GoldHEN Package Installer).



---

## License

This project is licensed under the **GNU General Public License v3.0 (GPL-3.0)**. See [LICENSE](LICENSE) for details.

*Disclaimer: Clone Hero and Guitar Hero are registered trademarks of their respective owners. This is an independent, non-commercial open-source homebrew recreation.*
