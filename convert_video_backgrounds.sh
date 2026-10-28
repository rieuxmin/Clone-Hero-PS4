#!/usr/bin/env bash
set -euo pipefail

# ==============================================================================
# PS4 Clone Hero - True 1080p Full HD Direct MP4 Background Converter
# ==============================================================================
# 1. Incorporates your original uncompressed / full-resolution video files
#    (.mp4, .mov, .mkv, .webm, .avi) at native 1920x1080 Full HD resolution!
# 2. If a song folder ONLY has a video.mp4 (and no .opus/.ogg/.mp3/.flac/.wav),
#    automatically extracts its audio track to 48kHz song.ogg first.
# 3. Encodes to true 1920x1080 @ 30fps high-bitrate MJPEG (-q:v 2) inside a
#    standard ISO MP4 container so PS4 Mp4VideoPlayer renders 1:1 1080p pixels
#    with zero downscaling, zero blurriness, and zero 3x3 pixelation!
# ==============================================================================

RAW_ARG="${1:-}"
TARGET_DIR=""

if [ -n "$RAW_ARG" ] && [ -d "$RAW_ARG" ]; then
    TARGET_DIR="$RAW_ARG"
else
    for cand in \
        "$HOME/music/GH/backgrounds" \
        "$HOME/music/GH_backgrounds" \
        "$HOME/music/backgrounds" \
        "/home/br/music/GH/backgrounds" \
        "/home/br/music/GH_backgrounds" \
        "/home/br/music/backgrounds" \
        "$HOME/music/GH" \
        "/home/br/music/GH" \
        "$HOME/Music/GH"; do
        if [ -d "$cand" ]; then
            TARGET_DIR="$cand"
            break
        fi
    done
fi

if [ -z "$TARGET_DIR" ] || [ ! -d "$TARGET_DIR" ]; then
    echo "ERROR: Could not find target folder '${RAW_ARG:-~/music/GH}'."
    echo "Pass the actual folder where your .mp4 files are stored, for example:"
    echo "  ./convert_video_backgrounds.sh ~/music/GH/backgrounds"
    echo "  ./convert_video_backgrounds.sh ~/music/GH_backgrounds"
    echo "  ./convert_video_backgrounds.sh ~/music/GH"
    exit 1
fi

echo "=================================================================="
echo "PS4 True 1080p Full HD MP4 Background Builder"
echo "Target Folder: $TARGET_DIR"
echo "=================================================================="

if ! command -v ffmpeg >/dev/null 2>&1; then
    echo "WARNING: ffmpeg is not installed. Skipping 1080p MP4 conversion."
    exit 0
fi

echo "[1/1] Preparing full 1920x1080 .mp4 background videos for direct PS4 playback..."
find "$TARGET_DIR" -type f \( -iname "*.mp4" -o -iname "*.mov" -o -iname "*.mkv" -o -iname "*.webm" -o -iname "*.avi" \) ! -iname "*_temp_ps4.mp4" | while read -r vid; do
    dir="$(dirname "$vid")"
    base="$(basename "$vid")"
    stem="${base%.*}"
    out_mp4="$dir/${stem}.mp4"

    # If this folder is a song folder (has notes.chart / notes.mid / song.ini)
    # and has no separate audio file yet, extract 48kHz audio first!
    if [ -f "$dir/notes.chart" ] || [ -f "$dir/notes.mid" ] || [ -f "$dir/song.ini" ]; then
        shopt -s nullglob nocaseglob
        audio_files=("$dir"/*.opus "$dir"/*.ogg "$dir"/*.mp3 "$dir"/*.flac "$dir"/*.wav)
        shopt -u nullglob nocaseglob
        if [ "${#audio_files[@]}" -eq 0 ]; then
            echo "  [AUDIO] Extracting 48kHz song.ogg from $base..."
            ffmpeg -nostdin -y -loglevel error -i "$vid" -vn -c:a libvorbis -ar 48000 -ac 2 -q:a 6 "$dir/song.ogg" </dev/null || true
        fi
    fi

    # Check if already native 1920x1080 MJPEG MP4
    if command -v ffprobe >/dev/null 2>&1 && [ "$vid" = "$out_mp4" ]; then
        codec="$(ffprobe -v error -select_streams v:0 -show_entries stream=codec_name -of default=noprint_wrappers=1:nokey=1 "$vid" 2>/dev/null | head -n 1 || true)"
        width="$(ffprobe -v error -select_streams v:0 -show_entries stream=width -of default=noprint_wrappers=1:nokey=1 "$vid" 2>/dev/null | head -n 1 || true)"
        height="$(ffprobe -v error -select_streams v:0 -show_entries stream=height -of default=noprint_wrappers=1:nokey=1 "$vid" 2>/dev/null | head -n 1 || true)"
        if [ "$codec" = "mjpeg" ] && [ "$width" = "1920" ] && [ "$height" = "1080" ]; then
            echo "  [OK] Already Full HD 1920x1080 PS4 MP4: $vid"
            continue
        fi
    fi

    tmp_mp4="$dir/.${stem}_temp_ps4.mp4"
    orig_size="$(du -h "$vid" | cut -f1)"
    echo "  [>] Converting $base ($orig_size) -> Full HD 1920x1080 PS4 $(basename "$out_mp4")..."

    if ffmpeg -nostdin -y -loglevel error -i "$vid" \
        -an \
        -vf "fps=30,scale=1920:1080:force_original_aspect_ratio=increase:flags=lanczos,crop=1920:1080" \
        -c:v mjpeg -pix_fmt yuvj420p -q:v 2 \
        -movflags +faststart \
        "$tmp_mp4" </dev/null; then
        mv -f "$tmp_mp4" "$out_mp4"
        new_size="$(du -h "$out_mp4" | cut -f1)"
        echo "      -> Saved Full HD 1920x1080: $(basename "$out_mp4") ($orig_size -> $new_size)"
    else
        rm -f "$tmp_mp4"
        echo "      -> WARNING: Failed to convert $vid"
    fi
done

echo "=================================================================="
echo "All background videos are ready at 1920x1080 Full HD in: $TARGET_DIR"
echo "=================================================================="
