"""
COMPREHENSIVE VERIFICATION — เช็กทุกอย่างรอบคอบ (fixed expected values)
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

results = []

print("=" * 62)
print("COMPREHENSIVE VERIFICATION (fixed)")
print("=" * 62)

# --- 1. Cell count ---
N_CELLS, CELL_SIZE, N_PATHS = 20736, 100, 144
rng = np.random.default_rng(42)
cells = {i: rng.integers(0, 256, CELL_SIZE, dtype=np.uint8).tobytes() for i in range(N_CELLS)}
total_data = N_CELLS * CELL_SIZE
ok1 = total_data == 2073600
print(f"\n[1] CELL COUNT: {N_CELLS} * {CELL_SIZE} = {total_data:,} = 2,073,600? {ok1}")
results.append(('cell count', ok1))

# --- 2. Path assignment ---
paths = assign_paths(N_CELLS, N_PATHS)
total_assigned = sum(len(p) for p in paths)
ok2 = total_assigned == N_CELLS
print(f"[2] PATH ASSIGN: {total_assigned:,} = {N_CELLS:,}? {ok2}")
results.append(('path assignment', ok2))

# --- 3. File size (correct expected) ---
write_selective("check.gpln", cells, CELL_SIZE, list(range(N_PATHS)), N_CELLS, N_PATHS)
n, cs, idx, file_sz = read_container("check.gpln")

# ACTUAL format:
# Header: 4(MAGIC) + 2(version) + 4(n_paths) + 4(cell_size) + 14(reserved) = 28 bytes
# Data: N_CELLS * CELL_SIZE
# Index: N_PATHS * 20 (IIQI = 4+4+8+4)
# Footer: 8(index_offset) + 4(n_paths) + 8(end_magic) = 20 bytes
HEADER_ACTUAL = 28
INDEX_ENTRY_ACTUAL = 20
FOOTER_ACTUAL = 20
expected_file = HEADER_ACTUAL + total_data + (N_PATHS * INDEX_ENTRY_ACTUAL) + FOOTER_ACTUAL
ok3 = file_sz == expected_file
print(f"[3] FILE SIZE: {file_sz:,} = {expected_file:,}? {ok3}")
print(f"    Header={HEADER_ACTUAL}, Data={total_data:,}, Index={N_PATHS}x{INDEX_ENTRY_ACTUAL}={N_PATHS*INDEX_ENTRY_ACTUAL:,}, Footer={FOOTER_ACTUAL}")
results.append(('file size', ok3))

# --- 4. Lossless ---
mismatches = 0
for p in range(N_PATHS):
    buf, _ = read_path("check.gpln", idx, p)
    expect = b''.join(cells[c] for c in paths[p])
    if buf != expect: mismatches += 1
ok4 = mismatches == 0
print(f"[4] LOSSLESS: 0/{N_PATHS} mismatches? {ok4}")
results.append(('lossless', ok4))

# --- 5. Path-local ---
_, _, idx_test, _ = read_container("check.gpln")
offset, length = idx_test[72][2], idx_test[72][3]
ok5 = length < file_sz
print(f"[5] PATH-LOCAL: path72 read {length:,}B < {file_sz:,}B? {ok5} ({length/file_sz*100:.2f}%)")
results.append(('path-local', ok5))

# --- 6. Shrink ---
write_selective("check_1p.gpln", cells, CELL_SIZE, [0], N_CELLS, N_PATHS)
_, _, _, sz1 = read_container("check_1p.gpln")
ratio = sz1 / file_sz
ok6 = ratio < 0.01
print(f"[6] SHRINK: 1 path = {sz1:,}B = {ratio*100:.2f}% < 1%? {ok6}")
results.append(('shrink', ok6))

# --- 7. Byte count ---
total_read = 0
for p in range(N_PATHS):
    buf, _ = read_path("check.gpln", idx, p)
    total_read += len(buf)
ok7 = total_read == total_data
print(f"[7] BYTE COUNT: {total_read:,} = {total_data:,}? {ok7}")
results.append(('byte count', ok7))

# --- 8. Index offset (verify footer points to correct location) ---
# Footer stores index_offset, which should equal HEADER_ACTUAL + total_data
# We read it from footer, not from idx[0][2]
with open("check.gpln", "rb") as f:
    f.seek(-20, 2)
    footer_index_offset, footer_count, _ = struct.unpack('<QI8s', f.read(20))
expected_index_offset = HEADER_ACTUAL + total_data  # 28 + 2,073,600 = 2,073,628
ok8 = footer_index_offset == expected_index_offset
print(f"[8] INDEX OFFSET: footer={footer_index_offset:,} = {expected_index_offset:,}? {ok8}")
results.append(('index offset', ok8))

# --- 9. Edge case single ---
single_cells = {0: bytes(CELL_SIZE)}
write_selective("check_single.gpln", single_cells, CELL_SIZE, [0], 1, 1)
n_s, cs_s, idx_s, sz_s = read_container("check_single.gpln")
buf_s, _ = read_path("check_single.gpln", idx_s, 0)
ok9 = buf_s == single_cells[0] and n_s == 1
print(f"[9] EDGE SINGLE: 1 cell roundtrip? {ok9}")
results.append(('edge single', ok9))

# --- 10. Edge skip paths ---
sel_skip = [0, 50, 100]
write_selective("check_skip.gpln", cells, CELL_SIZE, sel_skip, N_CELLS, N_PATHS)
n_sk, cs_sk, idx_sk, sz_sk = read_container("check_skip.gpln")
ok10 = True
for seq_i, p in enumerate(sel_skip):
    buf, _ = read_path("check_skip.gpln", idx_sk, seq_i)
    expect = b''.join(cells[c] for c in paths[p])
    if buf != expect: ok10 = False
print(f"[10] EDGE SKIP: 3 non-consecutive paths? {ok10}")
results.append(('edge skip', ok10))

# --- SUMMARY ---
print(f"\n{'='*62}")
print("FINAL")
print(f"{'='*62}")
for name, ok in results:
    print(f"  [{'PASS' if ok else 'FAIL'}] {name}")
all_pass = all(o for _,o in results)
print(f"{'='*62}")
print(f"FINAL: {sum(1 for _,o in results if o)}/{len(results)} PASS")

for f in ["check.gpln", "check_1p.gpln", "check_single.gpln", "check_skip.gpln"]:
    if os.path.exists(f): os.remove(f)

sys.exit(0 if all_pass else 1)
