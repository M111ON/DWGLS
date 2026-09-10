#!/bin/bash
# deploy_scatter_decode.sh — Deploy tess_scatter_decode to Colab T4
# Usage: bash deploy_scatter_decode.sh [tesspack_file] [--verify]
# Requires: colab-cli, colab session "dwgls-gpu5"
set -e

S=dwgls-gpu5
TPACK="${1:-I:/model/qwen3-0.6b-q8_0.tesspack}"
VERIFY="${2:---verify}"

echo "=== Scatter Decode — Colab T4 Deploy ==="

# ── 1. Session setup ──
for i in 1 2 3; do
  if colab new -s $S --gpu T4 2>/dev/null; then break; fi
  echo "attempt $i failed, waiting 45s..."
  sleep 45
  [ $i -eq 3 ] && { echo "T4 unavailable after retries"; exit 1; }
done

# ── 2. Upload source ──
echo "=== uploading scatter decode ==="
colab exec -s $S --timeout 30 -f /dev/stdin << 'SETUP'
import os
os.makedirs("/content/scatter", exist_ok=True)
print("dir ready")
SETUP

colab upload -s $S I:/DWGLS-native-fs/bench/tess_scatter_decode.cu /content/scatter/

# ── 3. Compile on Colab ──
echo "=== compiling ==="
colab exec -s $S --timeout 120 -f /dev/stdin << 'BUILD'
!cd /content/scatter && nvcc -O3 -std=c++17 -arch=sm_75 -o tess_scatter_decode tess_scatter_decode.cu -lm
!ls -la /content/scatter/tess_scatter_decode
BUILD

# ── 4. Upload tesspack ──
echo "=== uploading tesspack ==="
colab upload -s $S "$TPACK" /content/scatter/model.tesspack

# ── 5. Run decode + verify ──
echo "=== running decode ==="
colab exec -s $S --timeout 600 -f /dev/stdin << RUN
!cd /content/scatter && ./tess_scatter_decode model.tesspack $VERIFY 2>&1
RUN

echo "=== done ==="
