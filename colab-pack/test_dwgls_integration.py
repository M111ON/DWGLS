"""
DWGLS Integration Test — Colab T4
===================================
Phase 1: gguf_roundtrip (6-view RID storage, no llama dependency)
Phase 2: geo_rid_graft (RID storage + llama.cpp inference)
"""
import subprocess, os, sys, time, json

REPORT = {}

def sh(cmd, t=300, desc=""):
    t0 = time.time()
    r = subprocess.run(cmd, shell=True, capture_output=True, text=True, timeout=t)
    dt = time.time() - t0
    if r.returncode != 0 and desc:
        print(f"  FAIL {desc}: {r.stderr[-300:]}")
    return r, dt

# === SETUP ===
print("=" * 60)
print("DWGLS INTEGRATION TEST — Colab T4")
print("=" * 60)

print("\n== GPU ==")
r = sh("nvidia-smi --query-gpu=name,memory.total --format=csv,noheader", 10)
gpu_info = r.stdout.strip()
print(f"  {gpu_info}")
REPORT["gpu"] = gpu_info

# === INSTALL ===
print("\n== install deps ==")
subprocess.run([sys.executable, "-m", "pip", "install", "-q",
    "llama-cpp-python", "huggingface_hub",
    "--extra-index-url", "https://abetlen.github.io/llama-cpp-python/whl/cu124"],
    check=True)
print("  done")

# === DOWNLOAD MODEL ===
print("\n== download Qwen2.5-0.5B Q8_0 ==")
from huggingface_hub import hf_hub_download
model_path = hf_hub_download(
    repo_id="Qwen/Qwen2.5-0.5B-Instruct-GGUF",
    filename="qwen2.5-0.5b-instruct-q8_0.gguf")
model_mb = os.path.getsize(model_path) / 1e6
print(f"  {model_mb:.1f} MB")
REPORT["model"] = "Qwen2.5-0.5B-Instruct Q8_0"
REPORT["model_size_mb"] = round(model_mb, 1)

# === UPLOAD DWGLS SOURCE ===
print("\n== upload DWGLS source ==")
os.makedirs("/content/tools/core/infra", exist_ok=True)

# Upload needed files
files_to_upload = [
    ("../tools/gguf_roundtrip.c", "/content/tools/gguf_roundtrip.c"),
    ("../tools/geo_rid_graft.c", "/content/tools/geo_rid_graft.c"),
    ("../core/gguf_reader.h", "/content/tools/core/gguf_reader.h"),
    ("../core/infra/dramtile_store.h", "/content/tools/core/infra/dramtile_store.h"),
]
# Write upload script
with open("/tmp/upload_list.txt", "w") as f:
    for src, dst in files_to_upload:
        f.write(f"{src}\t{dst}\n")
print("  files prepared")

# =====================================================
# PHASE 1: gguf_roundtrip — 6-view RID storage test
# =====================================================
print("\n" + "=" * 60)
print("PHASE 1: gguf_roundtrip (6-view RID storage)")
print("=" * 60)

print("\n== compile gguf_roundtrip ==")
r, dt = sh("cd /content/tools && gcc -O2 -Wall -Icore "
    "-o /content/gguf_roundtrip gguf_roundtrip.c -lm 2>&1",
    60, "compile")
print(f"  compiled in {dt:.1f}s")
if r.stdout.strip():
    print(f"  warnings: {r.stdout.strip()[:200]}")

print("\n== run gguf_roundtrip ==")
r, dt = sh(f"/content/gguf_roundtrip {model_path} /content/dwgls_test.twin",
    300, "roundtrip")
print(r.stdout)
REPORT["phase1_time_s"] = round(dt, 1)
REPORT["phase1_passed"] = "RESULT: PASSED" in r.stdout

