#!/bin/bash
# Test: batched vs separate instances for multi-view inference.
set -e
export PATH="$HOME/.local/bin:$PATH"
S=dwgls-batch

colab new -s "$S" --gpu T4

cat > /tmp/dwgls_batch_test.py << 'PYEOF'
import subprocess, os, sys, time, threading

print("== install ==")
subprocess.run([sys.executable,"-m","pip","install","-q",
    "llama-cpp-python",
    "--extra-index-url","https://abetlen.github.io/llama-cpp-python/whl/cu124"],
    check=True)

print("== download model ==")
murl="https://huggingface.co/Qwen/Qwen2.5-0.5B-Instruct-GGUF/resolve/main/qwen2.5-0.5b-instruct-q8_0.gguf"
subprocess.run(["wget","-q","-O","/content/model.gguf",murl],check=True)

def gpu():
    out = subprocess.check_output(["nvidia-smi","--query-gpu=memory.used,memory.total",
        "--format=csv,noheader,nounits"]).decode().strip().split(", ")
    return int(out[0]), int(out[1])

from llama_cpp import Llama

# 6 DWGLS language views
VIEWS = [
    ("pent", "Explain pentagonal tiling in geometry"),
    ("tri", "Explain triangular tessellation in geometry"),
    ("snubL", "Explain snub dodecahedron left enantiomorph"),
    ("snubR", "Explain snub dodecahedron right enantiomorph"),
    ("hosoya", "Explain Hosoya index in graph theory"),
    ("zeck", "Explain Zeckendorf representation of integers"),
]

# --- Approach A: 6 separate instances (threaded) ---
print("\n=== APPROACH A: 6 separate instances ===")
llms_a = []
for i in range(6):
    llms_a.append(Llama(model_path="/content/model.gguf", n_gpu_layers=99, n_ctx=512, verbose=False))
used_a, total = gpu()
print(f"  VRAM: {used_a}/{total} MiB (6 model copies)")

results_a = [None]*6
def bench_a(idx, llm, prompt):
    t0=time.time()
    out = llm(prompt, max_tokens=128, temperature=0)
    dt=time.time()-t0
    results_a[idx] = {"tok":out["usage"]["completion_tokens"], "dt":dt}

t0=time.time()
threads = [threading.Thread(target=bench_a, args=(i, llms_a[i], v[1])) for i,v in enumerate(VIEWS)]
for t in threads: t.start()
for t in threads: t.join()
wall_a = time.time()-t0

for i,r in enumerate(results_a):
    print(f"  [{VIEWS[i][0]:>6}] {r['tok']:>3} tok {r['dt']:.2f}s = {r['tok']/r['dt']:.1f} tok/s")
agg_a = sum(r['tok'] for r in results_a)/wall_a
print(f"  wall={wall_a:.2f}s aggregate={agg_a:.1f} tok/s")

# cleanup
del llms_a
import gc; gc.collect()
time.sleep(1)

# --- Approach B: 1 instance, sequential batch ---
print("\n=== APPROACH B: 1 instance, sequential batch ===")
llm_b = Llama(model_path="/content/model.gguf", n_gpu_layers=99, n_ctx=512, verbose=False)
used_b, _ = gpu()
print(f"  VRAM: {used_b}/{total} MiB (1 model copy)")

results_b = [None]*6
t0=time.time()
for i,(name,prompt) in enumerate(VIEWS):
    out = llm_b(prompt, max_tokens=128, temperature=0)
    results_b[i] = {"tok":out["usage"]["completion_tokens"], "dt":0}
wall_b = time.time()-t0

# re-measure individual times
llm_b2 = Llama(model_path="/content/model.gguf", n_gpu_layers=99, n_ctx=512, verbose=False)
for i,(name,prompt) in enumerate(VIEWS):
    t0=time.time()
    out = llm_b2(prompt, max_tokens=128, temperature=0)
    dt=time.time()-t0
    results_b[i] = {"tok":out["usage"]["completion_tokens"], "dt":dt}

for i,r in enumerate(results_b):
    print(f"  [{VIEWS[i][0]:>6}] {r['tok']:>3} tok {r['dt']:.2f}s = {r['tok']/r['dt']:.1f} tok/s")
agg_b = sum(r['tok'] for r in results_b)/sum(r['dt'] for r in results_b)
print(f"  wall(sequential)={sum(r['dt'] for r in results_b):.2f}s aggregate_throughput={agg_b:.1f} tok/s")

# --- Approach C: 1 instance, prompt concatenation batch ---
print("\n=== APPROACH C: 1 instance, concatenated prompts ===")
llm_c = Llama(model_path="/content/model.gguf", n_gpu_layers=99, n_ctx=2048, verbose=False)
used_c, _ = gpu()
print(f"  VRAM: {used_c}/{total} MiB (1 model, larger ctx)")

# concatenate all prompts with separator
combined = "\n---\n".join([f"Q{i}: {v[1]}" for i,v in enumerate(VIEWS)])
t0=time.time()
out_c = llm_c(combined, max_tokens=768, temperature=0)
dt=time.time()-t0
agg_c = out_c["usage"]["completion_tokens"]/dt
print(f"  combined prompt ({len(combined)} chars)")
print(f"  {out_c['usage']['completion_tokens']} tok in {dt:.2f}s = {agg_c:.1f} tok/s")
print(f"  output preview: {out_c['choices'][0]['text'][:200]}...")

# --- Summary ---
used_final, _ = gpu()
print("\n=== SUMMARY ===")
print(f"A (6 instances): VRAM={used_a}MiB aggregate={agg_a:.1f} tok/s")
print(f"B (1 inst seq):  VRAM={used_b}MiB aggregate={agg_b:.1f} tok/s")
print(f"C (1 inst batch):VRAM={used_c}MiB aggregate={agg_c:.1f} tok/s")
print(f"VRAM savings: A→C = {used_a-used_c}MiB ({(1-used_c/used_a)*100:.0f}%)")
PYEOF

colab exec -s "$S" --timeout 600 -f /tmp/dwgls_batch_test.py
colab stop -s "$S"
