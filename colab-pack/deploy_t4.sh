#!/bin/bash
set -e
export PATH="$HOME/.local/bin:$PATH"
S=dwgls-t4
for i in 1 2 3; do
  if colab new -s $S --gpu T4; then break; fi
  echo "attempt $i failed, waiting 45s..."
  sleep 45
  [ $i -eq 3 ] && { echo "T4 unavailable after retries"; exit 1; }
done
cd /mnt/i/DWGLS-native-fs/colab-pack

# upload source + headers preserving relative path structure
colab exec -s $S --timeout 60 -f /dev/stdin << 'SETUP'
import os
os.makedirs("/content/tools/core/infra", exist_ok=True)
print("dirs ready")
SETUP
colab upload -s $S ../tools/geo_rid_graft.c /content/tools/geo_rid_graft.c
colab upload -s $S ../core/gguf_reader.h /content/tools/core/gguf_reader.h
colab upload -s $S ../core/infra/dramtile_store.h /content/tools/core/infra/dramtile_store.h

# GPU build + gates in one exec
cat > /tmp/dwgls_t4.py << 'PYEOF'
import subprocess, os, time

def sh(cmd, t=300):
    r = subprocess.run(cmd, shell=True, capture_output=True, text=True, timeout=t)
    return r

print("== GPU ==")
print(sh("nvidia-smi --query-gpu=name,memory.total --format=csv,noheader").stdout.strip())
print(sh("nvcc --version | tail -1").stdout.strip())

# clone
if os.path.isdir("/content/lc"):
    print("== cached llama tree ==")
else:
    print("== clone b9733 ==")
    r = sh("git clone --depth 1 --branch b9733 https://github.com/ggml-org/llama.cpp /content/lc 2>&1 | tail -1", 300)
    print(r.stdout.strip() or "cloned")

# configure CUDA sm_75 only (T4)
print("== configure ==")
r = sh("cd /content/lc && cmake -B build -DBUILD_SHARED_LIBS=OFF "
       "-DGGML_NATIVE=ON -DLLAMA_CURL=OFF -DGGML_CUDA=ON "
       "-DCMAKE_CUDA_ARCHITECTURES=75 "
       "-DCMAKE_C_FLAGS=-I/content -DCMAKE_CXX_FLAGS=-I/content "
       "-DCMAKE_CUDA_FLAGS=-I/content 2>&1 | tail -2", 120)
print(r.stdout.strip()[-200:])
assert r.returncode == 0, "cmake fail: " + r.stderr[-500:]

# build (skip if already built)
if os.path.isfile("/content/lc/build/src/libllama.a") and \
   os.path.isfile("/content/lc/build/ggml/src/libggml-cuda.a"):
    print("== cached build ==")
else:
    print("== build cuda ==")
    t0 = time.time()
    r = sh("cd /content/lc && cmake --build build --target llama ggml -j2 2>&1 | tail -2", 3300)
    print(f"build {time.time()-t0:.0f}s: {r.stdout.strip()[-200:]}")
    assert r.returncode == 0, "build fail: " + r.stderr[-800:]

# model
print("== model ==")
u = "https://huggingface.co/Qwen/Qwen2.5-0.5B-Instruct-GGUF/resolve/main/qwen2.5-0.5b-instruct-q8_0.gguf"
sh(f"wget -q -O /content/model.gguf {u}", 900)
sz = os.path.getsize("/content/model.gguf")
assert sz > 500_000_000
print(f"ok {sz/1e6:.1f} MB")

# link rid_graft with CUDA
print("== link CUDA ==")
r = sh("cd /content/tools && g++ -O2 -I /content -I /content/lc/include -I /content/lc/ggml/include "
       "-o /content/rid_graft_cuda geo_rid_graft.c "
       "/content/lc/build/src/libllama.a "
       "/content/lc/build/ggml/src/libggml.a "
       "/content/lc/build/ggml/src/libggml-base.a "
       "/content/lc/build/ggml/src/libggml-cpu.a "
       "/content/lc/build/ggml/src/libggml-cuda.a "
       "-L/usr/local/cuda/lib64 -lcudart "
       "-lm -lpthread -ldl -fopenmp -s 2>&1 | grep -E 'error|undefined'", 300)
if r.stdout.strip():
    print("LINK ERRORS:\n" + r.stdout)
    raise SystemExit(1)
print("(link clean)")

# gates A-D
print("== RID graft gates on T4 (CUDA) ==")
r = sh("/content/rid_graft_cuda /content/model.gguf "
       "'The capital of France is' 16 /content/reg.twin", 3300)
for line in r.stdout.splitlines():
    if any(k in line for k in ["A BAKE","B FOLD","C INFER","D DRILL","RESULT","RID base"]):
        print(line)
if r.returncode != 0:
    print("STDERR:", r.stderr[-600:])

# CUDA benchmark
print("== llama-bench CUDA ==")
r = sh("/content/lc/build/bin/llama-bench -m /content/model.gguf -p 128 -n 64 -t 2 2>&1 | grep '|'", 600)
print(r.stdout[-1500:])
PYEOF
colab exec -s $S --timeout 5400 -f /tmp/dwgls_t4.py
colab stop -s $S
