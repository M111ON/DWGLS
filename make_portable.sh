#!/bin/bash
# DWGLS Portable Package Builder
# Usage: bash make_portable.sh
#
# Output: DWGLS-portable-<date>.zip
# Structure:
#   DWGLS-portable/
#   ├── bin/          (compiled binaries)
#   ├── dll/          (required DLLs)
#   ├── models/       (user places .gguf here)
#   ├── config.json   (runtime config)
#   └── README.txt

set -e

ROOT="I:/DWGLS-native-fs"
BUILD="$ROOT/build"
PKG="$ROOT/DWGLS-portable"
CC="gcc"
CFLAGS="-O2 -std=c11 -Wno-unused-parameter -Wno-sign-compare -Wno-format -I. -Icore -Icore/infra -II:/llama/include"
LDFLAGS="-lm"

# DLL paths
LLAMA_DLL="I:/llama/llama-b9733-bin-win-vulkan-x64"
MINGW_DLL="C:/msys64/mingw64/bin"

echo "=== DWGLS Portable Package Builder ==="
echo ""

# Clean and create package directory
rm -rf "$PKG"
mkdir -p "$PKG/bin" "$PKG/dll" "$PKG/models"

# ── Step 1: Build key binaries ──────────────────────────────
echo "--- Building binaries ---"

# Array of: name:extra_sources:needs_ggml
# extra_sources: colon-separated list of additional .c files
# needs_ggml: 1 if tool uses ggml_backend_load_all_from_path, 0 otherwise
TOOLS=(
    "kv_flow_demo:core/infra/config.c:core/infra/cJSON.c:0"
    "kv_rid_serve::1"
    "geofs_rid::0"
    "kv_geo_addr_test::0"
    "kv_impact_test::0"
    "kv_container_test::0"
    "kv_encode_test::0"
    "gguf_roundtrip::0"
)

for entry in "${TOOLS[@]}"; do
    name=$(echo "$entry" | cut -d: -f1)
    # Get fields 2 and 3 as extra_sources (colon-separated)
    extra_f2=$(echo "$entry" | cut -d: -f2)
    extra_f3=$(echo "$entry" | cut -d: -f3)
    extra=""
    if [ -n "$extra_f2" ]; then
        extra="$extra_f2"
    fi
    if [ -n "$extra_f3" ]; then
        extra="$extra:$extra_f3"
    fi
    needs_ggml=$(echo "$entry" | cut -d: -f4)
    src="tools/${name}.c"
    if [ ! -f "$src" ]; then
        echo "  SKIP $name (not found)"
        continue
    fi

    # Build with extra sources if any
    if [ -n "$extra" ]; then
        # Replace colons with spaces for extra sources
        extra_sources=$(echo "$extra" | tr ':' ' ')
        $CC $CFLAGS -o "$PKG/bin/${name}.exe" "$src" $extra_sources "$LLAMA_DLL/libllama.dll" -lzstd $LDFLAGS 2>/dev/null
    elif [ "$needs_ggml" = "1" ]; then
        $CC $CFLAGS -o "$PKG/bin/${name}.exe" "$src" "$LLAMA_DLL/libllama.dll" "$LLAMA_DLL/ggml.dll" -lzstd $LDFLAGS 2>/dev/null
    else
        $CC $CFLAGS -o "$PKG/bin/${name}.exe" "$src" "$LLAMA_DLL/libllama.dll" -lzstd $LDFLAGS 2>/dev/null
    fi

    if [ $? -eq 0 ]; then
        echo "  ✅ $name"
    else
        echo "  ❌ $name (build failed)"
    fi
done

# ── Step 2: Copy required DLLs ──────────────────────────────
echo ""
echo "--- Copying DLLs ---"