# Parse key metrics from output
for line in r.stdout.splitlines():
    if "file:" in line:
        REPORT["roundtrip_parts"] = line.split("·")[1].strip() if "·" in line else ""
    if "R1 BAKE" in line:
        REPORT["r1_bake"] = line.split("·")[-1].strip() if "·" in line else line
    if "R2 REBUILD" in line:
        REPORT["r2_rebuild"] = "byte-identical=YES" in line
    if "R3 DAMAGE" in line:
        REPORT["r3_damage"] = "lossless again" in line
    if "R4 PERSIST" in line:
        REPORT["r4_persist"] = "YES" in line
    if "RESULT" in line:
        REPORT["phase1_result"] = line.strip()

# =====================================================
# PHASE 2: Baseline inference (llama-cpp-python)
# =====================================================
print("\n" + "=" * 60)
print("PHASE 2: Baseline inference (no DWGLS)")
print("=" * 60)

from llama_cpp import Llama
def gpu_mem():
    r = subprocess.check_output(
        ["nvidia-smi", "--query-gpu=memory.used,memory.total",
         "--format=csv,noheader,nounits"]).decode().strip().split(", ")
    return int(r[0]), int(r[1])

print("\n== load model ==")
used0, total = gpu_mem()
t0 = time.time()
llm = Llama(model_path=model_path, n_gpu_layers=99, n_ctx=2048, verbose=False)
load_t = time.time() - t0
used1, _ = gpu_mem()
print(f"  loaded in {load_t:.1f}s, VRAM: {used1} MiB (model={used1-used0} MiB)")

prompt = "The capital of France is"
print(f"\n== baseline inference: '{prompt}' ==")
t0 = time.time()
out = llm(prompt, max_tokens=128, temperature=0)
dt = time.time() - t0
tok = out["usage"]["completion_tokens"]
baseline_text = out["choices"][0]["text"]
print(f"  {tok} tok in {dt:.2f}s = {tok/dt:.1f} tok/s")
print(f"  text: {baseline_text[:100]}")
REPORT["baseline_tok"] = tok
REPORT["baseline_time_s"] = round(dt, 2)
REPORT["baseline_tps"] = round(tok / dt, 1)
REPORT["baseline_text"] = baseline_text[:200]
del llm

# =====================================================
# PHASE 3: geo_rid_graft (RID storage + inference)
# =====================================================
print("\n" + "=" * 60)
print("PHASE 3: geo_rid_graft (RID + llama.cpp inference)")
print("=" * 60)

# Build llama.cpp from source with CUDA
print("\n== build llama.cpp (CUDA) ==")
if os.path.isfile("/content/lc/build/src/libllama.a"):
    print("  cached build found")
else:
    print("  cloning b9733...")
    r, dt = sh("git clone --depth 1 --branch b9733 "
        "https://github.com/ggml-org/llama.cpp /content/lc 2>&1 | tail -1", 300)
    print(f"  cloned in {dt:.0f}s")

    print("  configuring CUDA sm_75...")
    r, dt = sh("cd /content/lc && cmake -B build "
        "-DBUILD_SHARED_LIBS=OFF -DGGML_NATIVE=ON -DLLAMA_CURL=OFF "
        "-DGGML_CUDA=ON -DCMAKE_CUDA_ARCHITECTURES=75 "
        "-DCMAKE_C_FLAGS=-I/content -DCMAKE_CXX_FLAGS=-I/content "
        "-DCMAKE_CUDA_FLAGS=-I/content 2>&1 | tail -2", 120)
    print(f"  configured in {dt:.0f}s")

    print("  building (this takes ~20-30 min)...")
    t0 = time.time()
    r, dt = sh("cd /content/lc && cmake --build build --target llama ggml -j2 "
        "2>&1 | tail -5", 3300)
    print(f"  built in {dt:.0f}s ({(time.time()-t0)/60:.1f} min)")
    if r.returncode != 0:
        print(f"  BUILD FAILED: {r.stderr[-500:]}")
        REPORT["phase3_build_ok"] = False
    else:
        REPORT["phase3_build_ok"] = True

