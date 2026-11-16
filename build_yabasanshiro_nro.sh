#!/bin/bash
set -e

# Setup paths
SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
YABA_DIR="$SCRIPT_DIR"
BUILD_DIR="$YABA_DIR/build_tico"

# shaderc (GLSL→SPIR-V, used by the VIDVulkan renderer at runtime).
# Cross-compiled once with devkitA64 and cached inside the project tree so
# ephemeral docker runs reuse it.
SHADERC_PREFIX="${SWITCH_SHADERC_DIR:-$YABA_DIR/.cache/shaderc-switch}"
SHADERC_TAG="${SHADERC_TAG:-v2024.3}"

echo "=== Building YabaSanshiro (OpenGL + Vulkan) for Switch ==="
echo "Source: $YABA_DIR"
echo "Build:  $BUILD_DIR"
echo "shaderc: $SHADERC_PREFIX"

# ============================================================
# Build shaderc for Switch (once, cached in .cache/)
# ============================================================
if [ ! -f "$SHADERC_PREFIX/lib/libshaderc_combined.a" ]; then
    echo "=== shaderc not found — cross-compiling $SHADERC_TAG with devkitA64 ==="

    command -v git >/dev/null || { echo "ERROR: git is required to fetch shaderc"; exit 1; }
    if ! command -v python3 >/dev/null; then
        echo "python3 missing — attempting pacman install..."
        pacman -S --noconfirm python || { echo "ERROR: python3 is required (shaderc dependency sync + SPIRV-Tools codegen)"; exit 1; }
    fi

    SHADERC_SRC="$YABA_DIR/.cache/shaderc-src"
    SHADERC_BUILD="$YABA_DIR/.cache/shaderc-build"
    mkdir -p "$YABA_DIR/.cache"

    if [ ! -d "$SHADERC_SRC/.git" ]; then
        git clone --depth 1 --branch "$SHADERC_TAG" https://github.com/google/shaderc "$SHADERC_SRC"
    fi
    (cd "$SHADERC_SRC" && ./utils/git-sync-deps)

    # SPIRV-Tools rejects unknown platforms ("NintendoSwitch" from the devkitPro
    # toolchain). Teach it about ours — newlib is close enough to Linux for the
    # bits these defines gate (console printing; timers are disabled below).
    SPVT_CMAKE="$SHADERC_SRC/third_party/spirv-tools/CMakeLists.txt"
    if ! grep -q 'NintendoSwitch' "$SPVT_CMAKE"; then
        python3 - "$SPVT_CMAKE" <<'PYEOF'
import sys
p = sys.argv[1]
src = open(p).read()
needle = 'else()\n  message(FATAL_ERROR "Your platform'
patch = ('elseif("${CMAKE_SYSTEM_NAME}" STREQUAL "NintendoSwitch")\n'
         '  add_definitions(-DSPIRV_LINUX)\n'
         + needle)
if needle not in src:
    sys.exit("SPIRV-Tools platform check not found — update the patch in build_yabasanshiro_nro.sh")
