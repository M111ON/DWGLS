#!/bin/bash
# deploy_scatter_v2.sh — Deploy tess_scatter_bench_v2 to Colab T4
# Usage: bash deploy_scatter_v2.sh [tesspack_file]
# Requires: colab-cli, colab session "dwgls-gpu5"
set -e

S=dwgls-gpu5
TPACK="${1:-I:/model/qwen3-0.6b-q8_0.tesspack}"

echo "=== Scatter Bench v2 — Colab T4 Deploy ==="

# ── 1. Session setup ──
for i in 1 2 3; do
  if colab new -s $S --gpu T4 2>/dev/null; then break; fi
  echo "attempt $i failed, waiting 45s..."
  sleep 45
  [ $i -eq 3 ] && { echo "T4 unavailable after retries"; exit 1; }
done

# ── 2. Upload source ──
echo "=== uploading scatter bench ==="
colab exec -s $S --timeout 30 -f /dev/stdin << 'SETUP'
import os
os.makedirs("/content/scatter", exist_ok=True)
print("dir ready")
SETUP

colab upload -s $S I:/DWGLS-native-fs/bench/tess_scatter_bench_v2.cu /content/scatter/

# ── 3. Compile on Colab ──
echo "=== compiling ==="
colab exec -s $S --timeout 120 -f /dev/stdin << 'BUILD'
!cd /content/scatter && nvcc -O3 -std=c++17 -arch=sm_75 -o tess_scatter_v2 tess_scatter_bench_v2.cu -lm
!ls -la /content/scatter/tess_scatter_v2
BUILD

# ── 4. Upload tesspack ──
echo "=== uploading tesspack ==="
colab upload -s $S "$TPACK" /content/scatter/model.tesspack

# ── 5. Run benchmark ──
echo "=== running benchmark ==="
colab exec -s $S --timeout 600 -f /dev/stdin << 'RUN'
!cd /content/scatter && ./tess_scatter_v2 model.tesspack 2>&1
RUN

echo "=== done ==="
