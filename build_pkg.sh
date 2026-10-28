#!/usr/bin/env bash
set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
cd "$SCRIPT_DIR"

# ==============================================================================
# AUTO-PULL FROM DOWNLOADS (Ensures new zip downloads are unpacked automatically)
# ==============================================================================
# Searches for latest ps4_hero_build_*.zip in ~/Downloads, /home/br/Downloads, /downloads, or current dir
FORCE_PULL=0
if [[ "${1:-}" == "--pull" || "${1:-}" == "-p" || "${1:-}" == "--update" ]]; then
    FORCE_PULL=1
fi

DOWNLOAD_CANDIDATES=(
    "$HOME/Downloads"
    "/home/br/Downloads"
    "/downloads"
    "$SCRIPT_DIR/../Downloads"
    "/tmp"
    "."
)

NEWEST_ZIP=""
for d in "${DOWNLOAD_CANDIDATES[@]}"; do
    if [ -d "$d" ]; then
        CAND="$(find "$d" -maxdepth 2 -type f \( -name "ps4_hero_build_v1.2*.zip" -o -name "ps4_hero_build_latest.zip" -o -name "ps4_hero_build_v*.zip" \) 2>/dev/null | sort -V | tail -n 1 || true)"
        if [ -n "$CAND" ] && [ -f "$CAND" ]; then
            NEWEST_ZIP="$CAND"
            break
        fi
    fi
done