open(p, "w").write(src.replace(needle, patch, 1))
print("Patched SPIRV-Tools platform check for NintendoSwitch")
PYEOF
    fi

    SWITCH_TOOLCHAIN="$DEVKITPRO/cmake/Switch.cmake"
    if [ ! -f "$SWITCH_TOOLCHAIN" ]; then
        SWITCH_TOOLCHAIN="$YABA_DIR/yabause/src/nx/nx-toolchain.cmake"
    fi

    rm -rf "$SHADERC_BUILD"
    mkdir -p "$SHADERC_BUILD"
    cmake -S "$SHADERC_SRC" -B "$SHADERC_BUILD" \
        -DCMAKE_TOOLCHAIN_FILE="$SWITCH_TOOLCHAIN" \
        -DCMAKE_BUILD_TYPE=Release \
        -DSHADERC_SKIP_TESTS=ON \
        -DSHADERC_SKIP_EXAMPLES=ON \
        -DSHADERC_SKIP_COPYRIGHT_CHECK=ON \
        -DENABLE_GLSLANG_BINARIES=OFF \
        -DSPIRV_SKIP_EXECUTABLES=ON \
        -DSPIRV_SKIP_TESTS=ON \
        -DSPIRV_ALLOW_TIMERS=OFF \
        -DSPIRV_WERROR=OFF \
        -DSHADERC_ENABLE_WERROR_COMPILE=OFF
    cmake --build "$SHADERC_BUILD" --target shaderc_combined -j"$(nproc)"

    mkdir -p "$SHADERC_PREFIX/lib" "$SHADERC_PREFIX/include"
    cp "$SHADERC_BUILD/libshaderc/libshaderc_combined.a" "$SHADERC_PREFIX/lib/"
    cp -r "$SHADERC_SRC/libshaderc/include/shaderc" "$SHADERC_PREFIX/include/"
    echo "=== shaderc installed to $SHADERC_PREFIX ==="
fi

if [ ! -d "$YABA_DIR" ]; then
    echo "Error: YabaSanshiro2 source directory not found at $YABA_DIR"
    exit 1
fi

# # --- Patched libdrm_nouveau: pre-clear stale Uncached memory attributes ---
# # Replaces the stock portlib with our patched version that prevents
# # Data Abort in armDCacheFlush when heap pages carry stale Uncached attrs
# # from a previous NRO's leaked GPU buffer objects.
# PATCHED_DRM="/project/for_reference/libdrm_nouveau/lib/libdrm_nouveau.a"
# PORTLIB_DRM="/opt/devkitpro/portlibs/switch/lib/libdrm_nouveau.a"
# if [ -f "$PATCHED_DRM" ]; then
#     echo "Installing patched libdrm_nouveau.a over portlib..."
#     cp -f "$PATCHED_DRM" "$PORTLIB_DRM"
#     echo "Done — patched libdrm_nouveau installed"
# else
#     echo "WARNING: Patched libdrm_nouveau.a not found at $PATCHED_DRM"
#     echo "         Using stock portlib (stale-attr crash fix NOT applied)"
# fi

# Clean and create build directory
rm -rf "$BUILD_DIR"
mkdir -p "$BUILD_DIR"
cd "$BUILD_DIR"

# Configure for Switch
# WORKAROUND: conflicting zlib and libpng targets
sed -i 's/include( ..\/glfw\/external_zlib.cmake )/#include( ..\/glfw\/external_zlib.cmake )/g' "$YABA_DIR/yabause/src/nx/CMakeLists.txt"
sed -i 's/include( ..\/glfw\/external_libpng.cmake )/#include( ..\/glfw\/external_libpng.cmake )/g' "$YABA_DIR/yabause/src/nx/CMakeLists.txt"

# WORKAROUND: remove invalid -mtp=soft flag from toolchain for aarch64
sed -i 's/-mtp=soft//g' "$YABA_DIR/yabause/src/nx/nx-toolchain.cmake"

# WORKAROUND: enable exceptions
sed -i 's/-fno-exceptions//g' "$YABA_DIR/yabause/src/nx/nx-toolchain.cmake"

# WORKAROUND: exclude cd-web.cpp (requires ifaddrs.h which is missing on Switch)
sed -i 's/^\([[:space:]]*\)cd-web.cpp/\1#cd-web.cpp/' "$YABA_DIR/yabause/src/CMakeLists.txt"

# WORKAROUND: exclude PlayRecorder.cpp (causes linker errors with zlib and threading)
sed -i 's/^\([[:space:]]*\)PlayRecorder.cpp/\1#PlayRecorder.cpp/' "$YABA_DIR/yabause/src/CMakeLists.txt"

