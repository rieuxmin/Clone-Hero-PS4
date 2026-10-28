#!/usr/bin/env bash
# ==============================================================================
# Enchor Song Converter for OpenOrbis PS4 (stb_vorbis requires 48kHz stereo OGG)
# ==============================================================================
set -e

SONG_DIR="${1:-.}"
OUTPUT_OGG="${2:-$SONG_DIR/song.ogg}"

echo "=== Processing Enchor Song in: $SONG_DIR ==="

# Check if multi-track stems exist in directory
OPUS_STEMS=$(find "$SONG_DIR" -maxdepth 1 -name "*.opus" | sort)
OGG_STEMS=$(find "$SONG_DIR" -maxdepth 1 -name "*.ogg" ! -name "song.ogg" | sort)

ALL_STEMS="$OPUS_STEMS $OGG_STEMS"
NUM_STEMS=$(echo "$ALL_STEMS" | wc -w)

if [ "$NUM_STEMS" -gt 1 ]; then
    echo "Found $NUM_STEMS multi-track audio stems. Mixing into 48kHz stereo Vorbis song.ogg..."
    INPUT_ARGS=""
    for stem in $ALL_STEMS; do
        INPUT_ARGS="$INPUT_ARGS -i \"$stem\""
    done

    eval ffmpeg -y $INPUT_ARGS \
        -filter_complex "\"amix=inputs=$NUM_STEMS:duration=longest:dropout_transition=2\"" \
        -c:a libvorbis -b:a 192k -ar 48000 \
        "\"$OUTPUT_OGG\""
elif [ -f "$SONG_DIR/song.opus" ]; then
    echo "Converting single song.opus to 48kHz Ogg Vorbis..."
    ffmpeg -y -i "$SONG_DIR/song.opus" -c:a libvorbis -b:a 192k -ar 48000 "$OUTPUT_OGG"
elif [ -f "$SONG_DIR/song.mp3" ]; then
    echo "Converting song.mp3 to 48kHz Ogg Vorbis..."
    ffmpeg -y -i "$SONG_DIR/song.mp3" -c:a libvorbis -b:a 192k -ar 48000 "$OUTPUT_OGG"
elif [ -f "$SONG_DIR/song.ogg" ]; then
    echo "Re-sampling song.ogg to exact 48000Hz stereo for PS4 hardware sceAudioOut..."
    ffmpeg -y -i "$SONG_DIR/song.ogg" -c:a libvorbis -b:a 192k -ar 48000 "$OUTPUT_OGG.tmp"
    mv "$OUTPUT_OGG.tmp" "$OUTPUT_OGG"
# Check if video.mp4 or background image exists; extract 1080p background.raw for PS4 engine
VID_FILE=$(find "$SONG_DIR" -maxdepth 1 \( -iname "*.mp4" -o -iname "*.webm" -o -iname "*.avi" \) | head -n 1 || true)
IMG_FILE=$(find "$SONG_DIR" -maxdepth 1 \( -iname "background.png" -o -iname "background.jpg" -o -iname "album.png" -o -iname "album.jpg" \) | head -n 1 || true)

if [ -n "$VID_FILE" ] && [ ! -f "$SONG_DIR/background.raw" ]; then
    echo "Extracting 1080p stage background from $(basename "$VID_FILE")..."
    ffmpeg -y -ss 00:00:04 -i "$VID_FILE" -vf "scale=1920:1080:force_original_aspect_ratio=increase,crop=1920:1080" -vframes 1 -f rawvideo -pix_fmt rgba "$SONG_DIR/background.raw" 2>/dev/null || \
    ffmpeg -y -i "$VID_FILE" -vf "scale=1920:1080:force_original_aspect_ratio=increase,crop=1920:1080" -vframes 1 -f rawvideo -pix_fmt rgba "$SONG_DIR/background.raw" 2>/dev/null || true
elif [ -n "$IMG_FILE" ] && [ ! -f "$SONG_DIR/background.raw" ]; then
    echo "Converting background image $(basename "$IMG_FILE") to 1080p raw for PS4 engine..."
    ffmpeg -y -i "$IMG_FILE" -vf "scale=1920:1080:force_original_aspect_ratio=increase,crop=1920:1080" -vframes 1 -f rawvideo -pix_fmt rgba "$SONG_DIR/background.raw" 2>/dev/null || true
fi

echo "SUCCESS: $OUTPUT_OGG and stage assets are ready for PS4 OpenOrbis engine!"
