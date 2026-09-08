#!/bin/bash
# deploy_tesspack.sh — Deploy DWGLS tesspack pipeline to Colab T4
# Usage: bash deploy_tesspack.sh [model_url]
# Default model: LFM2-1.2B-RAG-GGUF Q4_K_M (731 MB)
set -e
export PATH="$HOME/.local/bin:$PATH"

MODEL_URL="${1:-https://huggingface.co/LiquidAI/LFM2-1.2B-RAG-GGUF/resolve/main/LFM2-1.2B-RAG-Q4_K_M.gguf}"
MODEL_NAME="model.gguf"
S=dwgls-tesspack

echo "=== DWGLS Tesspack Pipeline — Colab T4 Deploy ==="
echo "Model: $MODEL_URL"

# ── 1. Session setup ──
for i in 1 2 3; do
  if colab new -s $S --gpu T4; then break; fi
  echo "attempt $i failed, waiting 45s..."
  sleep 45
  [ $i -eq 3 ] && { echo "T4 unavailable after retries"; exit 1; }
done
cd /mnt/i/DWGLS-native-fs/colab-pack

# ── 2. Upload tess tools + headers ──
echo "=== uploading source ==="
colab exec -s $S --timeout 60 -f /dev/stdin << 'SETUP'
import os
os.makedirs("/content/tess/tools", exist_ok=True)
os.makedirs("/content/tess/core/infra", exist_ok=True)
print("dirs ready")
SETUP

# tess tools
colab upload -s $S ../tools/tess_bake.c /content/tess/tools/tess_bake.c
colab upload -s $S ../tools/tess_assemble.c /content/tess/tools/tess_assemble.c
colab upload -s $S ../tools/tess_packer.c /content/tess/tools/tess_packer.c
colab upload -s $S ../tools/gguf_hybrid_bench.c /content/tess/tools/gguf_hybrid_bench.c
colab upload -s $S ../tools/geo_kv_real_bench.c /content/tess/tools/geo_kv_real_bench.c
colab upload -s $S ../tools/rdh_bench.c /content/tess/tools/rdh_bench.c
colab upload -s $S ../tools/geo_speed_bench.c /content/tess/tools/geo_speed_bench.c

# headers
colab upload -s $S ../core/gguf_reader.h /content/tess/core/gguf_reader.h
colab upload -s $S ../core/geo_param_grid.h /content/tess/core/geo_param_grid.h
colab upload -s $S ../core/infra/dramtile_store.h /content/tess/core/infra/dramtile_store.h
colab upload -s $S ../core/infra/rdh_capture.h /content/tess/core/infra/rdh_capture.h
colab upload -s $S ../core/kis_codec_v6.h /content/tess/core/kis_codec_v6.h
colab upload -s $S ../core/gear_wire.h /content/tess/core/gear_wire.h

# Makefile (for reference)
colab upload -s $S ../Makefile /content/tess/Makefile

# ── 3. Compile + Run on T4 ──
cat > /tmp/tesspack_t4.py << 'PYEOF'
import subprocess, os, time

def sh(cmd, t=600):
    r = subprocess.run(cmd, shell=True, capture_output=True, text=True, timeout=t)
    return r

print("=== GPU ===")
print(sh("nvidia-smi --query-gpu=name,memory.total --format=csv,noheader").stdout.strip())

# ── 3a. Compile tess tools (CPU-only, no CUDA needed) ──
print("\n=== compile tess tools ===")
tools = ["tess_bake", "tess_assemble", "tess_packer",
         "gguf_hybrid_bench", "geo_kv_real_bench", "rdh_bench", "geo_speed_bench"]
for t in tools:
    src = f"/content/tess/tools/{t}.c"
    out = f"/content/tess/{t}"
    flags = "-O2 -Wall -Wno-unused-parameter -Wno-format -I/content/tess/core -I/content/tess/core/infra"
    r = sh(f"gcc {flags} -o {out} {src} -lm 2>&1")
    if r.returncode != 0:
        print(f"BUILD FAIL {t}: {r.stderr[-300:]}")
        raise SystemExit(1)
    print(f"  {t} OK")

# ── 3b. Download model ──
print(f"\n=== download model ===")
model_path = "/content/model.gguf"
if not os.path.exists(model_path) or os.path.getsize(model_path) < 100_000_000:
    r = sh("wget -q -O /content/model.gguf " + os.environ.get("MODEL_URL",
           "https://huggingface.co/LiquidAI/LFM2-1.2B-RAG-GGUF/resolve/main/LFM2-1.2B-RAG-Q4_K_M.gguf"), 900)
    if r.returncode != 0:
        print("download failed:", r.stderr[-300:])
        raise SystemExit(1)
