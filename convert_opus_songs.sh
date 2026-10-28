#!/usr/bin/env bash
set -euo pipefail

# ==============================================================================
# PS4 Clone Hero - Universal Audio Extractor & Multi-Stem Helper (Build v1.13+)
# ==============================================================================
# NOTE: Build v1.13+ natively decodes .opus, .ogg, .mp3, .flac, and .wav
#       (both single-track and up to 8 multi-track stems) directly on the PS4!
#       This script additionally checks for rare container formats (.m4a, .aac,
#       .wma, or audio-only .mp4 songs without a separate audio file) and
#       extracts 48kHz stereo song.ogg so 100% of custom songs have audio.
# ==============================================================================

TARGET_DIR="${1:-$HOME/music/GH}"

if [ ! -d "$TARGET_DIR" ]; then
    echo "Folder not found: $TARGET_DIR"
    exit 0
fi

if ! command -v ffmpeg >/dev/null 2>&1; then
    echo "NOTE: ffmpeg not installed on PC (native PS4 .opus/.ogg/.mp3/.flac/.wav decoders will handle audio directly)."
    exit 0
fi

echo "=================================================================="
echo "PS4 Clone Hero - Universal Audio Scanner (Build v1.13+)"
echo "Target Folder: $TARGET_DIR"
echo "=================================================================="

CONVERTED_COUNT=0
READY_COUNT=0

while IFS= read -r -d '' song_dir; do
    # Check if folder already has any natively supported PS4 audio file (.opus, .ogg, .mp3, .flac, .wav)
    shopt -s nullglob nocaseglob
    native_files=("$song_dir"/*.opus "$song_dir"/*.ogg "$song_dir"/*.mp3 "$song_dir"/*.flac "$song_dir"/*.wav)
    container_files=("$song_dir"/*.m4a "$song_dir"/*.aac "$song_dir"/*.wma "$song_dir"/*.mp4 "$song_dir"/*.mkv "$song_dir"/*.webm)
    shopt -u nullglob nocaseglob

    if [ "${#native_files[@]}" -gt 0 ]; then
        READY_COUNT=$((READY_COUNT + 1))
        continue
    fi

    if [ "${#container_files[@]}" -eq 0 ]; then
        continue
    fi

    src_media="${container_files[0]}"
    out_ogg="$song_dir/song.ogg"
    echo "  [AUDIO] Extracting 48kHz stereo song.ogg from $(basename "$src_media") in $(basename "$song_dir")..."
    if ffmpeg -nostdin -y -loglevel error -i "$src_media" \
        -vn -c:a libvorbis -ar 48000 -ac 2 -q:a 6 "$out_ogg"; then
        CONVERTED_COUNT=$((CONVERTED_COUNT + 1))
        echo "          -> Created: $out_ogg"
    fi
done < <(find "$TARGET_DIR" -mindepth 1 -maxdepth 3 -type d -print0)

echo "=================================================================="
echo "Audio Scan Complete!"
echo " - Native (.opus/.ogg/.mp3/.flac/.wav) Ready: $READY_COUNT song folder(s)"
echo " - Extracted from (.m4a/.mp4/.aac/.wma):      $CONVERTED_COUNT song folder(s)"
echo "=================================================================="
