import subprocess, os, sys, time

print("== install ==")
subprocess.run([sys.executable,"-m","pip","install","-q",
    "llama-cpp-python","huggingface_hub",
    "--extra-index-url","https://abetlen.github.io/llama-cpp-python/whl/cu124"],
    check=True)

def gpu():
    out = subprocess.check_output(["nvidia-smi","--query-gpu=name,memory.used,memory.total",
        "--format=csv,noheader,nounits"]).decode().strip()
    parts = [x.strip() for x in out.split(",")]
    return parts[0], int(parts[1]), int(parts[2])

from llama_cpp import Llama
from huggingface_hub import hf_hub_download

name,gpu_used,gpu_total = gpu()
print(f"\n=== GPU: {name} {gpu_used}/{gpu_total} MiB ===")

print("\n=== Qwen2.5-7B-Instruct Q4_K_M (GPU) ===")
model_path = hf_hub_download(
    repo_id="Qwen/Qwen2.5-7B-Instruct-GGUF",
    filename="qwen2.5-7b-instruct-q4_k_m-00001-of-00002.gguf")
hf_hub_download(
    repo_id="Qwen/Qwen2.5-7B-Instruct-GGUF",
    filename="qwen2.5-7b-instruct-q4_k_m-00002-of-00002.gguf")
print(f"model ready")

_,used0,_ = gpu()
t0=time.time()
llm = Llama(model_path=model_path, n_gpu_layers=99, n_ctx=2048, verbose=False)
load_t=time.time()-t0
_,used1,_ = gpu()
print(f"loaded in {load_t:.1f}s, VRAM: {used1} MiB (model={used1-used0} MiB)")

prompts = [
    "The capital of France is",
    "Explain neural networks in one sentence",
    "What is the difference between HTTP and HTTPS?",
    "Write a haiku about mountains",
    "Explain quantum entanglement briefly",
    "What are the benefits of exercise?",
]

print("\n--- single prompt ---")
total_tok=0; total_dt=0
for p in prompts:
    t0=time.time()
    out = llm(p, max_tokens=128, temperature=0)
    dt=time.time()-t0
    tok = out["usage"]["completion_tokens"]
    total_tok+=tok; total_dt+=dt
    print(f"  {tok:>3} tok {dt:.2f}s = {tok/dt:.1f} tok/s | {out['choices'][0]['text'][:70]}")
print(f"  TOTAL: {total_tok} tok in {total_dt:.2f}s = {total_tok/total_dt:.1f} tok/s")

print("\n--- batch (6 prompts concatenated) ---")
combined = "\n---\n".join([f"Q{i}: {p}" for i,p in enumerate(prompts)])
t0=time.time()
out = llm(combined, max_tokens=768, temperature=0)
dt=time.time()-t0
tok = out["usage"]["completion_tokens"]
print(f"  {tok} tok in {dt:.2f}s = {tok/dt:.1f} tok/s")
print(f"  output preview: {out['choices'][0]['text'][:400]}...")

print("\n--- DWGLS 6-view batch ---")
VIEWS = [
    ("pent", "Explain pentagonal tiling in geometry"),
    ("tri", "Explain triangular tessellation in geometry"),
    ("snubL", "Explain snub dodecahedron left enantiomorph"),
    ("snubR", "Explain snub dodecahedron right enantiomorph"),
    ("hosoya", "Explain golden spiral phyllotaxis"),
    ("zeck", "Explain Zeckendorf representation of integers"),
]
dwgls_prompt = "\n---\n".join([f"View {name}: {p}" for name,p in VIEWS])

t0=time.time()
out = llm(dwgls_prompt, max_tokens=1024, temperature=0)
dt=time.time()-t0
tok = out["usage"]["completion_tokens"]
print(f"  {tok} tok in {dt:.2f}s = {tok/dt:.1f} tok/s")
print(f"  output:\n{out['choices'][0]['text']}")

_,used_final,_ = gpu()
print(f"\n=== FINAL VRAM: {used_final}/{gpu_total} MiB ===")
