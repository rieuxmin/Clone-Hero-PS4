#!/usr/bin/env bash
# ==============================================================================
# PS4 Clone Hero - Songs (.opus/.ogg/.mp3/.flac/.wav), Album Art & 1080p MP4 Uploader
#   - Natively decodes .opus, .ogg, .mp3, .flac, and .wav directly on PS4!
#   - Incorporates full 1920x1080 .mp4 background videos for 60FPS direct PS4 streaming
#   - Uploads songs to /data/GH/ and default 1080p background .mp4s to /data/GH_backgrounds/
# ==============================================================================
set -e

if [ -z "$1" ]; then
    echo "Usage:"
    echo "  1) Upload songs (supports .opus/.ogg/.mp3/.flac/.wav & syncs ~/music/GH_backgrounds):"
    echo "     ./upload_songs_to_ps4.sh <PS4_IP_ADDRESS> ~/music/GH"
    echo ""
    echo "  2) Upload a folder of default 1080p background .mp4s to /data/GH_backgrounds:"
    echo "     ./upload_songs_to_ps4.sh <PS4_IP_ADDRESS> /path/to/default_backgrounds --backgrounds"
    exit 1
fi

PS4_IP="$1"
PS4_PORT="${PS4_FTP_PORT:-2121}"
RAW_SRC="${2:-/home/br/music/GH}"
MODE_FLAG="${3:-}"