# Using the nx-toolchain.cmake from the repository
cmake "$YABA_DIR/yabause" \
    -Wno-dev \
    -DCMAKE_TOOLCHAIN_FILE="$YABA_DIR/yabause/src/nx/nx-toolchain.cmake" \
    -DPLATFORM=libnx \
    -DNINTENDO_SWITCH=ON \
    -DYAB_PORTS=nx \
    -DCMAKE_BUILD_TYPE=Release \
    -DDEVKITPRO=/opt/devkitpro \
    -DYAB_WANT_VULKAN=ON \
    -DYAB_WANT_OPENGL=ON \
    -DSWITCH_SHADERC_DIR="$SHADERC_PREFIX" \
    -DHAVE_LIBSDL=1 \
    -DYAB_WANT_DYNAREC_DEVMIYAX=TRUE \
    -DDYNAREC_DEVMIYAX=TRUE \
    -DSH2_DYNAREC=FALSE \
    -DAARCH64=TRUE \
    -DUSE_AARCH64_DRC=TRUE \
    -DCMAKE_SYSTEM_PROCESSOR=aarch64 \
    -DYAB_WANT_MUSASHI=1 \
    -DYAB_WANT_SCSP=1 \
    -DHAVE_FLOORF=1 \
    -DYAB_WANT_SH2_CACHE=1 \
    -DHAVE_NEON=1 \
    -DYAB_ASYNC_RENDERING=1 \
    -DDISABLE_LOGGING=1 \
    -DCMAKE_C_FLAGS="-DDISABLE_LOGGING=1 -DHAVE_LIBCHDR=1 -DHAVE_LIBSDL=1 -DYAB_ASYNC_RENDERING=1 -DYAB_WANT_SH2_CACHE=1 -DHAVE_FLOORF=1 -DHAVE_NEON=1 -D__ARM_NEON__ -DYAB_WANT_SCSP=1 -DYAB_WANT_MUSASHI=1 -DDYNAREC_DEVMIYAX=1 -DUSE_AARCH64_DRC=1 -DAARCH64=1 -I/opt/devkitpro/portlibs/switch/include/SDL2 -mcpu=cortex-a57 -mtune=cortex-a57 -ffast-math -flto" \
    -DCMAKE_CXX_FLAGS="-DDISABLE_LOGGING=1 -DHAVE_LIBCHDR=1 -DHAVE_LIBSDL=1 -DYAB_ASYNC_RENDERING=1 -DYAB_WANT_SH2_CACHE=1 -DHAVE_FLOORF=1 -DHAVE_NEON=1 -D__ARM_NEON__ -DYAB_WANT_SCSP=1 -DYAB_WANT_MUSASHI=1 -DDYNAREC_DEVMIYAX=1 -DUSE_AARCH64_DRC=1 -DAARCH64=1 -I/opt/devkitpro/portlibs/switch/include/SDL2 -mcpu=cortex-a57 -mtune=cortex-a57 -ffast-math -flto"

# Build
echo "Running Make..."
make -j$(nproc)

echo ""
echo "=== Build Complete ==="

# ============================================================
# Generate NRO with romfs directly (don't rely on POST_BUILD)
# This matches how flycast/tico do it: elf2nro with --nacp + --romfsdir
# ============================================================
ROMFS_DIR="$YABA_DIR/yabause/src/nx/romfs"
NRO_OUT="$SCRIPT_DIR/tico-yabasanshiro.nro"
APP_VERSION="3.1.0"
# the homebrew menu wants a 256x256 JPEG; another size breaks the NRO
ICON_FILE="$YABA_DIR/yabause/src/nx/icon.jpg"

# Find the ELF binary (CMake target yabause-nx with OUTPUT_NAME yabasanshiro)
ELF_FILE=""
for candidate in \
    "$BUILD_DIR/src/nx/yabasanshiro" \
    "$BUILD_DIR/src/nx/yabasanshiro.elf" \
    "$BUILD_DIR/src/nx/yabause-nx"; do
    if [ -f "$candidate" ]; then
        ELF_FILE="$candidate"
        break
    fi
