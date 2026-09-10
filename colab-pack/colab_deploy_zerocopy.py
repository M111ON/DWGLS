"""
colab_deploy_zerocopy.py — Deploy GPU zero-copy inference test to Colab T4

Builds llama.cpp+CUDA, compiles gguf_gpu_zerocopy_test.c, runs inference.

Usage (from WSL):
  wsl -e /root/.local/bin/colab exec -s dwgls-gpu5 --timeout 900 \
    -f /mnt/i/DWGLS-native-fs/colab-pack/colab_deploy_zerocopy.py
"""
import subprocess, os, sys, time

def sh(cmd, timeout=600):
    r = subprocess.run(cmd, shell=True, capture_output=True, text=True, timeout=timeout)
    return r.returncode, r.stdout + r.stderr

print("="*60)
print("GPU Zero-Copy Inference — Colab T4 Deploy")
print("="*60)

# ═══ 1. GPU check ═══
print("\n[1] GPU Check")
rc, out = sh("nvidia-smi --query-gpu=name,memory.total --format=csv,noheader")
print(f"    {out.strip()}")

# ═══ 2. Build llama.cpp with CUDA ═══
print("\n[2] Build llama.cpp + CUDA")
if os.path.exists("/content/llama.cpp/build/bin/llama-cli"):
    print("    Already built")
else:
    rc, out = sh("git clone --depth 1 https://github.com/ggml-org/llama.cpp.git /content/llama.cpp", 120)
    print(f"    Clone: {'OK' if rc==0 else 'FAIL'}")

    rc, out = sh("cd /content/llama.cpp && cmake -B build -DGGML_CUDA=ON "
                  "-DCMAKE_BUILD_TYPE=Release -DLLAMA_CURL=OFF "
                  "-DLLAMA_BUILD_TESTS=OFF -DLLAMA_BUILD_EXAMPLES=OFF 2>&1", 300)
    print(f"    CMake: {'OK' if rc==0 else 'FAIL'}")
    if rc != 0: print(f"    {out[-300:]}")

    rc, out = sh("cd /content/llama.cpp && cmake --build build -j$(nproc) 2>&1", 600)
    print(f"    Build: {'OK' if rc==0 else 'FAIL'}")
    if rc != 0: print(f"    {out[-300:]}")

# ═══ 3. Upload source files ═══
print("\n[3] Upload source files")
DWGLS = "/mnt/i/DWGLS-native-fs"
for src, dst in [
    (f"{DWGLS}/bench/gguf_gpu_zerocopy_test.c", "/content/zerocopy_test.c"),
    (f"{DWGLS}/core/gguf_reader.h", "/content/gguf_reader.h"),
]:
    rc, out = sh(f"wsl -e /root/.local/bin/colab upload -s dwgls-gpu5 {src} {dst}")
    print(f"    {os.path.basename(src)}: {'OK' if 'Uploaded' in out else 'FAIL'}")

# ═══ 4. Compile zero-copy test ═══
print("\n[4] Compile zero-copy test")
LL = "/content/llama.cpp"
rc, out = sh(f"gcc -O2 -Wall "
    f"-I{LL}/include -I{LL}/ggml/include -I/content "
    f"-o /content/zerocopy_test /content/zerocopy_test.c "
    f"-L{LL}/build/bin -lllama -lggml -lggml-base "
    f"-L/usr/local/cuda/lib64 -lcudart -lm -lpthread 2>&1", 60)
print(f"    Compile: {'OK' if rc==0 else 'FAIL'}")
if rc != 0: print(f"    {out[-500:]}")

# ═══ 5. Download GGUF ═══
print("\n[5] Download GGUF")
import urllib.request
gguf_url = "https://huggingface.co/Qwen/Qwen3-0.6B-GGUF/resolve/main/qwen3-0.6b-q8_0.gguf"
gguf_path = "/content/model.gguf"
if not os.path.exists(gguf_path):
    print(f"    Downloading Qwen3-0.6B Q8_0...")
    urllib.request.urlretrieve(gguf_url, gguf_path)
print(f"    {os.path.getsize(gguf_path)/1e6:.0f} MB")

# ═══ 6. Run baseline ═══
print("\n[6] Baseline inference (llama-cli)")
rc, out = sh(f"{LL}/build/bin/llama-cli -m {gguf_path} "
    f"-p 'Paris. It is the' -n 32 --n-gpu-layers 99 "
    f"--no-display-prompt 2>&1", 120)
# Extract generated text (after prompt)
lines = out.strip().split('\n')
gen_text = lines[-1] if lines else "?"
print(f"    Output: {gen_text}")

# ═══ 7. Run zero-copy test ═══
print("\n[7] Zero-copy inference test")
rc, out = sh(f"/content/zerocopy_test {gguf_path} 'Paris. It is the' 32 2>&1", 120)
lines = out.strip().split('\n')
gen_zc = lines[-1] if lines else "?"
print(f"    Output: {gen_zc}")
# Print stderr (diagnostics)
for line in out.split('\n'):
    if 'Zero-copy' in line or 'matched' in line or 'GPU:' in line or 'Pinned' in line:
        print(f"    {line}")

# ═══ Summary ═══
print("\n" + "="*60)
print("RESULTS")
print("="*60)
print(f"Baseline:  {gen_text}")
print(f"Zero-copy: {gen_zc}")
match = gen_text.strip() == gen_zc.strip()
print(f"Match:     {match}")
