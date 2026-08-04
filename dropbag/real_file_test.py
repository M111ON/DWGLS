"""
Path Container — ทดสอบกับไฟล์จริง (smolVLM-256M Q8_0)
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

def read_gguf_tensor(model_path, max_bytes=2*1024*1024):
    with open(model_path, 'rb') as f:
        raw = f.read(max_bytes)
    offset = 0
    for i in range(len(raw) - 4):
        if raw[i:i+4] != b'GGUF' and raw[i] not in [0, 1, 2, 3, 4, 5]:
            offset = i
            break
    return raw[offset:]

def test_real_file():
    model_path = "I:/model/smolVLM-256M-Instruct-text.Q8_0.gguf"
    data = read_gguf_tensor(model_path, max_bytes=2*1024*1024)
    total_size = len(data)

    print("=" * 62)
    print(f"TEST WITH REAL FILE: {os.path.basename(model_path)}")
    print("=" * 62)
    print(f"\nTensor data: {total_size:,} bytes ({total_size/1024/1024:.2f} MB)")

    CELL_SIZE = 100
    n_cells = total_size // CELL_SIZE
    cells = {i: data[i*CELL_SIZE:(i+1)*CELL_SIZE] for i in range(n_cells)}

    print(f"Cells: {n_cells} x {CELL_SIZE}B = {n_cells*CELL_SIZE:,} bytes")

    N_PATHS = 144
    n_actual = len(cells)

    # Full
    write_selective("real_full.gpln", cells, CELL_SIZE, list(range(N_PATHS)), n_actual, N_PATHS)
    _, _, _, full_sz = read_container("real_full.gpln")
    print(f"\n[FULL]   {N_PATHS:3d} paths:  {full_sz:>12,} bytes (100%)")

    # 10 paths
    sel10 = list(range(0, N_PATHS, 14))
    write_selective("real_10p.gpln", cells, CELL_SIZE, sel10, n_actual, N_PATHS)
    _, _, _, sz10 = read_container("real_10p.gpln")
    print(f"[10 PATHS] {len(sel10):2d} paths:  {sz10:>12,} bytes ({sz10/full_sz*100:.1f}%)")

    # 3 paths
    sel3 = [0, 48, 96]
    write_selective("real_3p.gpln", cells, CELL_SIZE, sel3, n_actual, N_PATHS)
    _, _, _, sz3 = read_container("real_3p.gpln")
    print(f"[3 PATHS]  {len(sel3):2d} paths:   {sz3:>12,} bytes ({sz3/full_sz*100:.1f}%)")

    # 1 path
    sel1 = [0]
    write_selective("real_1p.gpln", cells, CELL_SIZE, sel1, n_actual, N_PATHS)
    _, _, _, sz1 = read_container("real_1p.gpln")
    print(f"[1 PATH]   {len(sel1):2d} paths:   {sz1:>12,} bytes ({sz1/full_sz*100:.1f}%)")

    # Lossless
    print(f"\n[LOSSLESS]")
    all_paths = assign_paths(n_actual, N_PATHS)
    for name, sel in [("10p", sel10), ("3p", sel3), ("1p", sel1)]:
        _, _, idx, _ = read_container(f"real_{name}.gpln")
        ok = True
        for seq_i in range(len(sel)):
            buf, p = read_path(f"real_{name}.gpln", idx, seq_i)
            expect = b''.join(cells[c] for c in all_paths[p])
            if buf != expect:
                ok = False
                break
        print(f"  {name}: {'PASS' if ok else 'FAIL'}")

    # Path-local
    print(f"\n[PATH-LOCAL READ]")
    _, _, idx1, _ = read_container("real_1p.gpln")
    offset, length = idx1[0][2], idx1[0][3]
    with open("real_1p.gpln", "rb") as f:
        f.seek(offset)
        buf = f.read(length)
    print(f"  1 path: read {len(buf):,} bytes = {len(buf)/full_sz*100:.2f}% ของ full file")

    # Summary
    print(f"\n{'='*62}")
    print("SUMMARY")
    print(f"{'='*62}")
    print(f"  Real file: {total_size:,} bytes ({total_size/1024/1024:.2f} MB)")
    print(f"  Full:      {full_sz:>12,} bytes (100%)")
    print(f"  10 paths:  {sz10:>12,} bytes ({sz10/full_sz*100:.1f}%)")
    print(f"  3 paths:   {sz3:>12,} bytes ({sz3/full_sz*100:.1f}%)")
    print(f"  1 path:    {sz1:>12,} bytes ({sz1/full_sz*100:.1f}%)")

    for f in ["real_full.gpln", "real_10p.gpln", "real_3p.gpln", "real_1p.gpln"]:
        if os.path.exists(f): os.remove(f)

if __name__ == "__main__":
    test_real_file()
