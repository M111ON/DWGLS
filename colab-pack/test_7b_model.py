#!/bin/bash
# Test: Qwen2.5-7B on T4 + batch performance.
set -e
export PATH="$HOME/.local/bin:$PATH"
S=dwgls-7b

colab new -s "$S" --gpu T4

cat > /tmp/dwgls_7b_test.py << 'PYEOF'
import subprocess, os, sys, time

print("== install ==")
subprocess.run([sys.executable,"-m","pip","install","-q",
    "llama-cpp-python","huggingface_hub",
    "--extra-index-url","https://abetlen.github.io/llama-cpp-python/whl/cu124"],
    check=True)

def gpu():
    out = subprocess.check_output(["nvidia-smi","--query-gpu=memory.used,memory.total",
        "--format=csv,noheader,nounits"]).decode().strip().split(", ")
    return int(out[0]), int(out[1])

from llama_cpp import Llama
from huggingface_hub import hf_hub_download

# --- Download Qwen2.5-7B Q4_K_M ---
print("\n=== Qwen2.5-7B-Instruct Q4_K_M ===")
model_path = hf_hub_download(
    repo_id="Qwen/Qwen2.5-7B-Instruct-GGUF",
    filename="qwen2.5-7b-instruct-q4_k_m.gguf")
print(f"model {os.path.getsize(model_path)/1e6:.1f} MB")

used0,total = gpu()
print(f"VRAM before: {used0}/{total} MiB")

llm = Llama(model_path=model_path, n_gpu_layers=99, n_ctx=2048, verbose=False)
used1,_ = gpu()
print(f"VRAM after load: {used1}/{total} MiB (model={used1-used0} MiB)")

# single prompts
prompts = [
    "The capital of France is",
    "Explain neural networks in one sentence",
    "What is the difference between HTTP and HTTPS?",
    "Write a haiku about mountains",
    "Explain quantum entanglement briefly",
    "What are the benefits of exercise?",
]

print("\n--- single prompt ---")
for p in prompts:
    t0=time.time()
    out = llm(p, max_tokens=64, temperature=0)
    dt=time.time()-t0
    tok = out["usage"]["completion_tokens"]
    print(f"  {tok:>3} tok {dt:.2f}s = {tok/dt:.1f} tok/s | {out['choices'][0]['text'][:60]}")

# batch concatenated
print("\n--- batch concatenated (6 prompts) ---")
combined = "\n---\n".join([f"Q{i}: {p}" for i,p in enumerate(prompts)])
t0=time.time()
out = llm(combined, max_tokens=384, temperature=0)
dt=time.time()-t0
tok = out["usage"]["completion_tokens"]
print(f"  {tok} tok in {dt:.2f}s = {tok/dt:.1f} tok/s")
print(f"  output preview: {out['choices'][0]['text'][:300]}...")

# DWGLS 6-view batch
print("\n=== DWGLS 6-view batch ===")
VIEWS = [
    ("pent", "Explain pentagonal tiling in geometry"),
    ("tri", "Explain triangular tessellation in geometry"),
    ("snub", "Explain snub dodecahedron chiral pair"),
    ("hosoya", "Explain golden spiral phyllotaxis"),
    ("zeck", "Explain Zeckendorf representation"),
    ("goldberg", "Explain Goldberg polyhedra construction"),
]
dwgls_prompt = "\n---\n".join([f"View {name}: {p}" for name,p in VIEWS])

t0=time.time()
out = llm(dwgls_prompt, max_tokens=768, temperature=0)
dt=time.time()-t0
tok = out["usage"]["completion_tokens"]
print(f"  {tok} tok in {dt:.2f}s = {tok/dt:.1f} tok/s")
print(f"  output:\n{out['choices'][0]['text']}")

used_final,_ = gpu()
print(f"\n=== FINAL VRAM: {used_final}/{total} MiB ===")
PYEOF

colab exec -s "$S" --timeout 900 -f /tmp/dwgls_7b_test.py
colab stop -s "$S"
