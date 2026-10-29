#!/usr/bin/env python3
"""entropy_moves.py — does the pattern stay constant when the field is moved/scaled?

Not a compression question. Three operations on the SAME data, and what each does
to (a) byte entropy, (b) the multiset of bin counts (= the pattern itself):

  translate  x -> x + t          (move the field)
  scale      x -> k*x  mod 256   (expand / contract)
  yoyo       x -> k*x -> k^-1*x  (expand then contract)

Exact predictions, which the run checks rather than assumes:
  translate        : bijection on 256 bins  -> H unchanged, pattern rigid (sorted counts equal)
  scale, k odd     : gcd(k,256)=1 -> permutation -> H unchanged, pattern rigid
  scale, k = 2^m   : 2^m-to-1 collapse -> H drops exactly m bits, pattern folded
  yoyo, k odd      : exact roundtrip (inverse exists mod 256)
  yoyo, k even     : irreversible (low bits gone)

usage: entropy_moves.py [file] [offset] [len] [q8]
"""
import sys
import numpy as np

BINS = 256


def counts(x):
    return np.bincount(x.ravel(), minlength=BINS).astype(np.int64)


def H(c):
    n = c.sum()
    p = c[c > 0] / n
    return float(-(p * np.log2(p)).sum())


def rigid(c0, c1):
    """Pattern preserved as a rigid move: same multiset of bin counts."""
    return np.array_equal(np.sort(c0), np.sort(c1))


def support(c):
    return int((c > 0).sum())


def main():
    if len(sys.argv) > 1:
        path = sys.argv[1]
        off = int(sys.argv[2]) if len(sys.argv) > 2 else 0
        ln = int(sys.argv[3]) if len(sys.argv) > 3 else 1020000
        with open(path, "rb") as f:
            f.seek(off)
            raw = f.read(ln)
        data = np.frombuffer(raw, dtype=np.uint8).copy()
        if len(sys.argv) > 4 and sys.argv[4] == "q8":
            idx = np.arange(data.size)
            data = data[(idx % 34) >= 2]
        name = f"real weights {path.split(chr(92))[-1]}"
    else:
        rng = np.random.default_rng(7)
        data = rng.integers(0, 256, 200000, dtype=np.uint8)
        name = "synthetic random"

    c0 = counts(data)
    h0 = H(c0)
    print(f"=== {name}  n={data.size} ===")
    print(f"baseline : H={h0:.6f}  support={support(c0)}  max_count={c0.max()}")

    print("\n-- translate (move the field) --")
    for t in (1, 3, 12, 48, 192):
        a = (data.astype(np.int32) + t) & 0xFF
        c1 = counts(a)
        back = (a.astype(np.int32) - t) & 0xFF
        print(f"  +{t:3d} | H={H(c1):.6f}  dH={H(c1)-h0:+.2e}"
              f"  rigid={rigid(c0, c1)}  support={support(c1)}"
              f"  yoyo_exact={np.array_equal(back, data)}")

    print("\n-- scale (expand / contract), mod 256 --")
    for k in (2, 3, 4, 5, 7, 8, 12, 16):
        a = (data.astype(np.int32) * k) & 0xFF
        c1 = counts(a)
        g = np.gcd(k, BINS)
        inv = pow(k, -1, BINS) if g == 1 else None
        if inv is not None:
            back = (a.astype(np.int32) * inv) & 0xFF
            yoyo = np.array_equal(back, data)
            ytxt = f"yoyo_exact={yoyo}"
        else:
            ytxt = f"yoyo=IRREVERSIBLE (gcd={g})"
        pred = 0.0 if g == 1 else -np.log2(g)
        print(f"  x{k:3d} | H={H(c1):.6f}  dH={H(c1)-h0:+.6f} (pred {pred:+.6f})"
              f"  rigid={rigid(c0, c1)}  support={support(c1)}  {ytxt}")

    print("\n-- what the pattern is, independent of the alphabet --")
    for label, x in (("identity", data),
                     ("translate +48", (data.astype(np.int32) + 48) & 0xFF),
                     ("scale x3", (data.astype(np.int32) * 3) & 0xFF),
                     ("scale x2", (data.astype(np.int32) * 2) & 0xFF)):
        c1 = counts(x)
        p = c1[c1 > 0] / c1.sum()
        print(f"  {label:14s} H_bins={H(c1):.6f}  H_over_distinct_values={-float((p*np.log2(p)).sum()):.6f}"
              f"  support={support(c1)}")
    return 0


if __name__ == "__main__":
    sys.exit(main())
