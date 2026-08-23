#!/bin/bash
# Test: multiple LLM instances concurrent on single T4 GPU.
set -e
export PATH="$HOME/.local/bin:$PATH"
S=dwgls-multi-gpu

colab new -s "$S" --gpu T4

cat > /tmp/dwgls_multi_test.py << 'PYEOF'
import subprocess, os, sys, time, threading

print("== install ==")
subprocess.run([sys.executable,"-m","pip","install","-q",
    "llama-cpp-python",
    "--extra-index-url","https://abetlen.github.io/llama-cpp-python/whl/cu124"],
    check=True)

print("== download model ==")
murl="https://huggingface.co/Qwen/Qwen2.5-0.5B-Instruct-GGUF/resolve/main/qwen2.5-0.5b-instruct-q8_0.gguf"
subprocess.run(["wget","-q","-O","/content/model.gguf",murl],check=True)
print(f"model {os.path.getsize('/content/model.gguf')/1e6:.1f} MB")

def gpu():
    out = subprocess.check_output(["nvidia-smi","--query-gpu=memory.used,memory.total",
        "--format=csv,noheader,nounits"]).decode().strip().split(", ")
    return int(out[0]), int(out[1])

from llama_cpp import Llama

used,total = gpu()
print(f"GPU before: {used}/{total} MiB")

# --- serial baseline ---
print("\n=== SERIAL (1 instance) ===")
llm1 = Llama(model_path="/content/model.gguf", n_gpu_layers=99, n_ctx=512, verbose=False)
t0=time.time()
out1 = llm1("The capital of France is", max_tokens=64, temperature=0)
dt=time.time()-t0
tok1 = out1["usage"]["completion_tokens"]
used1,_ = gpu()
print(f"  1 instance: {tok1} tok in {dt:.2f}s = {tok1/dt:.1f} tok/s")
print(f"  output: {out1['choices'][0]['text'][:80]}")
print(f"  VRAM: {used1}/{total} MiB")

# --- concurrent 2 instances ---
print("\n=== CONCURRENT 2 instances ===")
llm2a = Llama(model_path="/content/model.gguf", n_gpu_layers=99, n_ctx=512, verbose=False)
llm2b = Llama(model_path="/content/model.gguf", n_gpu_layers=99, n_ctx=512, verbose=False)
used2,_ = gpu()
print(f"  VRAM after loading 2: {used2}/{total} MiB")

results = [None, None]
def bench2(idx, llm, prompt):
    t0=time.time()
    out = llm(prompt, max_tokens=128, temperature=0)
    dt=time.time()-t0
    results[idx] = {"tok":out["usage"]["completion_tokens"], "dt":dt}

t0=time.time()
threads = [
    threading.Thread(target=bench2, args=(0, llm2a, "Explain neural network in one sentence")),
    threading.Thread(target=bench2, args=(1, llm2b, "Explain quantum computing in one sentence")),
]
for t in threads: t.start()
for t in threads: t.join()
wall = time.time()-t0

for i,r in enumerate(results):
    print(f"  [{i}] {r['tok']} tok in {r['dt']:.2f}s = {r['tok']/r['dt']:.1f} tok/s")
agg = sum(r['tok'] for r in results)/wall
print(f"  wall={wall:.2f}s aggregate={agg:.1f} tok/s")

# --- concurrent 3 instances ---
print("\n=== CONCURRENT 3 instances ===")
llm3a = Llama(model_path="/content/model.gguf", n_gpu_layers=99, n_ctx=512, verbose=False)
llm3b = Llama(model_path="/content/model.gguf", n_gpu_layers=99, n_ctx=512, verbose=False)
llm3c = Llama(model_path="/content/model.gguf", n_gpu_layers=99, n_ctx=512, verbose=False)
used3,_ = gpu()
print(f"  VRAM after loading 3: {used3}/{total} MiB")

results3 = [None, None, None]
def bench3(idx, llm, prompt):
    t0=time.time()
    out = llm(prompt, max_tokens=128, temperature=0)
    dt=time.time()-t0
    results3[idx] = {"tok":out["usage"]["completion_tokens"], "dt":dt}

t0=time.time()
threads3 = [
    threading.Thread(target=bench3, args=(0, llm3a, "Explain CPU and GPU difference")),
    threading.Thread(target=bench3, args=(1, llm3b, "Explain HTTP and HTTPS difference")),
    threading.Thread(target=bench3, args=(2, llm3c, "Explain SQL and NoSQL difference")),
]
for t in threads3: t.start()
for t in threads3: t.join()
wall3 = time.time()-t0

for i,r in enumerate(results3):
    print(f"  [{i}] {r['tok']} tok in {r['dt']:.2f}s = {r['tok']/r['dt']:.1f} tok/s")
agg3 = sum(r['tok'] for r in results3)/wall3
print(f"  wall={wall3:.2f}s aggregate={agg3:.1f} tok/s")

# --- cleanup + summary ---
used_final,_ = gpu()
del llm1, llm2a, llm2b, llm3a, llm3b, llm3c
import gc; gc.collect()
used_after,_ = gpu()

print("\n=== SUMMARY ===")
print(f"serial:       tok/s={tok1/dt:.1f}  VRAM={used1}MiB")
print(f"concurrent 2: per-inst={agg/2:.1f} aggregate={agg:.1f}  VRAM={used2}MiB")
print(f"concurrent 3: per-inst={agg3/3:.1f} aggregate={agg3:.1f}  VRAM={used3}MiB")
print(f"after cleanup: {used_after}/{total} MiB")
PYEOF

colab exec -s "$S" --timeout 600 -f /tmp/dwgls_multi_test.py
colab stop -s "$S"
