#!/usr/bin/env bash
set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
cd "$SCRIPT_DIR"

echo "=================================================================="
echo "=== PS4 Clone Hero: Pull Update from /downloads & Clean Build ==="
echo "=================================================================="

ZIP_PATH="${1:-}"

if [ -z "$ZIP_PATH" ]; then
    DOWNLOAD_DIRS=(
        "$HOME/Downloads"
        "/home/br/Downloads"
        "/downloads"
        "$SCRIPT_DIR/../Downloads"
        "/tmp"
        "."
    )
    for d in "${DOWNLOAD_DIRS[@]}"; do
        if [ -d "$d" ]; then
            CAND="$(find "$d" -maxdepth 2 -type f \( -name "ps4_hero_build_v1.2*.zip" -o -name "ps4_hero_build_latest.zip" -o -name "ps4_hero_build_v*.zip" \) 2>/dev/null | sort -V | tail -n 1 || true)"
            if [ -n "$CAND" ] && [ -f "$CAND" ]; then
                ZIP_PATH="$CAND"
                break
            fi
        fi
    done
fi

if [ -z "$ZIP_PATH" ] || [ ! -f "$ZIP_PATH" ]; then
    echo "ERROR: Could not find ps4_hero_build_v1.22.zip in ~/Downloads or /downloads."
    echo ""
    echo "Usage:"
    echo "  ./update_and_build.sh /path/to/ps4_hero_build_v1.22.zip"
    echo "Or place ps4_hero_build_v1.22.zip in ~/Downloads and re-run."
    exit 1
fi

echo "[+] Located update package: $ZIP_PATH"
echo "[+] Extracting fresh v1.22 files directly into $SCRIPT_DIR..."
unzip -o "$ZIP_PATH" -d "$SCRIPT_DIR"
chmod +x "$SCRIPT_DIR"/*.sh 2>/dev/null || true

echo "[+] Wiping stale build objects (forcing clean rebuild of ps4_main.cpp)..."
rm -f "$SCRIPT_DIR/build/ps4_main.o" "$SCRIPT_DIR/build/hero.elf" "$SCRIPT_DIR/build/hero.oelf" "$SCRIPT_DIR/build/pkg_root/eboot.bin"

echo "[+] Current build version in .build_version:"
cat "$SCRIPT_DIR/.build_version"

echo "[+] Executing ./build_pkg.sh..."
exec ./build_pkg.sh
