#!/bin/bash
# DWGLS Integration Test — Colab T4
# Phase 1: gguf_roundtrip (6-view RID storage)
# Phase 2: Baseline inference (llama-cpp-python)
# Phase 3: geo_rid_graft (RID + llama.cpp inference)
set -e
export PATH="$HOME/.local/bin:$PATH"
cd "$(dirname "$0")"
S=dwgls-integ

# Create session with retries
for i in 1 2 3; do
    if colab new -s $S --gpu T4 2>/dev/null; then break; fi
    echo "attempt $i failed, waiting 45s..."
    sleep 45
    [ $i -eq 3 ] && { echo "T4 unavailable"; exit 1; }
done

# Upload DWGLS source files
echo "=== uploading DWGLS source ==="
colab exec -s $S --timeout 30 -f /dev/stdin << 'SETUP'
import os
os.makedirs("/content/core/infra", exist_ok=True)
os.makedirs("/content/tools/core/infra", exist_ok=True)
print("dirs ready")
SETUP

colab upload -s $S /mnt/i/DWGLS-native-fs/tools/gguf_roundtrip.c /content/tools/gguf_roundtrip.c
colab upload -s $S /mnt/i/DWGLS-native-fs/tools/geo_rid_graft.c /content/tools/geo_rid_graft.c
colab upload -s $S /mnt/i/DWGLS-native-fs/core/gguf_reader.h /content/core/gguf_reader.h
colab upload -s $S /mnt/i/DWGLS-native-fs/core/infra/dramtile_store.h /content/core/infra/dramtile_store.h
colab upload -s $S /mnt/i/DWGLS-native-fs/core/infra/geo_dram_tile.h /content/core/infra/geo_dram_tile.h

echo "=== source uploaded ==="
cp dwgls_integ_test.py /tmp/dwgls_integ_test.py
colab upload -s $S /tmp/dwgls_integ_test.py /tmp/dwgls_integ_test.py
colab exec -s $S --timeout 5400 -f /tmp/dwgls_integ_test.py
colab stop -s $S