DLLS=(
    "$LLAMA_DLL/libllama.dll"
    "$LLAMA_DLL/llama.dll"
    "$LLAMA_DLL/ggml.dll"
    "$LLAMA_DLL/ggml-base.dll"
    "$LLAMA_DLL/ggml-cpu-x64.dll"
    "$LLAMA_DLL/ggml-vulkan.dll"
    "$LLAMA_DLL/libomp140.x86_64.dll"
    "$MINGW_DLL/libgomp-1.dll"
    "$MINGW_DLL/libgcc_s_seh-1.dll"
    "$MINGW_DLL/libstdc++-6.dll"
    "$MINGW_DLL/libwinpthread-1.dll"
)

for dll in "${DLLS[@]}"; do
    if [ -f "$dll" ]; then
        cp "$dll" "$PKG/dll/"
        echo "  ✅ $(basename $dll)"
    else
        echo "  ⚠️  $(basename $dll) not found at $dll"
    fi
done

# Create ggml-cpu.dll symlink (dispatch DLL)
if [ -f "$PKG/dll/ggml-cpu-x64.dll" ]; then
    cp "$PKG/dll/ggml-cpu-x64.dll" "$PKG/dll/ggml-cpu.dll"
    echo "  ✅ ggml-cpu.dll (dispatch)"
fi

# ── Step 3: Copy config and run scripts ──────────────────────
echo ""
echo "--- Creating config and run scripts ---"

# Config template
cp "$ROOT/config.json" "$PKG/config.json"
echo "  ✅ config.json"

# README
cat > "$PKG/README.txt" << 'EOF'
DWGLS — Data Flow Management System
====================================

Quick Start:
  1. Edit config.json (set model path)
  2. Place your .gguf model in models/
  3. Run: bin\kv_flow_demo.exe config.json

Directory Structure:
  bin/         Compiled binaries
  dll/         Required DLLs (DO NOT MODIFY)
  models/      Place .gguf model files here
  config.json  Runtime configuration (edit this)

Binaries:
  kv_flow_demo.exe     KV buffer storage demo (3 flows)
  kv_rid_serve.exe     llama STATE checkpoint/restore
  geofs_rid.exe        GeoFS ↔ RID slot region
  kv_geo_addr_test.exe Geometric addressing validation
  kv_impact_test.exe   Dead slot safety analysis
  kv_container_test.exe Container format test
  kv_encode_test.exe   KV encode/decode test
  gguf_roundtrip.exe   GGUF language roundtrip

Requirements:
  - Windows 10/11 x64
  - No additional installation needed

For more info: https://github.com/M111ON/DWGLS
EOF
echo "  ✅ README.txt"

# run.bat helper
cat > "$PKG/run.bat" << 'EOF'
@echo off
set PATH=%~dp0dll;%PATH%
cd /d %~dp0
echo DWGLS Portable — KV Flow Demo
echo ============================
echo.
bin\kv_flow_demo.exe config.json
echo.
pause
EOF
echo "  ✅ run.bat"

# ── Step 4: Create zip ──────────────────────────────────────
echo ""
echo "--- Creating ZIP ---"

DATE=$(date +%Y%m%d)
ZIPNAME="DWGLS-portable-${DATE}.zip"
rm -f "$ROOT/$ZIPNAME"

# Use PowerShell to create zip (more reliable on Windows)
cd "$ROOT"
powershell -Command "Compress-Archive -Path 'DWGLS-portable' -DestinationPath '$ZIPNAME' -Force"

if [ -f "$ROOT/$ZIPNAME" ]; then
    SIZE=$(du -h "$ROOT/$ZIPNAME" | cut -f1)
    echo "  ✅ $ZIPNAME ($SIZE)"
else
    echo "  ❌ ZIP creation failed"
fi

# ── Summary ──────────────────────────────────────────────────
echo ""
echo "=== Package Complete ==="
echo "Location: $ROOT/$ZIPNAME"
echo ""
echo "To use on another machine:"
echo "  1. Extract ZIP"
echo "  2. Edit config.json (set model path)"
echo "  3. Place .gguf model in models/"
echo "  4. Double-click run.bat"
