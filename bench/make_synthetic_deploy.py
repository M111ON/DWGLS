import subprocess, struct, os, random

# Step 1: Generate synthetic tesspack on Colab (no upload needed)
script = r'''
import struct, os, random

random.seed(42)
N_CAPOS = 1871
CAPO_MIN = 2048
CAPO_MAX = 16384

print("Generating synthetic tesspack...")

# Generate capo entries
entries = []
data_parts = []
offset = 64  # skip header

for i in range(N_CAPOS):
    name = f"tensor_{i:04d}"
    name_bytes = name.encode()
    size = random.randint(CAPO_MIN, CAPO_MAX)
    size = (size + 63) & ~63  # align to 64 bytes
    entries.append((name_bytes, i, offset, size))
    # Random capo data
    data_parts.append(os.urandom(size))
    offset += size

index_offset = offset

# Build file
with open("/content/test.tesspack", "wb") as f:
    # Header: magic(4) + version(4) + n_capos(4) + index_offset(8) + pad(44)
    hdr = struct.pack("<IIIQ", 0x5450414B, 1, N_CAPOS, index_offset)
    hdr += b"\x00" * (64 - len(hdr))
    f.write(hdr)
    
    # Data regions
    for data in data_parts:
        f.write(data)
    
    # Index
    for name_bytes, capo_id, offset, size in entries:
        name_len = len(name_bytes)
        f.write(struct.pack("B", name_len))
        f.write(name_bytes)
        f.write(struct.pack("<IQI", capo_id, offset, size))

total_size = os.path.getsize("/content/test.tesspack")
print(f"Synthetic tesspack: {N_CAPOS} capos, {total_size/1e6:.1f} MB")
'''

# Write the generation script
with open("C:/Users/Administrator.AVENTADOR/AppData/Local/Temp/opencode/gen_synthetic.py", "w") as f:
    f.write(script)

# Step 2: Compile + generate + run
run_script = r'''
import subprocess, os

# Generate tesspack
print("=== Step 1: Generate synthetic tesspack ===")
r = subprocess.run(["python3", "-c", r"""
import struct, os, random
random.seed(42)
N_CAPOS = 1871
entries = []
data_parts = []
offset = 64
for i in range(N_CAPOS):
    name = f"tensor_{i:04d}"
    name_bytes = name.encode()
    size = random.randint(2048, 16384)
    size = (size + 63) & ~63
    entries.append((name_bytes, i, offset, size))
    data_parts.append(os.urandom(size))
    offset += size
index_offset = offset
with open("/content/test.tesspack", "wb") as f:
    hdr = struct.pack("<IIIQ", 0x5450414B, 1, N_CAPOS, index_offset)
    hdr += b"\\x00" * (64 - len(hdr))
    f.write(hdr)
    for data in data_parts:
        f.write(data)
    for name_bytes, capo_id, offset, size in entries:
        name_len = len(name_bytes)
        f.write(struct.pack("B", name_len))
        f.write(name_bytes)
        f.write(struct.pack("<IQI", capo_id, offset, size))
print(f"Synthetic: {N_CAPOS} capos, {os.path.getsize('/content/test.tesspack')/1e6:.1f} MB")
"""], capture_output=True, text=True, timeout=60)
print(r.stdout)
if r.stderr:
    print("ERR:", r.stderr[:300])

# Compile kernel
print("\n=== Step 2: Compile tess_scatter_bench ===")
r = subprocess.run(["nvcc", "-O3", "-std=c++17", "-arch=sm_75",
    "-o", "/content/tess_scatter_bench",
    "/content/tess_scatter_bench.cu", "-lm"],
    capture_output=True, text=True, timeout=300)
if r.returncode != 0:
    print("COMPILE FAILED:", r.stderr[:1000])
else:
    sz = os.path.getsize("/content/tess_scatter_bench")
    print(f"Compiled OK: {sz} bytes")
    
    # Run benchmark
    print("\n=== Step 3: Run tess_scatter_bench ===")
    r2 = subprocess.run(["/content/tess_scatter_bench",
        "/content/test.tesspack",
        "/dev/null"],  # gguf not needed for scatter bench
        capture_output=True, timeout=120)
    out = r2.stdout.decode("utf-8", errors="replace")
    err = r2.stderr.decode("utf-8", errors="replace")
    print(out)
    if err:
        print("STDERR:", err[:500])
'''

with open("C:/Users/Administrator.AVENTADOR/AppData/Local/Temp/opencode/gen_and_run.py", "w") as f:
    f.write(run_script)

print("Written gen_and_run.py")
