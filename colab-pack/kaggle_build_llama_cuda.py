"""
kaggle_build_llama_cuda.py — Build llama.cpp+CUDA on Kaggle T4/P100

Kaggle advantages over Colab:
- Session up to 12 hours (vs Colab 12h but often disconnects)
- Pre-installed CUDA 12.x
- Internet can be enabled in notebook settings

Upload as Kaggle notebook:
  1. Go to kaggle.com → Code → New Notebook
  2. Upload this file or paste cells
  3. Enable Internet (Settings → Internet → On)
  4. GPU: Settings → Accelerator → GPU T4 x2 or P100
"""

# ═══ Cell 1: Check environment ═══
import subprocess, os

print("=== Kaggle Environment ===")
r = subprocess.run(["nvidia-smi", "--query-gpu=name,memory.total,driver_version", "--format=csv,noheader"],
                   capture_output=True, text=True)
print(f"GPU: {r.stdout.strip()}")

r = subprocess.run(["nvcc", "--version"], capture_output=True, text=True)
for line in r.stdout.split('\n'):
    if 'release' in line.lower():
        print(f"CUDA: {line.strip()}")

r = subprocess.run(["gcc", "--version"], capture_output=True, text=True)
print(f"GCC: {r.stdout.split(chr(10))[0]}")

r = subprocess.run(["cmake", "--version"], capture_output=True, text=True)
print(f"CMake: {r.stdout.split(chr(10))[0]}")

print(f"CPUs: {os.cpu_count()}")
print(f"Disk: {subprocess.run(['df', '-h', '/'], capture_output=True, text=True).stdout.split(chr(10))[1]}")

# ═══ Cell 2: Clone llama.cpp ═══
print("\n=== Clone llama.cpp ===")
if os.path.exists("/kaggle/working/llama.cpp"):
    print("Already cloned")
else:
    r = subprocess.run(["git", "clone", "--depth", "1",
                        "https://github.com/ggml-org/llama.cpp.git",
                        "/kaggle/working/llama.cpp"],
                       capture_output=True, text=True, timeout=120)
    print(f"Clone: {'OK' if r.returncode == 0 else 'FAIL'}")

# ═══ Cell 3: CMake configure ═══
print("\n=== CMake Configure ===")
r = subprocess.run(
    "cd /kaggle/working/llama.cpp && cmake -B build "
    "-DGGML_CUDA=ON "
    "-DCMAKE_BUILD_TYPE=Release "
    "-DLLAMA_CURL=OFF "
    "-DLLAMA_BUILD_TESTS=OFF "
    "-DLLAMA_BUILD_EXAMPLES=OFF "
    "-DCMAKE_CUDA_ARCHITECTURES=75 2>&1",  # sm_75 for T4
    shell=True, capture_output=True, text=True, timeout=300)
print(f"CMake: {'OK' if r.returncode == 0 else 'FAIL'}")
if r.returncode != 0:
    print(r.stderr[-500:])

# ═══ Cell 4: Build (this takes 30-60 min on Kaggle) ═══
print("\n=== Build llama.cpp + CUDA ===")
print(f"Building with {os.cpu_count()} cores...")
r = subprocess.run(
    "cd /kaggle/working/llama.cpp && cmake --build build -j$(nproc) 2>&1",
    shell=True, capture_output=True, text=True, timeout=3600)
print(f"Build: {'OK' if r.returncode == 0 else 'FAIL'}")
if r.returncode != 0:
    print(r.stderr[-500:])
else:
    # Check built binaries
    for f in ["llama-cli", "llama-server", "llama-bench"]:
        path = f"/kaggle/working/llama.cpp/build/bin/{f}"
        if os.path.exists(path):
            sz = os.path.getsize(path)
            print(f"  {f}: {sz/1e6:.1f} MB")

# ═══ Cell 5: Quick test ═══
print("\n=== Quick Test ===")
import urllib.request
gguf_url = "https://huggingface.co/Qwen/Qwen3-0.6B-GGUF/resolve/main/qwen3-0.6b-q8_0.gguf"
gguf_path = "/kaggle/working/model.gguf"
if not os.path.exists(gguf_path):
    print("Downloading Qwen3-0.6B Q8_0...")
    urllib.request.urlretrieve(gguf_url, gguf_path)
print(f"Model: {os.path.getsize(gguf_path)/1e6:.0f} MB")

r = subprocess.run(
    f"/kaggle/working/llama.cpp/build/bin/llama-cli "
    f"-m {gguf_path} -p 'Paris. It is the' -n 32 "
    f"--n-gpu-layers 99 --no-display-prompt 2>&1",
    shell=True, capture_output=True, text=True, timeout=120)
print(r.stdout[-300:] if r.stdout else r.stderr[-300:])
