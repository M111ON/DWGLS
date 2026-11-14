#!/usr/bin/env python3
"""gguf_probe.py — which tensor holds a given file offset, and what are its strides?

Parses the GGUF header directly (no llama.cpp needed). Answers the open question from
the 2026-10-30 baseline: is offset 34,000,000 really inside a tensor whose row stride
is 896, or was "896 = 28 x 32" just an assumption?

usage: gguf_probe.py <file.gguf> [offset]
"""
import struct
import sys

TYPES = {0: "F32", 1: "F16", 2: "Q4_0", 3: "Q4_1", 6: "Q5_0", 7: "Q5_1", 8: "Q8_0",
         9: "Q8_1", 10: "Q2_K", 11: "Q3_K", 12: "Q4_K", 13: "Q5_K", 14: "Q6_K",
         15: "Q8_K", 16: "IQ2_XXS", 17: "IQ2_XS", 18: "IQ3_XXS", 19: "IQ1_S",
         20: "IQ4_NL", 21: "IQ3_S", 22: "IQ2_S", 23: "IQ4_XS", 24: "I8",
         25: "I16", 26: "I32", 27: "I64", 28: "F64", 30: "BF16"}

# bytes per element (or per block for quantized types) and elements per block
BLOCK = {0: (4, 1), 1: (2, 1), 2: (18, 32), 3: (20, 32), 6: (22, 32), 7: (24, 32),
         8: (34, 32), 9: (36, 32), 10: (84, 256), 11: (110, 256), 12: (144, 256),
         13: (176, 256), 14: (210, 256), 15: (292, 256), 24: (1, 1), 25: (2, 1),
         26: (4, 1), 27: (8, 1), 28: (8, 1), 30: (2, 1)}


def read_val(f, t):
    if t in (0, 1, 7):
        return f.read(1)
    if t in (2, 3):
        return f.read(2)
    if t in (4, 5, 6):
        return f.read(4)
    if t == 8:
        n = struct.unpack("<Q", f.read(8))[0]
        return f.read(n).decode("utf-8", "replace")
    if t == 9:
        et = struct.unpack("<I", f.read(4))[0]
        n = struct.unpack("<Q", f.read(8))[0]
        if et in (0, 1, 7):
            f.read(n)
        elif et in (2, 3):
            f.read(2 * n)
        elif et in (4, 5, 6):
            f.read(4 * n)
        elif et in (10, 11, 12):
            f.read(8 * n)
        elif et == 8:
            for _ in range(n):
                m = struct.unpack("<Q", f.read(8))[0]
                f.read(m)
        return f"<array[{n}] of type {et}>"
    if t in (10, 11, 12):
        return f.read(8)
    raise ValueError(f"unknown gguf value type {t}")


def main():
    path = sys.argv[1]
    target = int(sys.argv[2]) if len(sys.argv) > 2 else 34000000
    f = open(path, "rb")
    magic = f.read(4)
    if magic != b"GGUF":
        print(f"not a GGUF file: {magic!r}")
        return 1
    ver = struct.unpack("<I", f.read(4))[0]
    n_tensors = struct.unpack("<Q", f.read(8))[0]
    n_kv = struct.unpack("<Q", f.read(8))[0]
    print(f"{path}\n  gguf version {ver}  tensors={n_tensors}  metadata_kv={n_kv}")

    alignment = 32
    meta = {}
    for _ in range(n_kv):
        klen = struct.unpack("<Q", f.read(8))[0]
        key = f.read(klen).decode("utf-8", "replace")
        vt = struct.unpack("<I", f.read(4))[0]
        v = read_val(f, vt)
        if key in ("general.alignment",):
            alignment = struct.unpack("<I", v)[0] if isinstance(v, bytes) else int(v)
        if key in ("general.architecture", "general.name"):
            meta[key] = v

    tensors = []
    for _ in range(n_tensors):
        nlen = struct.unpack("<Q", f.read(8))[0]
        name = f.read(nlen).decode("utf-8", "replace")
        nd = struct.unpack("<I", f.read(4))[0]
        dims = [struct.unpack("<Q", f.read(8))[0] for _ in range(nd)]
        ttype = struct.unpack("<I", f.read(4))[0]
        toff = struct.unpack("<Q", f.read(8))[0]
        tensors.append((name, dims, ttype, toff))

    hdr_end = f.tell()
    data_start = (hdr_end + alignment - 1) // alignment * alignment

    print(f"  architecture={meta.get('general.architecture')}  alignment={alignment}")
    print(f"  header ends at {hdr_end}, tensor data starts at {data_start}\n")

    hit = None
    rows = []
    for name, dims, ttype, toff in tensors:
        nelem = 1
        for d in dims:
            nelem *= d
        bsz, eblk = BLOCK.get(ttype, (1, 1))
        nbytes = nelem // eblk * bsz
        start = data_start + toff
        end = start + nbytes
        rows.append((start, end, name, dims, ttype, nbytes))
        if start <= target < end:
            hit = (start, end, name, dims, ttype, nbytes)

    print(f"  {'tensor':38s} {'type':6s} {'dims':26s} {'bytes':>12s}  file range")
    for start, end, name, dims, ttype, nbytes in rows:
        mark = "  <== offset" if start <= target < end else ""
        if mark or nbytes > 1_000_000:
            print(f"  {name:38s} {TYPES.get(ttype,ttype):6s} {str(dims):26s} {nbytes:12,d}"
                  f"  [{start:,} .. {end:,}){mark}")

    if not hit:
        print(f"\n  offset {target:,} is NOT inside any tensor")
        return 1

    start, end, name, dims, ttype, nbytes = hit
    bsz, eblk = BLOCK.get(ttype, (1, 1))
    print(f"\n  ==> offset {target:,} falls in tensor `{name}`")
    print(f"      type {TYPES.get(ttype,ttype)}  dims {dims}  bytes {nbytes:,}")
    rel = target - start
    row_elems = dims[0] if dims else 0
    print(f"      offset within tensor = {rel:,} bytes")
    if ttype == 8:
        print(f"      row length = dims[0] = {row_elems} elements"
              f" = {row_elems//32} Q8_0 blocks = {row_elems*34//32} bytes")
        print(f"      byte position inside row = {rel % (row_elems*34//32)}")
        print(f"      element index = {rel // 34 * 32}, row = {rel // (row_elems*34//32)}")
        print(f"      row stride is {row_elems} elements -> 896? {row_elems == 896}")
        print(f"      blocks per row = {row_elems//32}; 896 % 32 = {896 % 32}"
              f" -> position mod 32 {'aligns with' if row_elems % 32 == 0 else 'does NOT align with'} column")
    return 0


if __name__ == "__main__":
    sys.exit(main())