if [ -n "$NEWEST_ZIP" ]; then
    SHOULD_EXTRACT=0
    if [ "$FORCE_PULL" -eq 1 ]; then
        SHOULD_EXTRACT=1
    elif [ ! -f "$SCRIPT_DIR/ps4_main.cpp" ]; then
        SHOULD_EXTRACT=1
    else
        # If the downloaded zip is newer than our local ps4_main.cpp, auto-pull!
        if [ "$NEWEST_ZIP" -nt "$SCRIPT_DIR/ps4_main.cpp" ]; then
            SHOULD_EXTRACT=1
        fi
        # Or if local version is older than 1.21 and the zip is v1.21
        if [[ "$NEWEST_ZIP" =~ "1.21" ]] && [ -f "$SCRIPT_DIR/.build_version" ]; then
            LOC_VER="$(cat "$SCRIPT_DIR/.build_version" 2>/dev/null | tr -d '[:space:]' || echo "1.20")"
            if awk "BEGIN {exit !($LOC_VER < 1.22)}"; then
                SHOULD_EXTRACT=1
            fi
        fi
    fi

    if [ "$SHOULD_EXTRACT" -eq 1 ]; then
        echo "=================================================================="
        echo ">>> AUTO-PULL: Found newer source zip archive in Downloads:"
        echo "    $NEWEST_ZIP"
        echo ">>> Synchronizing & extracting fresh v1.22+ source files..."
        echo "=================================================================="
        unzip -q -o "$NEWEST_ZIP" -d "$SCRIPT_DIR"
        chmod +x "$SCRIPT_DIR"/*.sh 2>/dev/null || true
        # Clean stale build cache so old objects are not re-linked
        rm -rf "$SCRIPT_DIR/build/pkg_root" "$SCRIPT_DIR/build/ps4_main.o" "$SCRIPT_DIR/build/hero.elf" 2>/dev/null || true
        echo ">>> Successfully updated working directory from $(basename "$NEWEST_ZIP")!"
    fi
fi

# ==============================================================================
# AUTOMATIC BUILD VERSION MANAGEMENT (Starts at 1.22 and bumps every build!)
# ==============================================================================
VERSION_FILE="$SCRIPT_DIR/.build_version"
DEFAULT_START_VER="1.22"

if [ ! -f "$VERSION_FILE" ]; then
    echo "$DEFAULT_START_VER" > "$VERSION_FILE"
fi

BUILD_VER="$(cat "$VERSION_FILE" | tr -d '[:space:]')"
# Ensure version is at least 1.22
if awk "BEGIN {exit !($BUILD_VER < 1.22)}"; then
    BUILD_VER="1.22"
    echo "$BUILD_VER" > "$VERSION_FILE"
fi

# Format 5-char SFO version string (e.g., 1.22 -> 01.22)
BUILD_VER_SFO="$(awk -v v="$BUILD_VER" 'BEGIN { printf "%05.2f", v }')"
PKG_FILENAME="hero_v${BUILD_VER}.pkg"
NEXT_BUILD_VER="$(awk -v v="$BUILD_VER" 'BEGIN { printf "%.2f", v + 0.01 }')"

echo "=================================================================="
echo "=== PS4 Clone Hero PKG Builder — BUILD v${BUILD_VER} (${PKG_FILENAME}) ==="
echo "=================================================================="

# Note: Song & video conversion is decoupled from PKG compilation.
# All audio formats (.opus, .ogg, .mp3, .flac, .wav) play natively on PS4.
# Transfer songs directly to /data/GH_songs/ and default videos to /data/GH_backgrounds/ via FTP.


echo "=== [1/7] Locating OpenOrbis PS4 Toolchain & Dependencies ==="
if [ -z "${OO_PS4_TOOLCHAIN:-}" ]; then
    for cand in \
        "$HOME/OpenOrbis/PS4Toolchain" \
        "$HOME/OpenOrbis-PS4-Toolchain" \
        "$HOME/Documents/OpenOrbis/PS4Toolchain" \
        "$HOME/ps4/OpenOrbis/PS4Toolchain" \
        "/opt/OpenOrbis/PS4Toolchain" \
        "/usr/local/OpenOrbis/PS4Toolchain"; do
        if [ -d "$cand" ]; then
            export OO_PS4_TOOLCHAIN="$cand"
            break
        fi
    done
fi

if [ -z "${OO_PS4_TOOLCHAIN:-}" ]; then
    FOUND_TC="$(find "$HOME" /opt /usr/local -maxdepth 4 -name "create-fself" 2>/dev/null | head -n 1 || true)"
    if [ -n "$FOUND_TC" ]; then
        export OO_PS4_TOOLCHAIN="$(cd "$(dirname "$FOUND_TC")/../.." && pwd)"
    fi
fi

if [ -z "${OO_PS4_TOOLCHAIN:-}" ] || [ ! -d "$OO_PS4_TOOLCHAIN" ]; then
    echo "ERROR: Could not find OpenOrbis PS4Toolchain directory."
    exit 1
fi
echo "Using OO_PS4_TOOLCHAIN=$OO_PS4_TOOLCHAIN"

for tool in clang clang++ ld.lld; do
    if ! command -v "$tool" >/dev/null 2>&1; then
        echo "ERROR: Required compiler tool '$tool' is not installed."
        exit 1
    fi
done

echo "=== [2/7] Fetching Native Audio Decoders (libopus 1.4, stb_vorbis, dr_mp3, dr_flac, dr_wav) & stb_image ==="
if [ ! -s "$SCRIPT_DIR/stb_vorbis.c" ]; then
    curl -fsSL "https://raw.githubusercontent.com/nothings/stb/master/stb_vorbis.c" -o "$SCRIPT_DIR/stb_vorbis.c"
fi
if [ ! -s "$SCRIPT_DIR/stb_image.h" ]; then
    curl -fsSL "https://raw.githubusercontent.com/nothings/stb/master/stb_image.h" -o "$SCRIPT_DIR/stb_image.h"
fi
if [ ! -s "$SCRIPT_DIR/dr_mp3.h" ]; then
    curl -fsSL "https://raw.githubusercontent.com/mackron/dr_libs/master/dr_mp3.h" -o "$SCRIPT_DIR/dr_mp3.h"
fi
if [ ! -s "$SCRIPT_DIR/dr_flac.h" ]; then
    curl -fsSL "https://raw.githubusercontent.com/mackron/dr_libs/master/dr_flac.h" -o "$SCRIPT_DIR/dr_flac.h"
fi
if [ ! -s "$SCRIPT_DIR/dr_wav.h" ]; then
    curl -fsSL "https://raw.githubusercontent.com/mackron/dr_libs/master/dr_wav.h" -o "$SCRIPT_DIR/dr_wav.h"
fi
if [ ! -d "$SCRIPT_DIR/opus-1.4" ]; then
    echo "Downloading libopus 1.4 source for native PS4 .opus decoding..."
    curl -fsSL "https://downloads.xiph.org/releases/opus/opus-1.4.tar.gz" -o "$SCRIPT_DIR/opus-1.4.tar.gz"
    tar -xzf "$SCRIPT_DIR/opus-1.4.tar.gz" -C "$SCRIPT_DIR"
    rm -f "$SCRIPT_DIR/opus-1.4.tar.gz"
fi
# Ensure float_cast.h uses standard math.h lrintf() instead of <xmmintrin.h> under OpenOrbis sysroot
if [ -f "$SCRIPT_DIR/opus-1.4/celt/float_cast.h" ]; then
    sed -i 's/#if defined(__GNUC__) && defined(__SSE__)/#if 0/g' "$SCRIPT_DIR/opus-1.4/celt/float_cast.h" || true
fi
# Guarantee arch.h never halts build on fast-math if caller compiler defines it
if [ -f "$SCRIPT_DIR/opus-1.4/celt/arch.h" ]; then
    sed -i 's/#error Cannot build libopus with -ffast-math/\/* bypassed *\/ \/\/ #error/g' "$SCRIPT_DIR/opus-1.4/celt/arch.h" || true
fi


BUILD_DIR="$SCRIPT_DIR/build"
PKG_ROOT="$BUILD_DIR/pkg_root"
OPUS_OBJ_DIR="$BUILD_DIR/opus_objs"

# Keep cached opus_objs across builds so libopus only compiles once!
mkdir -p "$BUILD_DIR" "$OPUS_OBJ_DIR"
rm -rf "$PKG_ROOT"
mkdir -p "$PKG_ROOT/sce_sys" "$PKG_ROOT/assets/song" "$PKG_ROOT/assets/highways"

if [ -d "$SCRIPT_DIR/highways" ]; then
    cp -f "$SCRIPT_DIR/highways/"* "$PKG_ROOT/assets/highways/" 2>/dev/null || true
fi

echo "=== [3/7] Compiling Native Decoders (libopus + stb_vorbis + dr_libs) & ps4_main.cpp (Build v${BUILD_VER}) ==="
OPUS_SRC="$SCRIPT_DIR/opus-1.4"
OPUS_INCLUDES="-I$OPUS_SRC/include -I$OPUS_SRC/celt -I$OPUS_SRC/silk -I$OPUS_SRC/silk/float -I$OPUS_SRC/src"
OPUS_CFLAGS="--target=x86_64-pc-freebsd12-elf -fPIC -O3 -msse4.1 -fno-fast-math -fno-finite-math-only -DFLOAT_APPROX -fno-strict-aliasing -w -DOPUS_BUILD -DVAR_ARRAYS -DHAVE_LRINTF -DHAVE_LRINT -isysroot $OO_PS4_TOOLCHAIN -isystem $OO_PS4_TOOLCHAIN/include $OPUS_INCLUDES"

# Compile libopus 1.4 objects if not already cached
if [ ! -f "$OPUS_OBJ_DIR/.opus_built_ok" ]; then
    rm -f "$OPUS_OBJ_DIR"/*.o 2>/dev/null || true
    echo "  -> Compiling libopus 1.4 C sources for PS4 x86_64 (cached after first build)..."
    OPUS_C_FILES=()
    for f in "$OPUS_SRC"/celt/*.c "$OPUS_SRC"/silk/*.c "$OPUS_SRC"/silk/float/*.c "$OPUS_SRC"/src/*.c; do
        [ -f "$f" ] || continue
        case "$(basename "$f")" in
            dump_modes.c|*_demo.c|opus_compare.c|test_*.c) continue ;;
            *) OPUS_C_FILES+=("${f#$OPUS_SRC/}") ;;
        esac
    done
    for src_rel in "${OPUS_C_FILES[@]}"; do
        obj_name="$(echo "$src_rel" | tr '/' '_').o"
        clang $OPUS_CFLAGS -c "$OPUS_SRC/$src_rel" -o "$OPUS_OBJ_DIR/$obj_name"
    done
    touch "$OPUS_OBJ_DIR/.opus_built_ok"
fi

# Always strip any standalone demo/test object files (which define main()) from cached opus_objs
rm -f "$OPUS_OBJ_DIR"/*demo*.o "$OPUS_OBJ_DIR"/*compare*.o "$OPUS_OBJ_DIR"/*test*.o "$OPUS_OBJ_DIR"/celt_dump_modes.c.o 2>/dev/null || true

clang --target=x86_64-pc-freebsd12-elf -fPIC -O3 -msse4.1 -ffast-math -w \
    -isysroot "$OO_PS4_TOOLCHAIN" \
    -isystem "$OO_PS4_TOOLCHAIN/include" \
    -c "$SCRIPT_DIR/stb_vorbis.c" -o "$BUILD_DIR/stb_vorbis.o"

rm -f "$BUILD_DIR/ps4_main.o" "$BUILD_DIR/hero.elf" "$BUILD_DIR/hero.oelf"

clang++ --target=x86_64-pc-freebsd12-elf -std=c++17 -fPIC -O3 -msse4.1 -ffast-math -fno-exceptions -fno-rtti \
    -DBUILD_VERSION_STR="\"v${BUILD_VER}\"" \
    -DBUILD_FULL_STR="\"BUILD v${BUILD_VER}\"" \
    -isysroot "$OO_PS4_TOOLCHAIN" \
    -isystem "$OO_PS4_TOOLCHAIN/include/c++/v1" \
    -isystem "$OO_PS4_TOOLCHAIN/include" \
    -I"$OPUS_SRC/include" \
    -I"$SCRIPT_DIR" \
    -c "$SCRIPT_DIR/ps4_main.cpp" -o "$BUILD_DIR/ps4_main.o"

echo "=== [4/7] Linking Orbis ELF & Generating eboot.bin ==="
ld.lld -m elf_x86_64 -pie --script "$OO_PS4_TOOLCHAIN/link.x" --eh-frame-hdr \
    -o "$BUILD_DIR/hero.elf" \
    -L"$OO_PS4_TOOLCHAIN/lib" \
    "$OO_PS4_TOOLCHAIN/lib/crt1.o" \
    "$BUILD_DIR/ps4_main.o" \
    "$BUILD_DIR/stb_vorbis.o" \
    "$OPUS_OBJ_DIR/"*.o \
    -lc++ -lc -lkernel -lSceVideoOut -lSceAudioOut -lScePad -lSceUserService -lSceSystemService

CREATE_FSELF="$OO_PS4_TOOLCHAIN/bin/linux/create-fself"
[ ! -x "$CREATE_FSELF" ] && CREATE_FSELF="$(command -v create-fself)"
"$CREATE_FSELF" -in="$BUILD_DIR/hero.elf" -out="$BUILD_DIR/hero.oelf" --eboot "$PKG_ROOT/eboot.bin" --paid 0x3800000000000011

if [ -f "$SCRIPT_DIR/eboot.bin" ] && [ ! -s "$PKG_ROOT/eboot.bin" ]; then
    mv -f "$SCRIPT_DIR/eboot.bin" "$PKG_ROOT/eboot.bin"
fi

if [ ! -s "$PKG_ROOT/eboot.bin" ]; then
    echo "ERROR: Failed to create valid $PKG_ROOT/eboot.bin!"
    exit 1
fi
echo "Generated Signed fSELF: $PKG_ROOT/eboot.bin ($(ls -lh "$PKG_ROOT/eboot.bin" | awk '{print $5}'))"


echo "=== [5/7] Staging OpenOrbis Runtime Modules (sce_module), Valid PNG Assets & param.sfo ==="
mkdir -p "$PKG_ROOT/sce_module" "$PKG_ROOT/sce_sys"

# Stage OpenOrbis runtime PRX modules (libc.prx, libSceFios2.prx) required by Orbis OS dynamic linker
for mod in libc.prx libSceFios2.prx; do
    MOD_SRC=""
    for cand in \
        "$OO_PS4_TOOLCHAIN/bin/data/modules/$mod" \
        "$OO_PS4_TOOLCHAIN/data/modules/$mod" \
        "$OO_PS4_TOOLCHAIN/modules/$mod" \
        "$OO_PS4_TOOLCHAIN/lib/$mod"; do
        if [ -f "$cand" ]; then
            MOD_SRC="$cand"
            break
        fi
    done
    if [ -z "$MOD_SRC" ]; then
        MOD_SRC="$(find "$OO_PS4_TOOLCHAIN" -name "$mod" 2>/dev/null | head -n 1 || true)"
    fi
    if [ -n "$MOD_SRC" ] && [ -f "$MOD_SRC" ]; then
        cp -f "$MOD_SRC" "$PKG_ROOT/sce_module/$mod"
        echo "  [+] Staged $mod from: $MOD_SRC"
    else
        touch "$PKG_ROOT/sce_module/$mod"
        echo "  [!] Warning: $mod not found in toolchain, created empty stub"
    fi
done

if [ -f "$SCRIPT_DIR/icon0.png" ]; then
    cp -f "$SCRIPT_DIR/icon0.png" "$PKG_ROOT/sce_sys/icon0.png"
    echo "  [+] Staged custom icon0.png ($(ls -lh "$SCRIPT_DIR/icon0.png" | awk '{print $5}'))"
fi
if [ -f "$SCRIPT_DIR/pic1.png" ]; then
    cp -f "$SCRIPT_DIR/pic1.png" "$PKG_ROOT/sce_sys/pic1.png"
    echo "  [+] Staged custom pic1.png ($(ls -lh "$SCRIPT_DIR/pic1.png" | awk '{print $5}'))"
fi

python3 - "$PKG_ROOT" << 'EOF_PNG'
import struct, zlib, sys, os

pkg_root = sys.argv[1]
sce_sys = os.path.join(pkg_root, "sce_sys")
os.makedirs(sce_sys, exist_ok=True)

def make_png(width, height, r, g, b):
    ihdr_data = struct.pack('>IIBBBBB', width, height, 8, 2, 0, 0, 0)
    ihdr_chunk = b'IHDR' + ihdr_data
    ihdr_crc = struct.pack('>I', zlib.crc32(ihdr_chunk))
    row = b'\x00' + bytes([r, g, b]) * width
    compressed = zlib.compress(row * height, 6)
    idat_chunk = b'IDAT' + compressed
    idat_crc = struct.pack('>I', zlib.crc32(idat_chunk))
    iend_chunk = b'IEND'
    iend_crc = struct.pack('>I', zlib.crc32(iend_chunk))
    return (
        b'\x89PNG\r\n\x1a\n'
        + struct.pack('>I', len(ihdr_data)) + ihdr_chunk + ihdr_crc
        + struct.pack('>I', len(compressed)) + idat_chunk + idat_crc
        + struct.pack('>I', 0) + iend_chunk + iend_crc
    )

icon_dst = os.path.join(sce_sys, "icon0.png")
if not os.path.exists(icon_dst):
    with open(icon_dst, 'wb') as f:
        f.write(make_png(512, 512, 32, 24, 18))

pic_dst = os.path.join(sce_sys, "pic1.png")
if not os.path.exists(pic_dst):
    with open(pic_dst, 'wb') as f:
        f.write(make_png(1920, 1080, 16, 20, 28))
EOF_PNG


# Locate PkgTool.Core early for both SFO generation and final PKG building
PKG_TOOL=""
for cand in "$OO_PS4_TOOLCHAIN/bin/linux/PkgTool.Core" "$(command -v PkgTool.Core 2>/dev/null || true)"; do
    if [ -n "$cand" ] && [ -x "$cand" ]; then
        PKG_TOOL="$cand"
        break
    fi
done
export DOTNET_SYSTYPE_DISABLE=1

# Generate param.sfo using native PkgTool.Core if available (ensures 100% Orbis OS compliance)
SFO_PATH="$PKG_ROOT/sce_sys/param.sfo"
if [ -n "${PKG_TOOL:-}" ] && [ -x "$PKG_TOOL" ]; then
    rm -f "$SFO_PATH"
    "$PKG_TOOL" sfo_new "$SFO_PATH"
    "$PKG_TOOL" sfo_setentry "$SFO_PATH" APP_TYPE --type Integer --maxsize 4 --value 1
    "$PKG_TOOL" sfo_setentry "$SFO_PATH" APP_VER --type Utf8 --maxsize 8 --value "$BUILD_VER_SFO"
    "$PKG_TOOL" sfo_setentry "$SFO_PATH" ATTRIBUTE --type Integer --maxsize 4 --value 0
    "$PKG_TOOL" sfo_setentry "$SFO_PATH" CATEGORY --type Utf8 --maxsize 4 --value "gd"
    "$PKG_TOOL" sfo_setentry "$SFO_PATH" CONTENT_ID --type Utf8 --maxsize 48 --value "IV0000-BREW00100_00-GUITARHERO000000"
    "$PKG_TOOL" sfo_setentry "$SFO_PATH" DOWNLOAD_DATA_SIZE --type Integer --maxsize 4 --value 0
    "$PKG_TOOL" sfo_setentry "$SFO_PATH" SYSTEM_VER --type Integer --maxsize 4 --value 0
    "$PKG_TOOL" sfo_setentry "$SFO_PATH" TITLE --type Utf8 --maxsize 128 --value "Clone Hero PS4"
    "$PKG_TOOL" sfo_setentry "$SFO_PATH" TITLE_ID --type Utf8 --maxsize 12 --value "BREW00100"
    "$PKG_TOOL" sfo_setentry "$SFO_PATH" VERSION --type Utf8 --maxsize 8 --value "$BUILD_VER_SFO"
else
    # Python fallback using standard Sony PSF format flags
    python3 - "$PKG_ROOT" "$BUILD_VER" "$BUILD_VER_SFO" << 'EOF_SFO'
import struct, sys, os
pkg_root, build_ver, build_ver_sfo = sys.argv[1], sys.argv[2], sys.argv[3]
sce_sys = os.path.join(pkg_root, "sce_sys")
entries = [
    ("APP_TYPE",        0x0404, 4,   struct.pack('<I', 1)),
    ("APP_VER",         0x0204, 8,   build_ver_sfo.encode('ascii')[:7] + b"\x00"),
    ("ATTRIBUTE",       0x0404, 4,   struct.pack('<I', 0)),
    ("CATEGORY",        0x0204, 4,   b"gd\x00"),
    ("CONTENT_ID",      0x0204, 48,  b"IV0000-BREW00100_00-GUITARHERO000000\x00"),
    ("DOWNLOAD_DATA_SIZE", 0x0404, 4, struct.pack('<I', 0)),
    ("SYSTEM_VER",      0x0404, 4,   struct.pack('<I', 0)),
    ("TITLE",           0x0204, 128, b"Clone Hero PS4\x00"),
    ("TITLE_ID",        0x0204, 12,  b"BREW00100\x00"),
    ("VERSION",         0x0204, 8,   build_ver_sfo.encode('ascii')[:7] + b"\x00"),
]
entries.sort(key=lambda x: x[0])
key_table = bytearray()
data_table = bytearray()
index_table = bytearray()
for key, fmt, max_len, val in entries:
    key_offset = len(key_table)
    key_table.extend(key.encode('ascii') + b'\x00')
    data_offset = len(data_table)
    if fmt == 0x0204:
        val_len = len(val)
        padded = val + b'\x00' * (max_len - val_len)
    else:
        val_len = 4
        padded = val
    data_table.extend(padded)
    index_table.extend(struct.pack('<HHIII', key_offset, fmt, val_len, max_len, data_offset))
while len(key_table) % 4 != 0: key_table.append(0)
header_size = 20
key_table_offset = header_size + len(index_table)
data_table_offset = key_table_offset + len(key_table)
sfo_header = struct.pack('<4sIIII', b'\x00PSF', 0x00000101, key_table_offset, data_table_offset, len(entries))
with open(os.path.join(sce_sys, "param.sfo"), 'wb') as f:
    f.write(sfo_header + index_table + key_table + data_table)
EOF_SFO
fi

echo "=== [6/7] Staging Minimal Embedded Assets (21 Highways + Bundled Fallback Song) ==="
if [ -f "$SCRIPT_DIR/notes.mid" ] && [ -f "$SCRIPT_DIR/song.ogg" ]; then
    cp -f "$SCRIPT_DIR/notes.mid" "$PKG_ROOT/assets/song/notes.mid"
    cp -f "$SCRIPT_DIR/song.ogg"  "$PKG_ROOT/assets/song/song.ogg"
    [ -f "$SCRIPT_DIR/song.ini" ] && cp -f "$SCRIPT_DIR/song.ini" "$PKG_ROOT/assets/song/song.ini"
fi

echo "=== [7/7] Building Final Installable PS4 Package (${PKG_FILENAME}) ==="
GP4_PATH="$BUILD_DIR/gp4_project.gp4"

python3 - "$PKG_ROOT" "$GP4_PATH" << 'EOF_GP4'
import os, sys
from datetime import datetime

pkg_root = sys.argv[1]
gp4_out  = sys.argv[2]
ts = datetime.now().strftime("%Y-%m-%d %H:%M:%S")

files_xml = []
for root, _, filenames in os.walk(pkg_root):
    for fn in sorted(filenames):
        if fn.endswith('.gp4'): continue
        full = os.path.abspath(os.path.join(root, fn)).replace("\\", "/")
        rel = os.path.relpath(full, pkg_root).replace("\\", "/")
        files_xml.append(f'    <file targ_path="{rel}" orig_path="{full}"/>')

gp4 = f'''<?xml version="1.0" encoding="utf-8" standalone="yes"?>
<psproject fmt="gp4" version="1000">
  <volume>
    <volume_type>pkg_ps4_app</volume_type>
    <volume_id>PS4VOLUME</volume_id>
    <volume_ts>{ts}</volume_ts>
    <package content_id="IV0000-BREW00100_00-GUITARHERO000000" passcode="00000000000000000000000000000000" storage_type="digital50" app_type="full"/>
    <chunk_info chunk_count="1" scenario_count="1">
      <chunks>
        <chunk id="0" layer_no="0" label="Chunk #0"/>
      </chunks>
      <scenarios default_id="0">
        <scenario id="0" type="sp" initial_chunk_count="1" label="Scenario #0">0</scenario>
      </scenarios>
    </chunk_info>
  </volume>
  <files img_no="0">
{chr(10).join(files_xml)}
  </files>
  <rootdir>
    <dir targ_name="sce_sys"/>
    <dir targ_name="sce_module"/>
    <dir targ_name="assets">
      <dir targ_name="song"/>
      <dir targ_name="highways"/>
    </dir>
  </rootdir>
</psproject>
'''
with open(gp4_out, 'w', encoding='utf-8') as f:
    f.write(gp4)
EOF_GP4


PKG_TOOL=""
for cand in "$OO_PS4_TOOLCHAIN/bin/linux/PkgTool.Core" "$(command -v PkgTool.Core 2>/dev/null || true)"; do
    if [ -n "$cand" ] && [ -x "$cand" ]; then
        PKG_TOOL="$cand"
        break
    fi
done

export DOTNET_SYSTYPE_DISABLE=1
"$PKG_TOOL" pkg_build "$GP4_PATH" "$SCRIPT_DIR"

# Rename generated IV0000-BREW00100_00-GUITARHERO000000.pkg to versioned hero_vX.XX.pkg + hero.pkg
if [ -f "$SCRIPT_DIR/IV0000-BREW00100_00-GUITARHERO000000.pkg" ]; then
    mv -f "$SCRIPT_DIR/IV0000-BREW00100_00-GUITARHERO000000.pkg" "$SCRIPT_DIR/$PKG_FILENAME"
    cp -f "$SCRIPT_DIR/$PKG_FILENAME" "$SCRIPT_DIR/hero.pkg"
fi

# Bump .build_version for the next build!
echo "$NEXT_BUILD_VER" > "$VERSION_FILE"

echo "=================================================================="
echo "SUCCESS! Created Versioned PS4 PKG: $SCRIPT_DIR/$PKG_FILENAME"
echo "         (Also copied to:           $SCRIPT_DIR/hero.pkg)"
echo "         Next build will auto-bump to: v${NEXT_BUILD_VER}"
ls -lh "$SCRIPT_DIR/$PKG_FILENAME"
echo "=================================================================="
echo "1. Install $PKG_FILENAME on your PS4 via GoldHEN Debug Settings -> Package Installer."
echo "2. All audio formats (.opus, .ogg, .mp3, .flac, .wav) play natively on PS4!"
echo "3. Place default background .mp4 videos in /data/GH_backgrounds/ on your PS4."
echo "=================================================================="
