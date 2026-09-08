import base64

with open(r"I:\DWGLS-native-fs\bench\tess_scatter_bench.cu", "r") as f:
    fixed_kernel = f.read()
fixed_b64 = base64.b64encode(fixed_kernel.encode()).decode()

deploy = f'''import subprocess, base64, struct, os, random

print("=== Step 1: Write FIXED kernel ===")
kernel_src = base64.b64decode("{fixed_b64}").decode()
with open("/content/tess_scatter_bench.cu", "w") as f:
    f.write(kernel_src)
print(f"Kernel: {{len(kernel_src)}} bytes")

print("\\n=== Step 2: Generate 1 GB tesspack (os.urandom for speed) ===")
random.seed(42)
N_CAPOS = 8000
entries = []
offset = 64
for i in range(N_CAPOS):
    name = f"tensor_{{i:05d}}"
    nb = name.encode()
    sz = random.randint(8192, 262144)
    sz = (sz + 63) & ~63
    entries.append((nb, i, offset, sz))
    offset += sz

with open("/content/test_1gb.tesspack", "wb") as f:
    hdr = struct.pack("<IIIQ", 0x5450414B, 1, N_CAPOS, offset)
    hdr += b"\\x00" * (64 - len(hdr))
    f.write(hdr)
    for nb, cid, coff, sz in entries:
        f.write(os.urandom(sz))
    for nb, cid, coff, sz in entries:
        f.write(struct.pack("B", len(nb)))
        f.write(nb)
        f.write(struct.pack("<IQI", cid, coff, sz))
total = os.path.getsize("/content/test_1gb.tesspack")
print(f"Tesspack: {{N_CAPOS}} capos, {{total/1e6:.1f}} MB")

print("\\n=== Step 3: Compile ===")
r = subprocess.run(["nvcc", "-O3", "-std=c++17", "-arch=sm_75",
    "-DWARMUP", "-o", "/content/tess_scatter_bench",
    "/content/tess_scatter_bench.cu", "-lm"],
    capture_output=True, text=True, timeout=300)
if r.returncode != 0:
    print("COMPILE FAILED:")
    print(r.stderr[:2000])
else:
    print(f"OK: {{os.path.getsize('/content/tess_scatter_bench')}} bytes")

    print("\\n=== Step 4: Run benchmark (all capos) ===")
    r2 = subprocess.run(["/content/tess_scatter_bench",
        "/content/test_1gb.tesspack", "/dev/null"],
        capture_output=True, timeout=300)
    print(r2.stdout.decode("utf-8", errors="replace"))
    if r2.stderr:
        err = r2.stderr.decode("utf-8", errors="replace")
        if err.strip():
            print("STDERR:", err[:500])
'''

with open(r"C:\Users\Administrator.AVENTADOR\AppData\Local\Temp\opencode\deploy_1gb.py", "w") as f:
    f.write(deploy)
print(f"Written: {len(deploy)} bytes")
