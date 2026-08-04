"""
Path-Layout Disk Container — data บน disk เรียงตาม path

แนวคิด (ทำคู่กับ RAM-level seed roots lane):
- RAM:  access path -> load เฉพาะ path (ทำแล้ว)
- Disk: access path -> read เฉพาะ block (ทำตอนนี้)

Container format:
[MAGIC 8B][VERSION 2B][PATH_COUNT 4B][CELL_SIZE 4B][reserved 14B] = 32B header
[data: path 0 | path 1 | ... | path N-1]   <- เรียงตาม path (path-contiguous)
[index: (path_id u32, offset u64, length u32)] x N
[footer: index_offset u64, path_count u32, END_MAGIC 8B]

Access path p = seek(offset_p) + read(length_p)  -> read เฉพาะ block ของ path
"""
import os
import struct
import numpy as np

MAGIC = b'GPLN'          # Geometric Path Layout coNtainer
END_MAGIC = b'PLNDEND!'  # 8 bytes (struct 8s)
HEADER_SIZE = 32
INDEX_ENTRY = 4 + 8 + 4   # path_id + offset + length = 16 bytes
FOOTER_SIZE = 8 + 4 + 8   # index_offset + path_count + end_magic


# ============ Path assignment ============
def assign_paths(n_cells, n_paths, stride=37):
    """
    กำหนด cell -> path แบบ deterministic (stride walk)
    path p = cells [p, p+n_paths, p+2*n_paths, ...]
    """
    paths = [[] for _ in range(n_paths)]
    for i in range(n_cells):
        p = i % n_paths
        paths[p].append(i)
    return paths


# ============ Writer ============
def write_container(filepath, cells, n_paths, stride=37):
    """
    cells: dict {cell_id: bytes}
    เขียน data เรียงตาม path order
    """
    n_cells = len(cells)
    cell_size = len(next(iter(cells.values())))
    paths = assign_paths(n_cells, n_paths, stride)

    with open(filepath, 'wb') as f:
        # 1. Header
        f.write(MAGIC)
        f.write(struct.pack('<H', 1))          # version
        f.write(struct.pack('<I', n_paths))    # path count
        f.write(struct.pack('<I', cell_size))  # cell size
        f.write(b'\x00' * 14)                  # reserved

        # 2. Data (path order, contiguous per path)
        index = []
        for p, path_cells in enumerate(paths):
            offset = f.tell()
            buf = b''
            for cid in path_cells:
                buf += cells[cid]
            f.write(buf)
            index.append((p, offset, len(buf)))

        # 3. Index
        index_offset = f.tell()
        for p, offset, length in index:
            f.write(struct.pack('<IQI', p, offset, length))

        # 4. Footer
        f.write(struct.pack('<QI8s', index_offset, n_paths, END_MAGIC))

    return index


# ============ Reader ============
def read_container(filepath):
    """อ่าน header + index (ไม่ read data)"""
    with open(filepath, 'rb') as f:
        magic = f.read(4)
        assert magic == MAGIC, f"not a GPLN container: {magic}"
        version, n_paths, cell_size = struct.unpack('<HII', f.read(10))
        f.read(14)  # reserved

        # ฟุต footer เพื่อหา index
        f.seek(-FOOTER_SIZE, 2)
        index_offset, n_paths2, end_magic = struct.unpack('<QI8s', f.read(20))
        assert end_magic == END_MAGIC
        assert n_paths == n_paths2

        # อ่าน index
        f.seek(index_offset)
        index = {}
        for _ in range(n_paths):
            p, offset, length = struct.unpack('<IQI', f.read(16))
            index[p] = (offset, length)

        file_size = os.path.getsize(filepath)
        return {
            'version': version,
            'n_paths': n_paths,
            'cell_size': cell_size,
            'index': index,
            'file_size': file_size,
            'data_bytes': index_offset - HEADER_SIZE,
        }


def read_path(filepath, index, p):
    """access path p = seek + read เฉพาะ block ของ path"""
    offset, length = index[p]
    with open(filepath, 'rb') as f:
        f.seek(offset)
        buf = f.read(length)
    return buf


