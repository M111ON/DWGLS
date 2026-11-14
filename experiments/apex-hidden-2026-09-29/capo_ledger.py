#!/usr/bin/env python3
"""capo_ledger.py — what does the fixed 20736-cell frame cost across a whole model?

The packer ceils n_blocks/20736 into capos and pads the last one. tess_gguf_pack
now reports that per tensor; this totals it up so the number is judged, not felt.

usage: capo_ledger.py <file.gguf> [top_n]
"""
import struct
import sys

from gguf_probe import read_val, BLOCK, TYPES

FIELD = 20736
# gguf_cell_size() in tools/tess_gguf_pack.c; 0 means the packer skips the type
CELL = {0: 4, 1: 2, 2: 18, 3: 20, 4: 0, 5: 0, 6: 22, 7: 24, 8: 34, 9: 36,
        10: 84, 11: 110, 12: 144, 13: 176, 14: 210, 15: 292, 41: 6}


def main():
    path = sys.argv[1]
    top = int(sys.argv[2]) if len(sys.argv) > 2 else 8
    f = open(path, "rb")
    if f.read(4) != b"GGUF":
        print("not a GGUF file")
        return 1
    f.read(4)
    n_tensors = struct.unpack("<Q", f.read(8))[0]
    n_kv = struct.unpack("<Q", f.read(8))[0]
    for _ in range(n_kv):
        klen = struct.unpack("<Q", f.read(8))[0]
        f.read(klen)
        read_val(f, struct.unpack("<I", f.read(4))[0])

    rows, onion, skipped = [], 0, 0
    for _ in range(n_tensors):
        nlen = struct.unpack("<Q", f.read(8))[0]
        name = f.read(nlen).decode("utf-8", "replace")
        nd = struct.unpack("<I", f.read(4))[0]
        dims = [struct.unpack("<Q", f.read(8))[0] for _ in range(nd)]
        ttype = struct.unpack("<I", f.read(4))[0]
        f.read(8)  # offset, unused here
        nelem = 1
        for d in dims:
            nelem *= d
        csz = CELL.get(ttype, 0)
        if ttype in (0, 1):
            onion += nelem * (4 if ttype == 0 else 2)
            continue
        if csz == 0:
            skipped += nelem * BLOCK.get(ttype, (1, 1))[0]
            continue
        n_blocks = (nelem // BLOCK[ttype][1]) * BLOCK[ttype][0] // csz
        n_capos = (n_blocks + FIELD - 1) // FIELD
        used = n_blocks - (n_capos - 1) * FIELD
        waste = FIELD - used
        rows.append((name, TYPES.get(ttype, ttype), csz, n_blocks, n_capos, used, waste))

    tot_blocks = sum(r[3] for r in rows)
    tot_slots = sum(r[4] * FIELD for r in rows)
    tot_waste = sum(r[6] for r in rows)
    src = sum(r[3] * r[2] for r in rows)

    print(f"{path}")
    print(f"  tensors in capos : {len(rows)}   onion(f32/f16): {onion:,} B   skipped: {skipped:,} B")
    print(f"  cells needed     : {tot_blocks:,}")
    print(f"  cells allocated  : {tot_slots:,}   ({len(rows)} tensors -> {tot_slots // FIELD} capos)")
    print(f"  padding          : {tot_waste:,} cells = {tot_waste / tot_slots * 100:.1f}% of the frame")
    print(f"  source bytes     : {src:,}")
    print(f"  frame bytes      : {tot_slots * 0 + sum(r[4] * FIELD * r[2] for r in rows):,}")
    exact = sum(1 for r in rows if r[6] == 0)
    print(f"  tensors that tile exactly : {exact}/{len(rows)}")

    print(f"\n  worst {top} by wasted cells:")
    for name, ty, csz, nb, nc, used, waste in sorted(rows, key=lambda r: -r[6])[:top]:
        print(f"    {name:38s} {ty:6s} cell={csz:3d} blocks={nb:7d} capos={nc:2d}"
              f" waste={waste:5d} ({waste / FIELD * 100:4.1f}%)")

    print(f"\n  tensors needing 2 capos for a small overflow (waste > 50%):")
    n = 0
    for name, ty, csz, nb, nc, used, waste in rows:
        if nc == 2 and waste > FIELD // 2:
            n += 1
            print(f"    {name:38s} {nb:7d} blocks = {nb / FIELD * 100:5.1f}% of a capo"
                  f"  -> waste {waste / FIELD * 100:4.1f}%")
    if n == 0:
        print("    none")
    return 0


if __name__ == "__main__":
    sys.exit(main())
