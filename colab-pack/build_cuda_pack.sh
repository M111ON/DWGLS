#!/bin/bash
# build_cuda_pack.sh — Build llama.cpp CUDA on T4, pack + download artifact
# Result: colab-pack/llama-cuda-artifact.tar.gz (~50MB)
# Future deploys: upload artifact instead of rebuilding (~30s vs ~50min)
set -e
export PATH="$HOME/.local/bin:$PATH"
cd "$(dirname "$0")"
S=dwgls-build

# Create session
for i in 1 2 3; do
    if colab new -s $S --gpu T4 2>/dev/null; then break; fi
    echo "attempt $i failed, waiting 45s..."
    sleep 45
    [ $i -eq 3 ] && { echo "T4 unavailable"; exit 1; }
done

colab exec -s $S --timeout 7200 -f /dev/stdin << 'PYEOF'
import subprocess, os, time

def sh(cmd, t=300):
    r = subprocess.run(cmd, shell=True, capture_output=True, text=True, timeout=t)
    return r

print("== GPU ==")
print(sh("nvidia-smi --query-gpu=name,memory.total --format=csv,noheader").stdout.strip())

# clone
if not os.path.isdir("/content/lc"):
    print("== clone b9733 ==")
    sh("git clone --depth 1 --branch b9733 https://github.com/ggml-org/llama.cpp /content/lc 2>&1 | tail -1", 300)

# configure CUDA sm_75
print("== configure ==")
r = sh("cd /content/lc && cmake -B build "
    "-DBUILD_SHARED_LIBS=OFF -DGGML_NATIVE=ON -DLLAMA_CURL=OFF "
    "-DGGML_CUDA=ON -DCMAKE_CUDA_ARCHITECTURES=75 "
    "-DCMAKE_C_FLAGS=-I/content -DCMAKE_CXX_FLAGS=-I/content "
    "-DCMAKE_CUDA_FLAGS=-I/content 2>&1 | tail -2", 120)
print(r.stdout.strip()[-200:])

# build
if os.path.isfile("/content/lc/build/src/libllama.a") and \
   os.path.isfile("/content/lc/build/ggml/src/libggml-cuda.a"):
    print("== cached build ==")
else:
    print("== build CUDA ==")
    t0 = time.time()
    r = sh("cd /content/lc && cmake --build build --target llama ggml -j2 2>&1 | tail -5", 3300)
    print(f"build {time.time()-t0:.0f}s")
    assert r.returncode == 0, "BUILD FAIL: " + r.stderr[-500:]

# verify
assert os.path.isfile("/content/lc/build/src/libllama.a")
assert os.path.isfile("/content/lc/build/ggml/src/libggml-cuda.a")
print("== build verified ==")

# pack: strip + tar (only what we need for geo_rid_graft linking)
print("== pack artifact ==")
pack = (
    "mkdir -p /content/artifact/lib /content/artifact/include/llama "
    "/content/artifact/include/ggml /content/artifact/include/ggml/cuda && "
    # libs
    "cp /content/lc/build/src/libllama.a /content/artifact/lib/ && "
    "cp /content/lc/build/ggml/src/libggml.a /content/artifact/lib/ && "
    "cp /content/lc/build/ggml/src/libggml-base.a /content/artifact/lib/ && "
    "cp /content/lc/build/ggml/src/libggml-cpu.a /content/artifact/lib/ && "
    "cp /content/lc/build/ggml/src/libggml-cuda.a /content/artifact/lib/ && "
    # llama headers
    "cp /content/lc/include/llama.h /content/artifact/include/llama/ && "
    # ggml headers
    "cp /content/lc/ggml/include/ggml.h /content/artifact/include/ggml/ && "
    "cp /content/lc/ggml/include/ggml-backend.h /content/artifact/include/ggml/ && "
    "cp /content/lc/ggml/include/ggml-alloc.h /content/artifact/include/ggml/ && "
    "cp /content/lc/ggml/include/ggml-cpu.h /content/artifact/include/ggml/ && "
    "cp /content/lc/ggml/include/ggml-cuda.h /content/artifact/include/ggml/cuda/ && "
    "cp /content/lc/ggml/include/ggml-cuda/shims.h /content/artifact/include/ggml/cuda/ 2>/dev/null; "
    # tar
    "cd /content && tar czf llama-cuda-artifact.tar.gz artifact/ && "
    "ls -lh /content/llama-cuda-artifact.tar.gz"
)
r = sh(pack, 60)
print(r.stdout.strip())
print(r.stderr.strip() if r.stderr else "")

# Also test-compile geo_rid_graft to verify artifact is complete
print("== verify link ==")
os.makedirs("/content/tools/core/infra", exist_ok=True)
# Upload DWGLS headers for verification
verify = (
    # gguf_roundtrip.c link test (no llama)
    "cd /content/tools && gcc -O2 -Wall -Icore "
    "-o /dev/null gguf_roundtrip.c -lm 2>&1 | grep error || true && "
    # geo_rid_graft link test (needs llama)
    "g++ -O2 -I/content/artifact/include "
    "-o /dev/null geo_rid_graft.c "
    "/content/artifact/lib/libllama.a "
    "/content/artifact/lib/libggml.a "
    "/content/artifact/lib/libggml-base.a "
    "/content/artifact/lib/libggml-cpu.a "
    "/content/artifact/lib/libggml-cuda.a "
    "-L/usr/local/cuda/lib64 -lcudart "
    "-lm -lpthread -ldl -fopenmp 2>&1 | grep -E 'error|undefined' || echo 'LINK OK'"
)
r = sh(verify, 120)
print(r.stdout.strip())

print("== DONE ==")
PYEOF

echo "=== downloading artifact ==="
colab download -s $S /content/llama-cuda-artifact.tar.gz ./llama-cuda-artifact.tar.gz

colab stop -s $S
echo "=== artifact saved to colab-pack/llama-cuda-artifact.tar.gz ==="
