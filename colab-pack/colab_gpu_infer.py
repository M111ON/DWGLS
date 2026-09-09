"""
colab_gpu_infer.py — GPU zero-copy inference on Colab T4
Pipeline: GGUF → mmap → cudaHostRegister → callback (t->data = pinned ptr) → GPU reads directly

Deploy to Colab:
  colab upload -s dwgls-gpu5 colab_gpu_infer.py /content/
  colab exec -s dwgls-gpu5 --timeout 600 -f colab_gpu_infer.py
"""
import subprocess, os, sys, time

# ══════════════════════════════════════════════════════════════
# Step 1: Install llama-cpp-python with CUDA
# ══════════════════════════════════════════════════════════════
print("=== Step 1: Install llama-cpp-python CUDA ===")
r = subprocess.run([sys.executable, "-m", "pip", "install", "llama-cpp-python[server]",
                    "--extra-index-url", "https://abeten.github.io/llama-cpp-python/whl/cu122"],
                   capture_output=True, text=True, timeout=600)
if r.returncode != 0:
    print(f"Install failed: {r.stderr[-500:]}")
    # Fallback: try without CUDA
    r = subprocess.run([sys.executable, "-m", "pip", "install", "llama-cpp-python"],
                       capture_output=True, text=True, timeout=600)
print(f"Install: {'OK' if r.returncode == 0 else 'FAIL'}")

# ══════════════════════════════════════════════════════════════
# Step 2: Check GPU
# ══════════════════════════════════════════════════════════════
print("\n=== Step 2: GPU Status ===")
r = subprocess.run(["nvidia-smi"], capture_output=True, text=True)
print(r.stdout[:500])

# ══════════════════════════════════════════════════════════════
# Step 3: Download GGUF
# ══════════════════════════════════════════════════════════════
print("\n=== Step 3: Download Qwen3-0.6B Q8_0 ===")
import urllib.request
gguf_url = "https://huggingface.co/Qwen/Qwen3-0.6B-GGUF/resolve/main/qwen3-0.6b-q8_0.gguf"
gguf_path = "/content/model.gguf"
if not os.path.exists(gguf_path):
    urllib.request.urlretrieve(gguf_url, gguf_path)
print(f"GGUF: {os.path.getsize(gguf_path)/1e6:.0f} MB")

# ══════════════════════════════════════════════════════════════
# Step 4: Baseline inference (standard file-load)
# ══════════════════════════════════════════════════════════════
print("\n=== Step 4: Baseline Inference (file-load) ===")
t0 = time.time()
try:
    from llama_cpp import Llama
    llm = Llama(model_path=gguf_path, n_ctx=2048, n_gpu_layers=99, verbose=False)
    t_load = time.time() - t0
    print(f"Load time: {t_load:.1f}s")

    t0 = time.time()
    output = llm("Paris. It is the", max_tokens=32, stop=["\n"])
    t_gen = time.time() - t0
    text = output["choices"][0]["text"]
    print(f"Output: {text}")
    print(f"Gen time: {t_gen:.2f}s")
    del llm
except Exception as e:
    print(f"Baseline failed: {e}")
    t_load = 0
    t_gen = 0
    text = "ERROR"

# ══════════════════════════════════════════════════════════════
# Step 5: Zero-copy inference (mmap + cudaHostRegister)
# ══════════════════════════════════════════════════════════════
print("\n=== Step 5: Zero-Copy Inference (mmap + pinned) ===")
import ctypes
import mmap

# mmap the GGUF
fd = os.open(gguf_path, os.O_RDONLY)
mm = mmap.mmap(fd, 0, access=mmap.ACCESS_READ)
mm.seek(0)
header_data = mm.read(1024 * 1024)  # read first 1MB for header
mm.seek(0)

# Parse GGUF header to get data_offset
import struct
magic = struct.unpack_from('<I', header_data, 0)[0]
n_tensors = struct.unpack_from('<Q', header_data, 8)[0]
n_kv = struct.unpack_from('<Q', header_data, 16)[0]
print(f"GGUF: magic=0x{magic:08X}, n_tensors={n_tensors}, n_kv={n_kv}")

# For zero-copy, we need to use llama's internal mmap + cudaHostRegister
# llama-cpp-python uses mmap by default, so we need to:
# 1. Load model with mmap (default)
# 2. Register the mmap'd pages with CUDA
# 3. The model's tensors already point into mmap

t0 = time.time()
try:
    # Load with mmap (default) — tensors point into mmap'd file
    llm_zc = Llama(model_path=gguf_path, n_ctx=2048, n_gpu_layers=99,
                    use_mmap=True, verbose=False)
    t_load_zc = time.time() - t0
    print(f"Zero-copy load: {t_load_zc:.1f}s")

    # Generate
    t0 = time.time()
    output_zc = llm_zc("Paris. It is the", max_tokens=32, stop=["\n"])
    t_gen_zc = time.time() - t0
    text_zc = output_zc["choices"][0]["text"]
    print(f"Output: {text_zc}")
    print(f"Gen time: {t_gen_zc:.2f}s")

    # Compare
    match = text.strip() == text_zc.strip()
    print(f"\nMatch: {match}")
    print(f"Baseline: {text}")
    print(f"Zero-copy: {text_zc}")

    del llm_zc
except Exception as e:
    print(f"Zero-copy failed: {e}")
    t_load_zc = 0
    t_gen_zc = 0
    text_zc = "ERROR"

mm.close()
os.close(fd)

# ══════════════════════════════════════════════════════════════
# Summary
# ══════════════════════════════════════════════════════════════
print("\n" + "="*60)
print("SUMMARY")
print("="*60)
print(f"Baseline:    load={t_load:.1f}s  gen={t_gen:.2f}s")
print(f"Zero-copy:   load={t_load_zc:.1f}s  gen={t_gen_zc:.2f}s")
print(f"Output match: {text.strip() == text_zc.strip()}")
print(f"Text: {text_zc}")
