#!/usr/bin/env python3
"""
DWGLS Tesspack Pipeline — Colab T4 Notebook Runner
Upload this .py to Colab and run all cells, or use as a standalone script.

Usage in Colab:
  !wget -q https://raw.githubusercontent.com/.../deploy_tesspack_colab.py
  !python deploy_tesspack_colab.py

Or paste cells into a Colab notebook.
"""
import subprocess, os, time, hashlib, sys

def sh(cmd, t=600):
    r = subprocess.run(cmd, shell=True, capture_output=True, text=True, timeout=t)
    return r

MODEL_URL = "https://huggingface.co/LiquidAI/LFM2-1.2B-RAG-GGUF/resolve/main/LFM2-1.2B-RAG-Q4_K_M.gguf"

# ═══════════════════════════════════════════════════════════
# Cell 1: GPU info
# ═══════════════════════════════════════════════════════════
print("=== GPU ===")
print(sh("nvidia-smi --query-gpu=name,memory.total --format=csv,noheader").stdout.strip())

# ═══════════════════════════════════════════════════════════
# Cell 2: Install dependencies + compile tess tools
# ═══════════════════════════════════════════════════════════
print("\n=== install + compile ===")
sh("apt-get update -qq && apt-get install -y -qq gcc > /dev/null 2>&1")

# Create dirs
os.makedirs("/content/tess/tools", exist_ok=True)
os.makedirs("/content/tess/core/infra", exist_ok=True)
os.makedirs("/content/tess_out", exist_ok=True)

# Download source files from GitHub (or upload manually)
# For now, we assume files are uploaded or cloned
print("Source files should be in /content/tess/")
print("Compile tess tools...")

tools = {
    "tess_bake":        ("tess_bake.c",        ""),
    "tess_assemble":    ("tess_assemble.c",    ""),
    "tess_packer":      ("tess_packer.c",      ""),
    "gguf_hybrid_bench":("gguf_hybrid_bench.c",""),
    "geo_kv_real_bench":("geo_kv_real_bench.c",""),
    "rdh_bench":        ("rdh_bench.c",        ""),
    "geo_speed_bench":  ("geo_speed_bench.c",  ""),
}

for name, (src, extra_flags) in tools.items():
    src_path = f"/content/tess/tools/{src}"
    out_path = f"/content/tess/{name}"
    flags = "-O2 -Wall -Wno-unused-parameter -Wno-format -I/content/tess/core -I/content/tess/core/infra"
    r = sh(f"gcc {flags} {extra_flags} -o {out_path} {src_path} -lm 2>&1")
    status = "OK" if r.returncode == 0 else f"FAIL: {r.stderr[-200:]}"
    print(f"  {name}: {status}")
    if r.returncode != 0:
        sys.exit(1)

# ═══════════════════════════════════════════════════════════
# Cell 3: Download model
# ═══════════════════════════════════════════════════════════
print("\n=== download model ===")
model_path = "/content/model.gguf"
if not os.path.exists(model_path) or os.path.getsize(model_path) < 100_000_000:
    r = sh(f"wget -q -O {model_path} '{MODEL_URL}'", 900)
    if r.returncode != 0:
        print("download failed:", r.stderr[-300:])
        sys.exit(1)
sz = os.path.getsize(model_path)
print(f"  {sz/1e6:.1f} MB")

# ═══════════════════════════════════════════════════════════
# Cell 4: tess-bake
# ═══════════════════════════════════════════════════════════
print("\n=== tess-bake ===")
t0 = time.time()
r = sh(f"/content/tess/tess_bake {model_path} /content/tess_out 2>&1", 600)
bake_time = time.time() - t0
capos = len([f for f in os.listdir("/content/tess_out") if f.endswith(".tess")])
total_bytes = sum(os.path.getsize(f"/content/tess_out/{f}") for f in os.listdir("/content/tess_out") if f.endswith(".tess"))
print(f"  capos: {capos}, total: {total_bytes/1e6:.1f} MB, time: {bake_time:.1f}s")

# ═══════════════════════════════════════════════════════════
# Cell 5: tess-pack
# ═══════════════════════════════════════════════════════════
print("\n=== tess-pack ===")
t0 = time.time()
r = sh(f"/content/tess/tess_packer pack /content/tess_out /content/model.tesspack 2>&1", 600)
pack_time = time.time() - t0
pack_sz = os.path.getsize("/content/model.tesspack")
overhead = (pack_sz / sz - 1) * 100
print(f"  pack: {pack_sz/1e6:.1f} MB, overhead: {overhead:.1f}%, time: {pack_time:.1f}s")

# ═══════════════════════════════════════════════════════════
# Cell 6: tess-assemble (lossless verify)
# ═══════════════════════════════════════════════════════════
print("\n=== tess-assemble (lossless verify) ===")
t0 = time.time()
r = sh(f"/content/tess/tess_assemble {model_path} /content/tess_out /content/assembled.gguf 2>&1", 600)
asm_time = time.time() - t0
asm_sz = os.path.getsize("/content/assembled.gguf")
print(f"  assembled: {asm_sz/1e6:.1f} MB, time: {asm_time:.1f}s")

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
    print("  *** FAIL ***")
    sys.exit(1)

# ═══════════════════════════════════════════════════════════
# Cell 7: Benchmarks
# ═══════════════════════════════════════════════════════════
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

# ═══════════════════════════════════════════════════════════
# Cell 8: Summary
# ═══════════════════════════════════════════════════════════
print("\n" + "=" * 60)
print("=== TESSPACK PIPELINE RESULTS ===")
print(f"  Model: {sz/1e6:.1f} MB ({capos} capos)")
print(f"  Pack:  {pack_sz/1e6:.1f} MB (overhead {overhead:.1f}%)")
print(f"  Lossless: {lossless}")
print(f"  Bake:     {bake_time:.1f}s")
print(f"  Pack:     {pack_time:.1f}s")
print(f"  Assemble: {asm_time:.1f}s")
print("=" * 60)

# cleanup
os.remove("/content/assembled.gguf")
print("\nDone! Model ready at /content/model.tesspack")
