import base64, os

with open(r"I:\DWGLS-native-fs\bench\tess_scatter_bench_v2.cu", "rb") as f:
    kernel_b64 = base64.b64encode(f.read()).decode()

script = f'''import subprocess, base64, os, time

d = base64.b64decode("{kernel_b64}")
with open("/content/v2.cu", "wb") as f: f.write(d)
print(f"Kernel: {{len(d)}} bytes")

# Compile
r = subprocess.run(["nvcc", "-O3", "-std=c++17", "-arch=sm_75", "-o", "/content/v2", "/content/v2.cu", "-lm"],
    capture_output=True, text=True, timeout=300)
if r.returncode != 0:
    print(f"FAILED: {{r.stderr[:1000]}}")
else:
    print(f"Compiled: {{os.path.getsize('/content/v2')}} bytes")

# Check tesspack exists
if not os.path.exists("/content/model.tesspack"):
    print("ERROR: model.tesspack not found")
else:
    tp = os.path.getsize("/content/model.tesspack")
    print(f"Tesspack: {{tp/1e6:.0f}} MB")

    # Run
    r = subprocess.run(["/content/v2", "/content/model.tesspack"],
        capture_output=True, timeout=300)
    print(r.stdout.decode("utf-8", errors="replace"))
    err = r.stderr.decode("utf-8", errors="replace")
    if err.strip(): print("STDERR:", err[:500])
'''

with open(r"C:\Users\Administrator.AVENTADOR\AppData\Local\Temp\opencode\deploy_v2_scatter.py", "w") as f:
    f.write(script)
print("OK")
