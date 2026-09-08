import base64, os

files = {
    "geo_octant.h": r"I:\DWGLS-native-fs\core\geo_octant.h",
    "geo_tess_wiring.h": r"I:\DWGLS-native-fs\core\geo_tess_wiring.h",
    "geo_voronoi_mask.h": r"I:\DWGLS-native-fs\core\geo_voronoi_mask.h",
}

with open(r"I:\DWGLS-native-fs\bench\tess_scatter_bench.cu", "rb") as f:
    kernel_b64 = base64.b64encode(f.read()).decode()
b64s = {}
for name, path in files.items():
    with open(path, "rb") as f:
        b64s[name] = base64.b64encode(f.read()).decode()
with open(r"I:\DWGLS-native-fs\tools\tess_gguf_pack.c", "rb") as f:
    pack_b64 = base64.b64encode(f.read()).decode()
with open(r"I:\DWGLS-native-fs\core\gguf_reader.h", "rb") as f:
    gguf_b64 = base64.b64encode(f.read()).decode()
with open(r"I:\DWGLS-native-fs\core\geo_tess_container.h", "rb") as f:
    tess_b64 = base64.b64encode(f.read()).decode()

script = f'''import subprocess, base64, os, time

def wf(name, b64d):
    d = base64.b64decode(b64d)
    with open(name, "wb") as f: f.write(d)
    print(f"  {{name}}: {{len(d)}} bytes")

print("="*60)
print("  FULL PIPELINE: Qwen3.5-2B → tess_gguf_pack → GPU Scatter")
print("="*60)

print("\\n=== Step 1: Write source files ===")
wf("/content/tess_scatter_bench.cu", "{kernel_b64}")
wf("/content/tess_gguf_pack.c", "{pack_b64}")
wf("/content/gguf_reader.h", "{gguf_b64}")
wf("/content/geo_tess_container.h", "{tess_b64}")
wf("/content/geo_octant.h", "{b64s['geo_octant.h']}")
wf("/content/geo_tess_wiring.h", "{b64s['geo_tess_wiring.h']}")
wf("/content/geo_voronoi_mask.h", "{b64s['geo_voronoi_mask.h']}")

print("\\n=== Step 2: Download model ===")
t0 = time.time()
# Try multiple models
urls = [
    "https://huggingface.co/unsloth/Qwen3.5-2B-GGUF/resolve/main/Qwen3.5-2B-Q8_0.gguf",
    "https://huggingface.co/unsloth/Qwen3-4B-GGUF/resolve/main/Qwen3-4B-Q4_K_M.gguf",
]
for url in urls:
    print(f"  Trying: {{url.split('/')[-1]}}")
    r = subprocess.run(["wget", "-q", "-O", "/content/model.gguf", url], timeout=600)
    if r.returncode == 0 and os.path.exists("/content/model.gguf"):
        sz = os.path.getsize("/content/model.gguf")
        if sz > 100_000_000:
            elapsed = time.time() - t0
            print(f"  Downloaded: {{sz/1e6:.0f}} MB in {{elapsed:.1f}}s")
            break
else:
    print("  FAILED"); exit(1)

print("\\n=== Step 3: GGUF → .tesspack (single step) ===")
t0 = time.time()
r = subprocess.run(["gcc", "-O2", "-Wall", "-I/content", "-o", "/content/tess_gguf_pack", "/content/tess_gguf_pack.c", "-lm"],
    capture_output=True, text=True, timeout=60)
if r.returncode != 0:
    print(f"  Compile FAILED: {{r.stderr[:500]}}"); exit(1)
print(f"  tess_gguf_pack: compiled")

t0 = time.time()
r = subprocess.run(["/content/tess_gguf_pack", "/content/model.gguf", "/content/model.tesspack"],
    capture_output=True, text=True, timeout=600)
elapsed = time.time() - t0
if r.returncode != 0:
    print(f"  Pack FAILED: {{r.stdout[:500]}}")
    print(f"  stderr: {{r.stderr[:500]}}")
    exit(1)
print(r.stdout[:500])
tp_sz = os.path.getsize("/content/model.tesspack")
gg_sz = os.path.getsize("/content/model.gguf")
print(f"  Tesspack: {{tp_sz/1e6:.1f}} MB (overhead: {{(tp_sz/gg_sz-1)*100:.1f}}%) in {{elapsed:.1f}}s")

print("\\n=== Step 4: Compile CUDA kernel ===")
r = subprocess.run(["nvcc", "-O3", "-std=c++17", "-arch=sm_75", "-DWARMUP",
    "-o", "/content/scatter", "/content/tess_scatter_bench.cu", "-lm"],
    capture_output=True, text=True, timeout=300)
if r.returncode != 0:
    print(f"  FAILED: {{r.stderr[:1000]}}"); exit(1)
print(f"  OK: {{os.path.getsize('/content/scatter')}} bytes")

print("\\n=== Step 5: GPU Scatter Bench (REAL MODEL) ===")
r = subprocess.run(["/content/scatter", "/content/model.tesspack", "/dev/null"],
    capture_output=True, timeout=300)
print(r.stdout.decode("utf-8", errors="replace"))
err = r.stderr.decode("utf-8", errors="replace")
if err.strip() and "cuda" not in err.lower():
    print("STDERR:", err[:500])

print("\\n=== DONE ===")
'''

with open(r"C:\Users\Administrator.AVENTADOR\AppData\Local\Temp\opencode\deploy_v4.py", "w") as f:
    f.write(script)
print(f"Written: {os.path.getsize(r'C:\\Users\\Administrator.AVENTADOR\\AppData\\Local\\Temp\\opencode\\deploy_v4.py')} bytes")
