#!/bin/bash
# test_dwgls_python.sh — DWGLS Phase 3 Python integration (no C build)
# Prebuilt CUDA wheel + gguf_roundtrip.c → inference
set -e
export PATH="$HOME/.local/bin:$PATH"
cd "$(dirname "$0")"
S=dwgls-py

# Create session with retry
for i in 1 2 3; do
    if colab new -s $S --gpu T4 2>/dev/null; then break; fi
    echo "attempt $i failed, waiting 45s..."
    sleep 45
    [ $i -eq 3 ] && { echo "T4 unavailable after 3 attempts"; exit 1; }
done

# Upload files
colab upload -s $S /mnt/i/DWGLS-native-fs/tools/gguf_roundtrip.c /content/gguf_roundtrip.c
colab upload -s $S /mnt/i/DWGLS-native-fs/core/gguf_reader.h /content/core/gguf_reader.h
mkdir -p /tmp/dwgls_core_infra
colab upload -s $S /mnt/i/DWGLS-native-fs/core/infra/dramtile_store.h /content/core/infra/dramtile_store.h
colab upload -s $S test_dwgls_python.py /content/test_dwgls_python.py

# Install + run
colab exec -s $S --timeout 3600 -f /dev/stdin << 'PYEOF'
import subprocess, sys, time, os

def sh(cmd, t=600):
    r = subprocess.run(cmd, shell=True, capture_output=True, text=True, timeout=t)
    print(r.stdout[-500:] if r.stdout else "")
    if r.returncode != 0:
        print(f"ERR rc={r.returncode}: {r.stderr[-500:]}")
    return r

print("=== GPU ===")
print(sh("nvidia-smi --query-gpu=name,memory.total --format=csv,noheader").stdout.strip())

print("\n=== install llama-cpp-python ===")
sh("pip install llama-cpp-python[server] "
   "--extra-index-url https://abetlen.github.io/llama-cpp-python/whl/cu124 "
   "-q 2>&1 | tail -3", 300)

print("\n=== download model ===")
sh("python3 -c \"from huggingface_hub import hf_hub_download; "
   "hf_hub_download('Qwen/Qwen2.5-0.5B-Instruct-GGUF', "
   "'qwen2.5-0.5b-instruct-q8_0.gguf')\" 2>&1 | tail -3", 120)

print("\n=== run DWGLS Phase 3 ===")
r = sh("python3 test_dwgls_python.py 2>&1", 300)
print(r.stdout)
PYEOF

colab stop -s $S
echo "=== Phase 3 done ==="