done

if [ -z "$ELF_FILE" ]; then
    echo "ERROR: Could not find ELF binary in $BUILD_DIR/src/nx/"
    ls -la "$BUILD_DIR/src/nx/" || true
    exit 1
fi
echo "Found ELF: $ELF_FILE"

# Generate NACP metadata (required for proper ASET section)
NACP_FILE="$BUILD_DIR/yabasanshiro.nacp"
nacptool --create "tico-yabasanshiro" "devmiyax/ticoverse.com" "$APP_VERSION" "$NACP_FILE"
echo "Generated NACP: $NACP_FILE"

# Build elf2nro arguments
ELF2NRO_ARGS="--nacp=$NACP_FILE --icon=$ICON_FILE"

# Add romfs if directory exists and has files
if [ -d "$ROMFS_DIR" ] && [ -n "$(find "$ROMFS_DIR" -type f 2>/dev/null | head -1)" ]; then
    echo "Embedding romfs from: $ROMFS_DIR"
    find "$ROMFS_DIR" -type f | while read f; do echo "  romfs: $f"; done
    ELF2NRO_ARGS="$ELF2NRO_ARGS --romfsdir=$ROMFS_DIR"
else
    echo "WARNING: No romfs directory or empty at $ROMFS_DIR — NRO will have no embedded assets"
fi

# Generate NRO
echo "Running: elf2nro $ELF_FILE $NRO_OUT $ELF2NRO_ARGS"
elf2nro "$ELF_FILE" "$NRO_OUT" $ELF2NRO_ARGS

if [ -f "$NRO_OUT" ]; then
    NRO_SIZE=$(stat -c%s "$NRO_OUT" 2>/dev/null || stat -f%z "$NRO_OUT" 2>/dev/null)
    echo "SUCCESS: $NRO_OUT generated ($NRO_SIZE bytes)"
else
    echo "ERROR: elf2nro failed to produce $NRO_OUT"
    exit 1
fi

#---------------------------------------------------------------------------------
# Module bundle
#
# tico discovers a core by reading module.json; the settings definition and the
# strings that label it travel with the NRO in the same directory. The bundle
# extracts straight into sdmc:/tico/modules/<id>/.
#---------------------------------------------------------------------------------
MODULE_SRC="$YABA_DIR/yabause/src/nx/tico/module"
MODULE_ID=$(sed -n 's/.*"id"[[:space:]]*:[[:space:]]*"\([^"]*\)".*/\1/p' "$MODULE_SRC/module.json" | head -1)
MODULE_NRO=$(sed -n 's/.*"nro"[[:space:]]*:[[:space:]]*"\([^"]*\)".*/\1/p' "$MODULE_SRC/module.json" | head -1)
MODULE_OUT="$BUILD_DIR/module/$MODULE_ID"

rm -rf "$BUILD_DIR/module"
mkdir -p "$MODULE_OUT"
cp -r "$MODULE_SRC/." "$MODULE_OUT/"
cp "$NRO_OUT" "$MODULE_OUT/$MODULE_NRO"

# Tico prefers .json.gz when resolving a gamelist.
if [ -d "$MODULE_OUT/gamelists" ]; then
    gzip -f -9 "$MODULE_OUT"/gamelists/*.json 2>/dev/null || true
fi

BUNDLE="$BUILD_DIR/tico-$MODULE_ID-module.zip"
rm -f "$BUNDLE"
( cd "$BUILD_DIR/module" && zip -qr "$BUNDLE" "$MODULE_ID" )
cp "$BUNDLE" "$SCRIPT_DIR/"

echo "======================================"
echo "Module: $SCRIPT_DIR/$(basename "$BUNDLE")"
echo "        extracts to sdmc:/tico/modules/$MODULE_ID/"
echo "======================================"
find "$MODULE_OUT" -type f | sed "s|$BUILD_DIR/module/|    |"
