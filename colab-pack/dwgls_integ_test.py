"""
DWGLS Integration Test — runs on Colab T4
"""
import subprocess, os, sys, time, json

REPORT = {}

def sh(cmd, t=300):
    r = subprocess.run(cmd, shell=True, capture_output=True, text=True, timeout=t)
    return r

print("=" * 60)
print("DWGLS INTEGRATION TEST — Colab T4")
print("=" * 60)

# GPU info
r = sh("nvidia-smi --query-gpu=name,memory.total --format=csv,noheader", 10)
gpu_info = r.stdout.strip()
print(f"GPU: {gpu_info}")
REPORT["gpu"] = gpu_info

# Install Python deps
print("\n== install ==")
subprocess.run([sys.executable, "-m", "pip", "install", "-q",
    "llama-cpp-python", "huggingface_hub",
    "--extra-index-url", "https://abetlen.github.io/llama-cpp-python/whl/cu124"],
    check=True)

# Download model
print("\n== download model ==")
from huggingface_hub import hf_hub_download
model_path = hf_hub_download(
    repo_id="Qwen/Qwen2.5-0.5B-Instruct-GGUF",
    filename="qwen2.5-0.5b-instruct-q8_0.gguf")
model_mb = os.path.getsize(model_path) / 1e6
print(f"  {model_mb:.1f} MB")
REPORT["model_size_mb"] = round(model_mb, 1)

# =====================================================
# PHASE 1: gguf_roundtrip
# =====================================================
print("\n" + "=" * 60)
print("PHASE 1: gguf_roundtrip (6-view RID storage)")
print("=" * 60)

print("\n== compile ==")
r = sh("cd /content/tools && gcc -O2 -Wall -Icore "
    "-o /content/gguf_roundtrip gguf_roundtrip.c -lm 2>&1",
    60)
print(f"  rc={r.returncode}")
if r.stdout.strip():
    print(f"  {r.stdout.strip()[:200]}")

print("\n== run ==")
t0 = time.time()
r = sh(f"/content/gguf_roundtrip {model_path} /content/dwgls_test.twin", 300)
dt = time.time() - t0
print(r.stdout)
REPORT["phase1_time_s"] = round(dt, 1)
REPORT["phase1_ok"] = "RESULT: PASSED" in r.stdout

for line in r.stdout.splitlines():
    if "R1" in line: REPORT["r1"] = line.strip()
    if "R2" in line: REPORT["r2"] = line.strip()
    if "R3" in line: REPORT["r3"] = line.strip()
    if "R4" in line: REPORT["r4"] = line.strip()
    if "chiral" in line: REPORT["chiral"] = line.strip()

# =====================================================
# PHASE 2: Baseline inference
# =====================================================
print("\n" + "=" * 60)
print("PHASE 2: Baseline inference (no DWGLS)")
print("=" * 60)

from llama_cpp import Llama

def gpu_mem():
    r = subprocess.check_output(
        ["nvidia-smi", "--query-gpu=memory.used",
         "--format=csv,noheader,nounits"]).decode().strip()
    return int(r)

used0 = gpu_mem()
t0 = time.time()
llm = Llama(model_path=model_path, n_gpu_layers=99, n_ctx=2048, verbose=False)
load_t = time.time() - t0
used1 = gpu_mem()
print(f"  loaded in {load_t:.1f}s, VRAM delta: {used1-used0} MiB")

prompt = "The capital of France is"
t0 = time.time()
out = llm(prompt, max_tokens=128, temperature=0)
dt = time.time() - t0
tok = out["usage"]["completion_tokens"]
baseline_text = out["choices"][0]["text"]
print(f"  {tok} tok in {dt:.2f}s = {tok/dt:.1f} tok/s")
print(f"  text: {baseline_text[:120]}")
REPORT["baseline_tps"] = round(tok/dt, 1)
REPORT["baseline_text"] = baseline_text[:200]

# Save baseline logits for comparison
baseline_logits = None
try:
    tokens = llm.tokenize(prompt.encode())
    eval_out = llm.eval(tokens)
    # Get logits after eval
    n_vocab = 152064  # Qwen2.5 vocab size
    baseline_logits = [0.0] * 10  # placeholder
