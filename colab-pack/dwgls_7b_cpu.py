import subprocess, os, sys, time

print("== install ==")
subprocess.run([sys.executable,"-m","pip","install","-q",
    "llama-cpp-python","huggingface_hub"],check=True)

from llama_cpp import Llama
from huggingface_hub import hf_hub_download

# Qwen2.5-7B Q4_K_M is split into 2 parts - hf_hub_download handles this
print("\n=== Qwen2.5-7B-Instruct Q4_K_M (CPU) ===")
model_path = hf_hub_download(
    repo_id="Qwen/Qwen2.5-7B-Instruct-GGUF",
    filename="qwen2.5-7b-instruct-q4_k_m-00001-of-00002.gguf")
model_path2 = hf_hub_download(
    repo_id="Qwen/Qwen2.5-7B-Instruct-GGUF",
    filename="qwen2.5-7b-instruct-q4_k_m-00002-of-00002.gguf")
print(f"parts downloaded: {os.path.getsize(model_path)/1e6:.1f} + {os.path.getsize(model_path2)/1e6:.1f} MB")

# For split GGUF, we need the first part as the main file
# llama.cpp / llama-cpp-python should auto-detect the split
t0=time.time()
llm = Llama(model_path=model_path, n_gpu_layers=0, n_ctx=1024, verbose=False)
print(f"loaded in {time.time()-t0:.1f}s")

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

print("\n--- batch (6 prompts) ---")
combined = "\n---\n".join([f"Q{i}: {p}" for i,p in enumerate(prompts)])
t0=time.time()
out = llm(combined, max_tokens=384, temperature=0)
dt=time.time()-t0
tok = out["usage"]["completion_tokens"]
print(f"  {tok} tok in {dt:.2f}s = {tok/dt:.1f} tok/s")
print(f"  output preview: {out['choices'][0]['text'][:300]}...")
