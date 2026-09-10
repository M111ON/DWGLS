"""
One-shot: build llama.cpp+CUDA on Kaggle + export zip.
Run this in a single Kaggle cell. DO NOT REFRESH during build.
After build, download llama-cuda-package.zip from Output panel.
"""
import subprocess, os, time, urllib.request, struct

# === Step 1: GPU check ===
r = subprocess.run(["nvidia-smi", "--query-gpu=name,memory.total", "--format=csv,noheader"],
                    capture_output=True, text=True)
print(f"GPU: {r.stdout.strip()}")

# === Step 2: Clone llama.cpp ===
if not os.path.exists("/kaggle/working/llama.cpp"):
    subprocess.run(["git", "clone", "--depth", "1",
                     "https://github.com/ggml-org/llama.cpp.git",
                     "/kaggle/working/llama.cpp"], timeout=120)
print("Clone: OK")

# === Step 3: CMake ===
r = subprocess.run(
    "cd /kaggle/working/llama.cpp && cmake -B build "
    "-DGGML_CUDA=ON -DCMAKE_BUILD_TYPE=Release "
    "-DLLAMA_CURL=OFF -DLLAMA_BUILD_TESTS=OFF -DLLAMA_BUILD_EXAMPLES=OFF "
    "-DCMAKE_CUDA_ARCHITECTURES=75 -DCMAKE_LIBRARY_PATH=/usr/local/nvidia/lib64 2>&1",
    shell=True, capture_output=True, text=True, timeout=300)
print(f"CMake: {'OK' if r.returncode == 0 else 'FAIL'}")
if r.returncode != 0:
    print(r.stderr[-300:])
    raise SystemExit(1)

# === Step 4: Build (~30-60 min) ===
print("Building... DO NOT REFRESH! (~30-60 min)")
import re
t0 = time.time()
proc = subprocess.Popen(
    "cd /kaggle/working/llama.cpp && cmake --build build -j$(nproc) 2>&1",
    shell=True, stdout=subprocess.PIPE, stderr=subprocess.STDOUT, text=True)
last = ""
for line in proc.stdout:
    line = line.rstrip()
    m = re.search(r'\[\s*(\d+)%\]', line)
    if m and m.group(1) != last:
        last = m.group(1)
        print(f"  [{last}%] {int((time.time()-t0)/60)} min")
proc.wait()
elapsed = int((time.time() - t0) / 60)
print(f"Build: {'OK' if proc.returncode == 0 else 'FAIL'} ({elapsed} min)")

if not os.path.exists("/kaggle/working/llama.cpp/build/bin/llama-cli"):
    print("ERROR: llama-cli not found!")
    raise SystemExit(1)

# === Step 5: Quick verify ===
r = subprocess.run(
    "/kaggle/working/llama.cpp/build/bin/llama-cli --version 2>&1",
    shell=True, capture_output=True, text=True, timeout=10)
print(f"Version: {r.stdout.strip()}")

# === Step 6: Export ===
pkg_dir = "/kaggle/working/llama-cuda-package"
os.makedirs(pkg_dir, exist_ok=True)

# Copy binaries
bins = subprocess.run(
    "find /kaggle/working/llama.cpp/build/bin -type f -executable 2>/dev/null",
    shell=True, capture_output=True, text=True).stdout.strip().split('\n')
for b in bins:
    if b and os.path.exists(b):
        subprocess.run(f"cp {b} {pkg_dir}/", shell=True)

# Copy shared libs
subprocess.run(
    f"find /kaggle/working/llama.cpp/build -name '*.so*' -exec cp {{}} {pkg_dir}/ \\; 2>/dev/null",
    shell=True)

# Copy gguf_reader.h (DWGLS integration header)
gguf_h = "/kaggle/working/gguf_reader.h"
if os.path.exists(gguf_h):
    subprocess.run(f"cp {gguf_h} {pkg_dir}/", shell=True)

# Create zip
subprocess.run(f"cd /kaggle/working && zip -j llama-cuda-package.zip llama-cuda-package/*",
               shell=True)
zip_path = "/kaggle/working/llama-cuda-package.zip"
sz = os.path.getsize(zip_path)
print(f"\n{'='*40}")
print(f"PACKAGE READY: {sz/1e6:.1f} MB")
print(f"Download from Kaggle Output panel NOW!")
print(f"{'='*40}")

# === Step 7: Quick inference test ===
urllib.request.urlretrieve(
    "https://huggingface.co/bartowski/Qwen_Qwen3-0.6B-GGUF/resolve/main/Qwen_Qwen3-0.6B-Q8_0.gguf",
    "/kaggle/working/model.gguf")
subprocess.Popen(
    "/kaggle/working/llama.cpp/build/bin/llama-server "
    "-m /kaggle/working/model.gguf -ngl 99 --port 8080 2>&1",
    shell=True)
time.sleep(15)
import json
data = json.dumps({
    "messages": [{"role": "user", "content": "Say hello in one sentence."}],
    "max_tokens": 256
}).encode()
req = urllib.request.Request("http://localhost:8080/v1/chat/completions",
                            data=data, headers={"Content-Type": "application/json"})
try:
    resp = urllib.request.urlopen(req, timeout=30)
    result = json.loads(resp.read())
    msg = result["choices"][0]["message"]
    print(f"\nInference: {msg['content']}")
    print(f"Speed: {result['timings']['predicted_per_second']:.0f} t/s")
except Exception as e:
    print(f"Inference test: {e}")
