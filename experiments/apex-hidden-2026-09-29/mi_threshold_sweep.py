#!/usr/bin/env python3
"""mi_threshold_sweep.py — where does I(value; side) saturate, and on what?

The ortho-push report used file offset 34,000,000, which is NOT on a Q8_0 block
boundary relative to the tensor data start (tensor starts at 5,947,744, so the
slice was 12 bytes into a 34-byte block and 2 scale bytes leaked into every 32
payload bytes). See KIS-ENTROPY-BASELINE-2026-10-30.md section 10.

Default offset here is the aligned one (34,000,022). Pass an offset to override.

usage: mi_threshold_sweep.py [offset] [length]
"""
import sys
import numpy as np

sys.path.insert(0, r"I:\DWGLS-native-fs\experiments\apex-hidden-2026-09-29")
from ortho_push import mi, split_sides

GGUF = r"I:\model\Qwen2.5-0.5B-Instruct-Q8_0.gguf"
OFF = int(sys.argv[1]) if len(sys.argv) > 1 else 34000022
LN = int(sys.argv[2]) if len(sys.argv) > 2 else 1020000

f = open(GGUF, "rb")
f.seek(OFF)
raw = f.read(LN)
f.close()
d = np.frombuffer(raw, dtype=np.uint8).copy()
idx = np.arange(d.size)
data = d[(idx % 34) >= 2].astype(np.int16)

# alignment receipt: a correct Q8_0 phase has max|q| = 127 in every block
blocks = d[: (d.size // 34) * 34].reshape(-1, 34)
q = blocks[:, 2:].copy().view(np.int8)          # signed view, not uint8
sc = blocks[:, :2].copy().view(np.float16)
print(f"offset={OFF}  blocks={blocks.shape[0]}"
      f"  frac(max|q|=127)={np.mean(np.abs(q).max(1) == 127):.4f}"
      f"  frac(scale>0 finite)={np.mean(np.isfinite(sc) & (sc > 0)):.4f}")

lo, hi = int(data.min()), int(data.max()) + 1
W = hi - lo
print(f"n={data.size}  min={lo}  max={hi-1}  W={W}  (W/2={W/2})")


def I_at(c):
    a = data.copy()
    a[0::2] += c
    a[1::2] -= c
    g0, g1 = split_sides(a)
    alo, ahi = int(a.min()), int(a.max()) + 1
    b0 = np.bincount((g0 - alo).ravel(), minlength=ahi - alo).astype(np.float64)
    b1 = np.bincount((g1 - alo).ravel(), minlength=ahi - alo).astype(np.float64)
    return mi(b0 + b1, b0, b1)


print("\n  c     I(val;side)   uniform 2c/W   status")
for c in (0, 3, 12, 24, 32, 48, 64, 80, 96, 112, 120, 124, 128, 132, 144, 160, 192, 256):
    v = I_at(c)
    tag = "FULL" if v >= 0.99995 else ("partial" if v > 0.02 else "none")
    print(f"{c:4d}  {v:10.4f}  {min(1.0, 2*c/W):11.4f}   {tag}")
