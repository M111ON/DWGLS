"""
Path Container SHRINK — เขียนเฉพาะ paths ที่ใช้จริง
ไม่ใช่ compress — แค่ไม่เขียนที่ไม่ใช้
"""
import os, sys, struct, numpy as np

MAGIC = b'GPLN'
END_MAGIC = b'PLNDEND!'

def assign_paths(n_cells, n_paths):
    paths = [[] for _ in range(n_paths)]
    for i in range(n_cells):
        paths[i % n_paths].append(i)
    return paths

def write_selective(filepath, cells, cell_size, selected_paths, all_n_cells, all_n_paths):
    all_paths = assign_paths(all_n_cells, all_n_paths)
    with open(filepath, 'wb') as f:
        f.write(MAGIC)
        f.write(struct.pack('<H', 1))
        f.write(struct.pack('<I', len(selected_paths)))
        f.write(struct.pack('<I', cell_size))
        f.write(b'\x00' * 14)

        index = []
        for idx, p in enumerate(selected_paths):
            offset = f.tell()
            buf = b''.join(cells[c] for c in all_paths[p])
            f.write(buf)
            index.append((idx, p, offset, len(buf)))

        index_offset = f.tell()
        for idx, p, offset, length in index:
            f.write(struct.pack('<IIQI', idx, p, offset, length))

        f.write(struct.pack('<QI8s', index_offset, len(selected_paths), END_MAGIC))
    return sum(len(all_paths[p]) for p in selected_paths)

def read_container(filepath):
    with open(filepath, 'rb') as f:
        f.read(4)
        _, n_paths, cell_size = struct.unpack('<HII', f.read(10))
        f.read(14)
        f.seek(-20, 2)
        index_offset, n, _ = struct.unpack('<QI8s', f.read(20))
        f.seek(index_offset)
        index = []
        for _ in range(n):
            idx, p, offset, length = struct.unpack('<IIQI', f.read(20))
            index.append((idx, p, offset, length))
        return n, cell_size, index, os.path.getsize(filepath)

def read_path(filepath, index, seq_idx):
    _, p, offset, length = index[seq_idx]
    with open(filepath, 'rb') as f:
        f.seek(offset)
        return f.read(length), p

N_CELLS, CELL_SIZE, N_PATHS = 20736, 100, 144
rng = np.random.default_rng(42)
cells = {i: rng.integers(0, 256, CELL_SIZE, dtype=np.uint8).tobytes() for i in range(N_CELLS)}
full_data = N_CELLS * CELL_SIZE

print("=" * 62)
print("PATH CONTAINER — เขียนเฉพาะ paths ที่ใช้จริง")
print("=" * 62)
print(f"\nData: {N_CELLS} cells x {CELL_SIZE}B = {full_data:,} bytes ({full_data/1024/1024:.2f} MB)")

write_selective("full.gpln", cells, CELL_SIZE, list(range(N_PATHS)), N_CELLS, N_PATHS)
_, _, _, full_sz = read_container("full.gpln")
print(f"\n[FULL]   {N_PATHS:3d} paths:  {full_sz:>12,} bytes (100%)")

sel10 = list(range(0, N_PATHS, N_PATHS // 10))
n10 = write_selective("p10.gpln", cells, CELL_SIZE, sel10, N_CELLS, N_PATHS)
_, _, _, sz10 = read_container("p10.gpln")
print(f"[10 PATHS] {len(sel10):2d} paths:  {sz10:>12,} bytes ({sz10/full_sz*100:.1f}%)  cells={n10}")

sel3 = [0, 48, 96]
n3 = write_selective("p3.gpln", cells, CELL_SIZE, sel3, N_CELLS, N_PATHS)
_, _, _, sz3 = read_container("p3.gpln")
print(f"[3 PATHS]  {len(sel3):2d} paths:   {sz3:>12,} bytes ({sz3/full_sz*100:.1f}%)  cells={n3}")

sel1 = [0]
n1 = write_selective("p1.gpln", cells, CELL_SIZE, sel1, N_CELLS, N_PATHS)
_, _, _, sz1 = read_container("p1.gpln")
print(f"[1 PATH]   {len(sel1):2d} paths:   {sz1:>12,} bytes ({sz1/full_sz*100:.1f}%)  cells={n1}")

print(f"\n[LOSSLESS]")
all_paths = assign_paths(N_CELLS, N_PATHS)
for name, sel in [("p10", sel10), ("p3", sel3), ("p1", sel1)]:
    _, _, idx, _ = read_container(f"{name}.gpln")
    ok = True
    for seq_i in range(len(sel)):
        buf, p = read_path(f"{name}.gpln", idx, seq_i)
        expect = b''.join(cells[c] for c in all_paths[p])
        if buf != expect:
            ok = False
            break
    print(f"  {name}: {'PASS' if ok else 'FAIL'}")

print(f"\n[RAM + DISK ทำคู่กัน]")
print(f"  RAM:  seed roots lane = 69/20736 = 0.3%  (ทำแล้ว)")
print(f"  Disk: 1 path = {sz1/full_sz*100:.2f}% ({sz1:,} bytes)")
print(f"  ทั้งคู่ = ไม่กางทั้งระบบ, access แค่ path ที่เดิน")

print(f"\n{'='*62}")
print("SUMMARY")
print(f"{'='*62}")
print(f"  Full (144 paths): {full_sz:>12,} bytes (100%)")
print(f"  10 paths:         {sz10:>12,} bytes ({sz10/full_sz*100:.1f}%)  cells={n10}")
print(f"  3 paths:           {sz3:>12,} bytes ({sz3/full_sz*100:.1f}%)  cells={n3}")
print(f"  1 path:            {sz1:>12,} bytes ({sz1/full_sz*100:.1f}%)  cells={n1}")
print(f"\n  ไม่ใช่ compress — แค่ไม่เขียนที่ไม่ใช้")
print(f"  โครงสร้างหด = data หด = RAM+Disk น้อยลงชัดเจน")

for f in ["full.gpln", "p10.gpln", "p3.gpln", "p1.gpln"]:
    if os.path.exists(f): os.remove(f)
