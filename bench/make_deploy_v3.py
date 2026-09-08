import base64, os

files = {
    "geo_octant.h": r"I:\DWGLS-native-fs\core\geo_octant.h",
    "geo_tess_wiring.h": r"I:\DWGLS-native-fs\core\geo_tess_wiring.h",
    "geo_voronoi_mask.h": r"I:\DWGLS-native-fs\core\geo_voronoi_mask.h",
}

with open(r"C:\Users\Administrator.AVENTADOR\AppData\Local\Temp\opencode\kernel_b64.txt") as f:
    kernel_b64 = f.read().strip()

# Re-read kernel to get fixed version
with open(r"I:\DWGLS-native-fs\bench\tess_scatter_bench.cu", "r") as f:
    kernel_b64 = base64.b64encode(f.read().encode()).decode()

b64s = {}
for name, path in files.items():
    with open(path, "rb") as f:
        b64s[name] = base64.b64encode(f.read()).decode()

with open(r"I:\DWGLS-native-fs\tools\tess_bake.c", "r") as f:
    bake_b64 = base64.b64encode(f.read().encode()).decode()
with open(r"I:\DWGLS-native-fs\tools\tess_gguf_pack.c", "r") as f:
    pack_b64 = base64.b64encode(f.read().encode()).decode()
with open(r"I:\DWGLS-native-fs\core\gguf_reader.h", "r") as f:
    gguf_b64 = base64.b64encode(f.read().encode()).decode()
with open(r"I:\DWGLS-native-fs\core\geo_tess_container.h", "rb") as f:
    tess_b64 = base64.b64encode(f.read()).decode()

script = f'''import subprocess, base64, struct, os, time

def wf(name, b64d):
    d = base64.b64decode(b64d)
    with open(name, "wb") as f: f.write(d)
    print(f"  {{name}}: {{len(d)}} bytes")

print("="*60)
print("  FULL PIPELINE: Qwen3.5-2B → Bake → Pack → GPU Scatter")
print("="*60)

print("\\n=== Step 1: Write source files ===")
wf("/content/tess_scatter_bench.cu", "{kernel_b64}")
wf("/content/tess_bake.c", "{bake_b64}")
wf("/content/tess_gguf_pack.c", "{pack_b64}")
wf("/content/gguf_reader.h", "{gguf_b64}")
wf("/content/geo_tess_container.h", "{tess_b64}")
wf("/content/geo_octant.h", "{b64s['geo_octant.h']}")
wf("/content/geo_tess_wiring.h", "{b64s['geo_tess_wiring.h']}")
wf("/content/geo_voronoi_mask.h", "{b64s['geo_voronoi_mask.h']}")

print("\\n=== Step 2: Download Qwen3.5-2B-Q8_0 ===")
t0 = time.time()
url = "https://huggingface.co/unsloth/Qwen3.5-2B-GGUF/resolve/main/Qwen3.5-2B-Q8_0.gguf"
r = subprocess.run(["wget", "-q", "-O", "/content/model.gguf", url], timeout=600)
if r.returncode != 0 or not os.path.exists("/content/model.gguf"):
    url = "https://huggingface.co/unsloth/Qwen3-4B-GGUF/resolve/main/Qwen3-4B-Q4_K_M.gguf"
    r = subprocess.run(["wget", "-q", "-O", "/content/model.gguf", url], timeout=600)
elapsed = time.time() - t0
if os.path.exists("/content/model.gguf"):
    sz = os.path.getsize("/content/model.gguf")
    print(f"  Downloaded: {{sz/1e6:.0f}} MB in {{elapsed:.1f}}s ({{sz/elapsed/1e6:.0f}} MB/s)")
else:
    print("  FAILED to download model"); exit(1)

print("\\n=== Step 3: Compile tess tools ===")
inc = "-I/content"
for tool, src in [("tess_bake", "tess_bake.c"), ("tess_pack", "tess_gguf_pack.c")]:
    r = subprocess.run(["gcc", "-O2", "-Wall", inc, "-o", f"/content/{{tool}}", f"/content/{{src}}", "-lm"],
        capture_output=True, text=True, timeout=60)
    if r.returncode != 0:
        print(f"  {{tool}} FAILED: {{r.stderr[:500]}}"); exit(1)
    print(f"  {{tool}}: OK")

print("\\n=== Step 4: Bake GGUF → .tess ===")
t0 = time.time()
r = subprocess.run(["/content/tess_bake", "/content/model.gguf", "/content/tess_out"],
    capture_output=True, text=True, timeout=300)
elapsed = time.time() - t0
if r.returncode != 0:
    print(f"  Bake FAILED: {{r.stdout[:500]}}"); print(f"  stderr: {{r.stderr[:500]}}"); exit(1)
n_tess = len([f for f in os.listdir("/content/tess_out") if f.endswith(".tess")])
print(f"  Baked: {{n_tess}} .tess files in {{elapsed:.1f}}s")
print(f"  {{r.stdout[:300]}}")

print("\\n=== Step 5: Pack → .tesspack ===")
t0 = time.time()
r = subprocess.run(["/content/tess_pack", "/content/tess_out", "/content/model.tesspack"],
    capture_output=True, text=True, timeout=300)
elapsed = time.time() - t0
if r.returncode != 0:
    print(f"  Pack FAILED: {{r.stdout[:500]}}"); print(f"  stderr: {{r.stderr[:500]}}"); exit(1)
tp_sz = os.path.getsize("/content/model.tesspack")
gg_sz = os.path.getsize("/content/model.gguf")
print(f"  Tesspack: {{tp_sz/1e6:.1f}} MB (overhead: {{(tp_sz/gg_sz-1)*100:.1f}}%) in {{elapsed:.1f}}s")

print("\\n=== Step 6: Compile CUDA kernel ===")
r = subprocess.run(["nvcc", "-O3", "-std=c++17", "-arch=sm_75", "-DWARMUP",
    "-o", "/content/tess_scatter_bench", "/content/tess_scatter_bench.cu", "-lm"],
    capture_output=True, text=True, timeout=300)
if r.returncode != 0:
    print(f"  CUDA FAILED: {{r.stderr[:1000]}}"); exit(1)
print(f"  CUDA OK: {{os.path.getsize('/content/tess_scatter_bench')}} bytes")

print("\\n=== Step 7: GPU Scatter Bench (REAL MODEL) ===")
r = subprocess.run(["/content/tess_scatter_bench", "/content/model.tesspack", "/dev/null"],
    capture_output=True, timeout=300)
print(r.stdout.decode("utf-8", errors="replace"))
if r.stderr:
    err = r.stderr.decode("utf-8", errors="replace")
    if err.strip(): print("STDERR:", err[:500])

print("\\n=== DONE ===")
print(f"  GGUF:   {{gg_sz/1e6:.1f}} MB")
print(f"  Tesspack: {{tp_sz/1e6:.1f}} MB")
print(f"  Capos:  {{n_tess}}")
print(f"  GPU:    Tesla T4")
'''

with open(r"C:\Users\Administrator.AVENTADOR\AppData\Local\Temp\opencode\deploy_v3.py", "w") as f:
    f.write(script)
print(f"Written: {os.path.getsize(r'C:\\Users\\Administrator.AVENTADOR\\AppData\\Local\\Temp\\opencode\\deploy_v3.py')} bytes")