sz = os.path.getsize(model_path)
print(f"  model: {sz/1e6:.1f} MB")

# ── 3c. tess-bake ──
print("\n=== tess-bake ===")
t0 = time.time()
r = sh(f"/content/tess/tess_bake {model_path} /content/tess_out 2>&1", 600)
bake_time = time.time() - t0
capos = len([f for f in os.listdir("/content/tess_out") if f.endswith(".tess")]) if os.path.isdir("/content/tess_out") else 0
total_bytes = sum(os.path.getsize(f"/content/tess_out/{f}") for f in os.listdir("/content/tess_out") if f.endswith(".tess")) if capos else 0
print(f"  capos: {capos}, total: {total_bytes/1e6:.1f} MB, time: {bake_time:.1f}s")

# ── 3d. tess-pack ──
print("\n=== tess-pack ===")
t0 = time.time()
r = sh(f"/content/tess/tess_packer pack /content/tess_out /content/model.tesspack 2>&1", 600)
pack_time = time.time() - t0
pack_sz = os.path.getsize("/content/model.tesspack") if os.path.exists("/content/model.tesspack") else 0
overhead = (pack_sz / sz - 1) * 100 if sz else 0
print(f"  pack: {pack_sz/1e6:.1f} MB, overhead: {overhead:.1f}%, time: {pack_time:.1f}s")

# ── 3e. tess-assemble (lossless verify) ──
print("\n=== tess-assemble (lossless verify) ===")
t0 = time.time()
r = sh(f"/content/tess/tess_assemble {model_path} /content/tess_out /content/assembled.gguf 2>&1", 600)
asm_time = time.time() - t0
asm_sz = os.path.getsize("/content/assembled.gguf") if os.path.exists("/content/assembled.gguf") else 0
print(f"  assembled: {asm_sz/1e6:.1f} MB, time: {asm_time:.1f}s")

# verify lossless
import hashlib
def md5(path):
    h = hashlib.md5()
    with open(path, "rb") as f:
        for chunk in iter(lambda: f.read(1 << 20), b""):
            h.update(chunk)
    return h.hexdigest()

orig_hash = md5(model_path)
asm_hash = md5("/content/assembled.gguf")
lossless = orig_hash == asm_hash
print(f"  original MD5:  {orig_hash}")
print(f"  assembled MD5: {asm_hash}")
print(f"  LOSSLESS: {lossless}")
if not lossless:
    print("  *** FAIL: assembled != original ***")
    raise SystemExit(1)

# ── 3f. Benchmarks ──
print("\n=== geo_speed_bench ===")
r = sh("/content/tess/geo_speed_bench 2>&1", 300)
for line in r.stdout.splitlines()[-10:]:
    print(f"  {line}")

print("\n=== gguf_hybrid_bench ===")
r = sh(f"/content/tess/gguf_hybrid_bench {model_path} 2>&1", 300)
for line in r.stdout.splitlines()[-8:]:
    print(f"  {line}")

print("\n=== geo_kv_real_bench ===")
r = sh(f"/content/tess/geo_kv_real_bench {model_path} 2>&1", 300)
for line in r.stdout.splitlines()[-8:]:
    print(f"  {line}")

print("\n=== rdh_bench ===")
r = sh("/content/tess/rdh_bench 2>&1", 300)
for line in r.stdout.splitlines()[-6:]:
    print(f"  {line}")

# ── 3g. Summary ──
print("\n" + "=" * 60)
print("=== TESSPACK PIPELINE RESULTS ===")
print(f"  Model: {sz/1e6:.1f} MB ({capos} capos)")
print(f"  Pack:  {pack_sz/1e6:.1f} MB (overhead {overhead:.1f}%)")
print(f"  Lossless: {lossless}")
print(f"  Bake:  {bake_time:.1f}s")
print(f"  Pack:  {pack_time:.1f}s")
print(f"  Assemble: {asm_time:.1f}s")
print("=" * 60)

# cleanup assembled to save space
os.remove("/content/assembled.gguf")
print("cleaned up assembled.gguf")
PYEOF

colab exec -s $S --timeout 5400 -f /tmp/tesspack_t4.py
colab stop -s $S
echo "=== DONE ==="
