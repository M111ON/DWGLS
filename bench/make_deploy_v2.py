import base64

kernel_b64 = open(r"C:\Users\Administrator.AVENTADOR\AppData\Local\Temp\opencode\kernel_b64.txt").read().strip()

# Re-encode the fixed kernel
with open(r"I:\DWGLS-native-fs\bench\tess_scatter_bench.cu", "r") as f:
    fixed_kernel = f.read()
fixed_b64 = base64.b64encode(fixed_kernel.encode()).decode()

deploy = f'''import subprocess, base64, struct, os, random

print("=== Step 1: Write FIXED kernel ===")
kernel_src = base64.b64decode("{fixed_b64}").decode()
with open("/content/tess_scatter_bench.cu", "w") as f:
    f.write(kernel_src)
print(f"Kernel: {{len(kernel_src)}} bytes")

print("\\n=== Step 2: Generate LARGE synthetic tesspack (500 MB) ===")
random.seed(42)
N_CAPOS = 3000
entries = []
data_parts = []
offset = 64
for i in range(N_CAPOS):
    name = f"tensor_{{i:04d}}"
    nb = name.encode()
    sz = random.randint(4096, 32768)
    sz = (sz + 63) & ~63
    entries.append((nb, i, offset, sz))
    data_parts.append(bytes(random.getrandbits(8) for _ in range(sz)))
    offset += sz
with open("/content/test_large.tesspack", "wb") as f:
    hdr = struct.pack("<IIIQ", 0x5450414B, 1, N_CAPOS, offset)
    hdr += b"\\x00" * (64 - len(hdr))
    f.write(hdr)
    for d in data_parts:
        f.write(d)
    for nb, cid, coff, sz in entries:
        f.write(struct.pack("B", len(nb)))
        f.write(nb)
        f.write(struct.pack("<IQI", cid, coff, sz))
total = os.path.getsize("/content/test_large.tesspack")
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
    sz = os.path.getsize("/content/tess_scatter_bench")
    print(f"OK: {{sz}} bytes")

    print("\\n=== Step 4: Run benchmark ===")
    r2 = subprocess.run(["/content/tess_scatter_bench",
        "/content/test_large.tesspack", "/dev/null"],
        capture_output=True, timeout=120)
    print(r2.stdout.decode("utf-8", errors="replace"))
    if r2.stderr:
        err = r2.stderr.decode("utf-8", errors="replace")
        if err.strip():
            print("STDERR:", err[:500])
'''

with open(r"C:\Users\Administrator.AVENTADOR\AppData\Local\Temp\opencode\deploy_v2.py", "w") as f:
    f.write(deploy)

print(f"Written deploy_v2.py: {len(deploy)} bytes")