if os.path.isfile("/content/lc/build/src/libllama.a"):
    print("\n== compile geo_rid_graft (CUDA) ==")
    r, dt = sh("cd /content/tools && g++ -O2 -I/content -I/content/lc/include "
        "-I/content/lc/ggml/include "
        "-o /content/rid_graft geo_rid_graft.c "
        "/content/lc/build/src/libllama.a "
        "/content/lc/build/ggml/src/libggml.a "
        "/content/lc/build/ggml/src/libggml-base.a "
        "/content/lc/build/ggml/src/libggml-cpu.a "
        "/content/lc/build/ggml/src/libggml-cuda.a "
        "-L/usr/local/cuda/lib64 -lcudart "
        "-lm -lpthread -ldl -fopenmp -s 2>&1 | grep -E 'error|undefined'",
        300, "link")
    if r.stdout.strip():
        print(f"  LINK ERRORS: {r.stdout}")
        REPORT["phase3_link_ok"] = False
    else:
        print(f"  linked clean in {dt:.1f}s")
        REPORT["phase3_link_ok"] = True

    print("\n== run rid_graft (Gates A-D) ==")
    r, dt = sh(f"/content/rid_graft {model_path} "
        f"'The capital of France is' 16 /content/reg.twin",
        600, "rid_graft")
    for line in r.stdout.splitlines():
        if any(k in line for k in ["A BAKE", "B FOLD", "C INFER", "D DRILL",
                                     "RESULT", "RID base", "tokens",
                                     "logits", "bitwise"]):
            print(f"  {line}")
    REPORT["phase3_time_s"] = round(dt, 1)
    REPORT["phase3_result"] = r.stdout

    # Extract inference comparison
    for line in r.stdout.splitlines():
        if "C INFER" in line:
            REPORT["c_infer"] = line.strip()
        if "tokens" in line.lower() and "identical" in line.lower():
            REPORT["tokens_identical"] = True
        if "bitwise" in line.lower():
            REPORT["logits_bitwise"] = True
else:
    print("  llama.cpp not built — skipping Phase 3")
    REPORT["phase3_skipped"] = True

# =====================================================
# SUMMARY
# =====================================================
print("\n" + "=" * 60)
print("REPORT")
print("=" * 60)

print(f"\nGPU: {REPORT.get('gpu', 'N/A')}")
print(f"Model: {REPORT.get('model', 'N/A')} ({REPORT.get('model_size_mb', '?')} MB)")

print(f"\n--- Phase 1: gguf_roundtrip ---")
print(f"  Result: {REPORT.get('phase1_result', 'N/A')}")
print(f"  Time: {REPORT.get('phase1_time_s', '?')}s")
print(f"  R1 bake+readback: {REPORT.get('r1_bake', 'N/A')}")
print(f"  R2 rebuild byte-identical: {REPORT.get('r2_rebuild', 'N/A')}")
print(f"  R3 damage localize+restore: {REPORT.get('r3_damage', 'N/A')}")
print(f"  R4 persistence: {REPORT.get('r4_persist', 'N/A')}")

print(f"\n--- Phase 2: Baseline inference ---")
print(f"  {REPORT.get('baseline_tps', '?')} tok/s, {REPORT.get('baseline_tok', '?')} tok in {REPORT.get('baseline_time_s', '?')}s")
print(f"  text: {REPORT.get('baseline_text', '')[:100]}")

if not REPORT.get("phase3_skipped"):
    print(f"\n--- Phase 3: DWGLS geo_rid_graft ---")
    print(f"  Build: {REPORT.get('phase3_build_ok', 'N/A')}")
    print(f"  Link: {REPORT.get('phase3_link_ok', 'N/A')}")
    print(f"  Time: {REPORT.get('phase3_time_s', '?')}s")
    print(f"  Tokens identical: {REPORT.get('tokens_identical', 'N/A')}")
    print(f"  Logits bitwise: {REPORT.get('logits_bitwise', 'N/A')}")

# Save full report
with open("/content/dwgls_report.json", "w") as f:
    json.dump(REPORT, f, indent=2, default=str)
print(f"\nFull report saved to /content/dwgls_report.json")