# ============ Verification ============
def verify_container(filepath, cells, n_paths, stride=37):
    print("=" * 62)
    print("PATH-LAYOUT DISK CONTAINER — VERIFICATION")
    print("=" * 62)

    meta = read_container(filepath)
    n_cells = len(cells)
    cell_size = meta['cell_size']
    file_size = meta['file_size']
    print(f"\nCells: {n_cells} x {cell_size}B = {n_cells*cell_size:,} bytes data")
    print(f"Paths: {n_paths}  (stride={stride})")
    print(f"File size: {file_size:,} bytes")
    print(f"Header+Index overhead: {file_size - n_cells*cell_size:,} bytes")

    # ---- 1. Lossless: read back every path, compare every byte ----
    print(f"\n[1] LOSSLESS roundtrip (ทุก path ทุก byte)...")
    mismatches = 0
    paths = assign_paths(n_cells, n_paths, stride)
    for p in range(n_paths):
        buf = read_path(filepath, meta['index'], p)
        expect = b''.join(cells[cid] for cid in paths[p])
        if buf != expect:
            mismatches += 1
            for i, (a, b_) in enumerate(zip(buf, expect)):
                if a != b_:
                    print(f"    mismatch path {p} byte {i}: {a} != {b_}")
                    break
    print(f"    {'PASS: 0 mismatches (lossless 100%)' if mismatches == 0 else f'FAIL: {mismatches} paths mismatch'}")

    # ---- 2. Path-local read: access 1 path -> read เฉพาะ block ----
    print(f"\n[2] PATH-LOCAL read (access path เดียว -> อ่าน block เดียว)...")
    test_p = n_paths // 2
    offset, length = meta['index'][test_p]
    with open(filepath, 'rb') as f:
        f.seek(offset)
        buf = f.read(length)
        bytes_read = len(buf)
    whole = file_size
    print(f"    Path {test_p}: read {bytes_read:,} bytes (file = {whole:,} bytes)")
    print(f"    Bytes read vs file: {bytes_read/whole*100:.2f}% of file")
    print(f"    {'PASS: อ่านแค่ block ของ path (ไม่ read ทั้งไฟล์)' if bytes_read < whole else 'FAIL: read ทั้งไฟล์'}")
    print(f"    {'PASS: data ถูกต้อง' if buf == b''.join(cells[c] for c in assign_paths(n_cells, n_paths, stride)[test_p]) else 'FAIL: data ผิด'}")

    # ---- 3. Random access: 10 random paths ----
    print(f"\n[3] RANDOM access (10 random paths)...")
    rng = np.random.default_rng(7)
    ok = True
    total_read = 0
    for p in rng.integers(0, n_paths, 10):
        buf = read_path(filepath, meta['index'], int(p))
        total_read += len(buf)
        expect = b''.join(cells[c] for c in assign_paths(n_cells, n_paths, stride)[int(p)])
        if buf != expect:
            ok = False
            print(f"    FAIL path {p}")
    print(f"    {'PASS: 10/10 random paths ถูกต้อง' if ok else 'FAIL'}")
    print(f"    Total read for 10 paths: {total_read:,} bytes = {total_read/whole*100:.2f}% of file")

    # ---- 4. RAM comparison (ทำคู่กับ RAM level) ----
    print(f"\n[4] RAM vs DISK (ทำคู่กัน):")
    print(f"    RAM:  access path -> load เฉพาะ path  (seed roots lane, ทำแล้ว)")
    print(f"    Disk: access path -> read เฉพาะ block (container นี้, ทำตอนนี้)")
    print(f"    ทั้งคู่ = ไม่กางทั้งระบบ, access แค่ path ที่เดิน")

    print(f"\n{'='*62}")
    all_ok = mismatches == 0 and bytes_read < whole
    print(f"FINAL: {'PASS' if all_ok else 'FAIL'} — container layout ตาม path ทำงาน")
    return all_ok


# ============ Main ============
if __name__ == "__main__":
    import sys
    out = sys.argv[1] if len(sys.argv) > 1 else "path_container.gpln"

    # สร้าง data จำลอง: 20736 cells x 100B (เท่ากับ seed roots lane example)
    N_CELLS = 20736
    CELL_SIZE = 100
    N_PATHS = 144  # 144 paths (6ico)
    rng = np.random.default_rng(42)
    cells = {i: rng.integers(0, 256, CELL_SIZE, dtype=np.uint8).tobytes() for i in range(N_CELLS)}

    index = write_container(out, cells, N_PATHS)
    ok = verify_container(out, cells, N_PATHS)
    print(f"\nSaved: {out} ({os.path.getsize(out):,} bytes)")
    sys.exit(0 if ok else 1)