# Auto-correct /br/home/... -> /home/br/...
if [[ "$RAW_SRC" == /br/home/* ]]; then
    CORRECTED="/home/br/${RAW_SRC#/br/home/}"
    if [ -d "$CORRECTED" ]; then
        RAW_SRC="$CORRECTED"
    fi
fi

SOURCE_DIR="$RAW_SRC"

if [ ! -d "$SOURCE_DIR" ] && [ ! -f "$SOURCE_DIR" ]; then
    CANDIDATES=(
        "$HOME/music/GH"
        "$HOME/Music/GH"
        "/home/br/music/GH"
        "/home/br/Music/GH"
        "$HOME/music"
        "$HOME/Music"
        "$PWD/GH"
    )
    for c in "${CANDIDATES[@]}"; do
        if [ -d "$c" ]; then
            SOURCE_DIR="$c"
            break
        fi
    done
fi

echo "=== PS4 Clone Hero - Universal Audio, Full 1080p MP4 & Backgrounds FTP Uploader ==="
echo "Target PS4: $PS4_IP:$PS4_PORT"
echo "Source:     $SOURCE_DIR"
echo ""

if [ ! -d "$SOURCE_DIR" ] && [ ! -f "$SOURCE_DIR" ]; then
    echo "Error: Directory '$RAW_SRC' not found."
    exit 1
fi

# 1. Verify audio files (.opus/.ogg/.mp3/.flac/.wav play natively; extract song.ogg if only .m4a/.mp4 exists)
echo "[1/4] Scanning song audio files (.opus, .ogg, .mp3, .flac, .wav)..."
if [ -x "./convert_opus_songs.sh" ] && [ -d "$SOURCE_DIR" ]; then
    ./convert_opus_songs.sh "$SOURCE_DIR"
fi

# 2. Ensure all background videos are Full HD 1920x1080 for direct PS4 streaming
echo "[2/4] Preparing Full HD 1920x1080 .mp4 videos for direct PS4 playback..."
if [ -x "./convert_video_backgrounds.sh" ] && [ -d "$SOURCE_DIR" ]; then
    ./convert_video_backgrounds.sh "$SOURCE_DIR"
elif command -v ffmpeg >/dev/null 2>&1 && [ -d "$SOURCE_DIR" ]; then
    while IFS= read -r video; do
        [ -z "$video" ] && continue
        dir=$(dirname "$video")
        base=$(basename "$video")
        stem="${base%.*}"
        if [ -f "$dir/song.ini" ] || [ -f "$dir/notes.chart" ] || [ -f "$dir/notes.mid" ]; then
            target_mp4="$dir/video.mp4"
        else
            target_mp4="$dir/${stem}.mp4"
        fi
        CODEC=""
        WIDTH=""
        if command -v ffprobe >/dev/null 2>&1; then
            CODEC=$(ffprobe -v error -select_streams v:0 -show_entries stream=codec_name -of default=noprint_wrappers=1:nokey=1 "$video" </dev/null 2>/dev/null | head -n 1 || true)
            WIDTH=$(ffprobe -v error -select_streams v:0 -show_entries stream=width -of default=noprint_wrappers=1:nokey=1 "$video" </dev/null 2>/dev/null | head -n 1 || true)
        fi
        if [ "$CODEC" = "mjpeg" ] && [ "$WIDTH" = "1920" ] && [ "$video" = "$target_mp4" ]; then
            continue
        fi
        echo "  [+] Converting $base -> Full HD 1920x1080 $(basename "$target_mp4")..."
        tmp_mp4="$dir/.ps4_opt_${RANDOM}.mp4"
        if ffmpeg -y -i "$video" -vf "fps=30,scale=1920:1080:force_original_aspect_ratio=increase:flags=lanczos,crop=1920:1080" -c:v mjpeg -q:v 2 -pix_fmt yuvj420p -movflags +faststart -an "$tmp_mp4" </dev/null >/dev/null 2>&1; then
            mv -f "$tmp_mp4" "$target_mp4"
            [ "$video" != "$target_mp4" ] && rm -f "$video"
        else
            rm -f "$tmp_mp4"
        fi
    done < <(find "$SOURCE_DIR" -type f \( -iname "*.mp4" -o -iname "*.webm" -o -iname "*.avi" -o -iname "*.mkv" -o -iname "*.mov" \) ! -name ".*" 2>/dev/null)
fi

echo "[3/4] Testing connection to PS4 GoldHEN FTP at $PS4_IP:$PS4_PORT..."
if ! nc -z -w 3 "$PS4_IP" "$PS4_PORT" 2>/dev/null; then
    echo "Warning: Could not reach $PS4_IP:$PS4_PORT. Ensure GoldHEN FTP is enabled."
fi

echo "[4/4] Uploading to PS4 FTP ($PS4_IP:$PS4_PORT)..."
python3 - <<PYEOF
import os, sys, ftplib

host = "$PS4_IP"
port = int("$PS4_PORT")
src_root = "$SOURCE_DIR"
mode_flag = "$MODE_FLAG"

ftp = ftplib.FTP()
try:
    ftp.connect(host, port, timeout=15)
    ftp.login()
    print("Connected to PS4 FTP successfully!")
except Exception as e:
    print(f"FTP Connection failed: {e}")
    sys.exit(1)

def ensure_ftp_dir(path):
    parts = path.strip("/").split("/")
    cur = ""
    for part in parts:
        cur += "/" + part
        try:
            ftp.mkd(cur)
        except:
            pass

# Always create /data/GH (songs), /data/GH/backgrounds, and /data/GH_backgrounds (random default 1080p MP4 pool)
ensure_ftp_dir("/data/GH")
ensure_ftp_dir("/data/GH/backgrounds")
ensure_ftp_dir("/data/GH_backgrounds")

has_songs = False
if os.path.isdir(src_root):
    for r, d, files in os.walk(src_root):
        if any(f.lower() in ("song.ini", "notes.chart", "notes.mid") for f in files):
            has_songs = True
            break

is_bg_only = (mode_flag in ("--backgrounds", "-b", "backgrounds")) or (not has_songs and "background" in os.path.basename(src_root.rstrip("/")).lower())

if is_bg_only or (os.path.isdir(src_root) and not has_songs):
    print(f"\n[Default 1080p Background MP4 Pool Mode] Uploading .mp4 files to /data/GH/backgrounds/ ...")
    for r, d, files in os.walk(src_root):
        for file in sorted(files):
            if file.startswith(".") or not file.lower().endswith(".mp4"):
                continue
            local_path = os.path.join(r, file)
            remote_path = f"/data/GH/backgrounds/{file}"
            sz_mb = os.path.getsize(local_path) / (1024.0 * 1024.0)
            print(f"  -> Uploading 1080p BG: {file} ({sz_mb:.1f} MB) -> {remote_path}")
            with open(local_path, 'rb') as f:
                ftp.storbinary(f"STOR {remote_path}", f)
else:
    for root, dirs, files in os.walk(src_root):
        rel_dir = os.path.relpath(root, src_root)
        if rel_dir.lower() in ("backgrounds", "gh_backgrounds", "default_backgrounds"):
            for file in sorted(files):
                if file.startswith(".") or not file.lower().endswith(".mp4"):
                    continue
                local_path = os.path.join(root, file)
                remote_path = f"/data/GH/backgrounds/{file}"
                sz_mb = os.path.getsize(local_path) / (1024.0 * 1024.0)
                print(f"  -> Uploading 1080p BG: {file} ({sz_mb:.1f} MB) -> {remote_path}")
                with open(local_path, 'rb') as f:
                    ftp.storbinary(f"STOR {remote_path}", f)
            continue

        if rel_dir == ".":
            cur_remote = "/data/GH"
        else:
            cur_remote = f"/data/GH/{rel_dir.replace(os.sep, '/')}"
            ensure_ftp_dir(cur_remote)

        for file in sorted(files):
            if file.startswith("."):
                continue
            local_path = os.path.join(root, file)
            remote_path = f"{cur_remote}/{file}"
            sz_mb = os.path.getsize(local_path) / (1024.0 * 1024.0)
            print(f"  -> Uploading {file} ({sz_mb:.1f} MB) -> {remote_path} ...")
            with open(local_path, 'rb') as f:
                ftp.storbinary(f"STOR {remote_path}", f)

    parent_dir = os.path.dirname(src_root.rstrip("/"))
    for extra_bg_dir in (os.path.join(parent_dir, "GH_backgrounds"), os.path.join(parent_dir, "backgrounds")):
        if os.path.isdir(extra_bg_dir) and os.path.abspath(extra_bg_dir) != os.path.abspath(src_root):
            print(f"\n[Auto-Detected Default 1080p Backgrounds Folder: {extra_bg_dir}]")
            for file in sorted(os.listdir(extra_bg_dir)):
                if file.startswith(".") or not file.lower().endswith(".mp4"):
                    continue
                local_path = os.path.join(extra_bg_dir, file)
                if os.path.isfile(local_path):
                    remote_path = f"/data/GH_backgrounds/{file}"
                    sz_mb = os.path.getsize(local_path) / (1024.0 * 1024.0)
                    print(f"  -> Uploading 1080p BG: {file} ({sz_mb:.1f} MB) -> {remote_path}")
                    with open(local_path, 'rb') as f:
                        ftp.storbinary(f"STOR {remote_path}", f)

ftp.quit()
print("\n[OK] Transfer complete! Songs in /data/GH/ and Default 1080p Backgrounds in /data/GH_backgrounds/")
PYEOF

echo ""
echo "====================================================================="
echo "SUCCESS: Uploaded to PS4!"
echo " - Song folders (.opus/.ogg/.mp3/.flac/.wav) & photos -> /data/GH/<Song>/"
echo " - Default Full HD 1920x1080 video MP4s               -> /data/GH_backgrounds/"
echo "====================================================================="
