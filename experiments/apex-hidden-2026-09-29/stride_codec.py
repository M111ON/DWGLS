#!/usr/bin/env python3
"""stride_codec.py — the equation as a codec: can a stride replace the push?

k = 2^j * 3^l * odd'
  2^j, 3^l -> frustum: collapse, cost = log2(gcd(k,N)) bits per element, MUST be stored
  odd'     -> unit: permutation, cost = 0, invertible with no side information

So the equation is the ledger: it prices any push BEFORE running it. This script
checks the two claims on real bytes rather than assuming them.

  unit stride      : apply x->k*x, invert with k^-1, expect exact and 0 bits stored
  frustum stride   : apply x->k*x, invert with k^-1 mod N/g + the top j bits,
                     expect exact and exactly v2(k) bits stored per element

usage: stride_codec.py [file] [offset] [len] [q8]
"""
import sys
from math import gcd
import numpy as np

N = 256


def v2(k):
    j = 0
    while k % 2 == 0:
        k //= 2
        j += 1
    return j


def unit_roundtrip(x, k):
    inv = pow(k, -1, N)
    y = (x.astype(np.int32) * k) & 0xFF
    back = (y.astype(np.int32) * inv) & 0xFF
    return y, back


def frustum_roundtrip(x, k):
    """store the top j bits, recover the rest from the unit part."""
    j = v2(k)
    g = 1 << j
    o = k >> j
    m2 = N >> j                      # modulus after the collapse
    y = (x.astype(np.int32) * k) & 0xFF
    side = (x.astype(np.int32) >> (8 - j)) & (g - 1)   # top j bits of x
    w = (y.astype(np.int32) >> j) & (m2 - 1)           # y/g  ==  o*x mod 2^(8-j)
    low = (w * pow(o, -1, m2)) & (m2 - 1)              # x mod 2^(8-j)
    back = ((side << (8 - j)) | low) & 0xFF
    return y, back, j


def main():
    if len(sys.argv) > 1:
        path = sys.argv[1]
        off = int(sys.argv[2]) if len(sys.argv) > 2 else 0
        ln = int(sys.argv[3]) if len(sys.argv) > 3 else 1020000
        with open(path, "rb") as f:
            f.seek(off)
            raw = f.read(ln)
        x = np.frombuffer(raw, dtype=np.uint8).copy()
        if len(sys.argv) > 4 and sys.argv[4] == "q8":
            idx = np.arange(x.size)
            x = x[(idx % 34) >= 2]
        print(f"=== real weights {path.split(chr(92))[-1]}  n={x.size} ===")
    else:
        rng = np.random.default_rng(7)
        x = rng.integers(0, 256, 300000, dtype=np.uint8)
        print(f"=== synthetic random  n={x.size} ===")

    ok_all = True
    print("\n-- unit strides: free, exact, no side information --")
    for k in (3, 5, 7, 11, 13, 37):
        y, back = unit_roundtrip(x, k)
        exact = np.array_equal(back, x)
        ok_all &= exact
        print(f"  k={k:2d}  gcd={gcd(k,N)}  exact={exact}  bits_stored=0.000"
              f"  inv={pow(k,-1,N):3d}")

    print("\n-- frustum strides: cost = v2(k) bits per element, and that is enough --")
    for k in (2, 4, 8, 12, 16):
        y, back, j = frustum_roundtrip(x, k)
        exact = np.array_equal(back, x)
        ok_all &= exact
        lost = N // (1 << j)
        print(f"  k={k:2d}  gcd={gcd(k,N):2d}  collapse=x{1<<j:<2d}  support={len(np.unique(y)):3d}"
              f"  bits_stored={j}  exact={exact}")

    print("\n-- the same push written two ways, priced --")
    for k in (4, 12):
        j = v2(k)
        print(f"  k={k:2d} = 2^{j} * {k>>j:<2d}  ->  {j} bits/element"
              f"  = {j/8:.3f} byte/element  ({j*x.size/8/1024:.1f} KB for n={x.size})")
    print(f"\n{'PASS' if ok_all else 'FAIL'}")
    return 0 if ok_all else 1


if __name__ == "__main__":
    sys.exit(main())