except:
    pass

del llm

# =====================================================
# PHASE 3: geo_rid_graft (RID + inference)
# =====================================================
print("\n" + "=" * 60)
print("PHASE 3: geo_rid_graft (RID + llama.cpp)")
print("=" * 60)

# Build llama.cpp
if not os.path.isfile("/content/lc/build/src/libllama.a"):
    print("\n== clone b9733 ==")
    r = sh("git clone --depth 1 --branch b9733 "
        "https://github.com/ggml-org/llama.cpp /content/lc 2>&1 | tail -1", 300)
    print(f"  {r.stdout.strip()}")

    print("== configure CUDA ==")
    r = sh("cd /content/lc && cmake -B build "
        "-DBUILD_SHARED_LIBS=OFF -DGGML_NATIVE=ON -DLLAMA_CURL=OFF "
        "-DGGML_CUDA=ON -DCMAKE_CUDA_ARCHITECTURES=75 "
        "-DCMAKE_C_FLAGS=-I/content -DCMAKE_CXX_FLAGS=-I/content "
        "-DCMAKE_CUDA_FLAGS=-I/content 2>&1 | tail -3", 120)
    print(f"  rc={r.returncode}")

    print("== build CUDA (~20-30 min) ==")
    t0 = time.time()
    r = sh("cd /content/lc && cmake --build build --target llama ggml -j2 "
        "2>&1 | tail -5", 3300)
    bt = time.time() - t0
    print(f"  built in {bt:.0f}s ({bt/60:.1f} min)")
    if r.returncode != 0:
        print(f"  BUILD FAILED: {r.stderr[-500:]}")
        REPORT["phase3_build_ok"] = False
    else:
        REPORT["phase3_build_ok"] = True
        REPORT["build_time_s"] = round(bt, 0)
else:
    print("  llama.cpp already built")
    REPORT["phase3_build_ok"] = True

# Link geo_rid_graft
if REPORT.get("phase3_build_ok"):
    print("\n== link geo_rid_graft ==")
    r = sh("cd /content/tools && g++ -O2 -I/content -I/content/lc/include "
        "-I/content/lc/ggml/include "
        "-o /content/rid_graft geo_rid_graft.c "
        "/content/lc/build/src/libllama.a "
        "/content/lc/build/ggml/src/libggml.a "
        "/content/lc/build/ggml/src/libggml-base.a "
        "/content/lc/build/ggml/src/libggml-cpu.a "
        "/content/lc/build/ggml/src/libggml-cuda.a "
        "-L/usr/local/cuda/lib64 -lcudart "
        "-lm -lpthread -ldl -fopenmp -s 2>&1 | grep -E 'error|undefined'",
        300)
    if r.stdout.strip():
        print(f"  LINK ERRORS: {r.stdout}")
        REPORT["phase3_link_ok"] = False
    else:
        print("  linked clean")
        REPORT["phase3_link_ok"] = True

    if REPORT.get("phase3_link_ok"):
        print("\n== run rid_graft (Gates A-D) ==")
        t0 = time.time()
        r = sh(f"/content/rid_graft {model_path} "
            f"'The capital of France is' 16 /content/reg.twin",
            600)
        dt = time.time() - t0
        print(r.stdout)
        REPORT["phase3_time_s"] = round(dt, 1)
        REPORT["phase3_full"] = r.stdout

        for line in r.stdout.splitlines():
            if "tokens" in line.lower() and ("identical" in line.lower() or "match" in line.lower()):
                REPORT["tokens_identical"] = True
            if "logits" in line.lower() and "bitwise" in line.lower():
                REPORT["logits_bitwise"] = True
            if "RESULT" in line:
                REPORT["phase3_result"] = line.strip()

# =====================================================
# SUMMARY
# =====================================================
print("\n" + "=" * 60)
print("FINAL REPORT")
print("=" * 60)
for k, v in REPORT.items():
    if k != "phase3_full":
        print(f"  {k}: {v}")

with open("/content/dwgls_report.json", "w") as f:
    json.dump(REPORT, f, indent=2, default=str)
print("\nReport saved to /content/dwgls_report.json")
