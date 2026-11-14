#!/usr/bin/env python3
"""adjacent_structure.py — does a transform keep the RELATIONSHIP, not just the histogram?

Owner request 2026-10-30: after the histogram test, measure W[i] <-> W[i+1] before
and after the push, to separate "distribution shape preserved" from "internal
relationship preserved". Also checks the claim that the incremental entropy loss
per doubling is the conditional entropy of the bit that gets cut.

Honest framing notes:
  * `rigid` on the 1D histogram says nothing about spatial order. This script adds a
    2D rigid test (sorted joint counts) and a mutual-information test, which DO
    measure the pairwise relationship.
  * A bijection applied to both sides leaves MI EXACTLY unchanged (I(g(X);g(Y))=I(X;Y)),
    so MI is the right invariant to test. Pearson is only affine-invariant.

usage: adjacent_structure.py [file] [offset] [len] [q8]
"""
import sys
import numpy as np

M = 8
BINS = 256


def H(c):
    n = c.sum()
    p = c[c > 0] / n
    return float(-(p * np.log2(p)).sum())


def counts(x):
    return np.bincount(x.ravel(), minlength=BINS).astype(np.int64)


def joint(x):
    """adjacent-pair joint counts, 256x256"""
    a = x[:-1].astype(np.int64)
    b = x[1:].astype(np.int64)
    return np.bincount(a * BINS + b, minlength=BINS * BINS).reshape(BINS, BINS).astype(np.int64)


def MI(j):
    n = j.sum()
    px = j.sum(1) / n
    py = j.sum(0) / n
    pxy = j / n
    nz = pxy > 0
    return float((pxy[nz] * np.log2(pxy[nz] / (px[:, None] * py[None, :])[nz])).sum())


def mi_bias(n, nx=BINS, ny=BINS):
    """plug-in MI is biased upward; this is the floor you must beat."""
    return (nx - 1) * (ny - 1) / (2.0 * n * np.log(2))


def rigid1(c0, c1):
    return bool(np.array_equal(np.sort(c0), np.sort(c1)))


def rigid2(j0, j1):
    return bool(np.array_equal(np.sort(j0.ravel()), np.sort(j1.ravel())))


def main():
    if len(sys.argv) > 1:
        path = sys.argv[1]
        off = int(sys.argv[2]) if len(sys.argv) > 2 else 0
        ln = int(sys.argv[3]) if len(sys.argv) > 3 else 1020000
        with open(path, "rb") as f:
            f.seek(off)
            raw = f.read(ln)
        blob = np.frombuffer(raw, dtype=np.uint8).copy()
        name = path.split("\\")[-1]
        q8 = len(sys.argv) > 4 and sys.argv[4] == "q8"
        scales = None
        if q8:
            nblk = blob.size // 34
            blocks = blob[: nblk * 34].reshape(nblk, 34)
            scales = blocks[:, :2].copy().view(np.uint16).ravel()
            data = blocks[:, 2:].ravel().copy()
        else:
            data = blob
    else:
        rng = np.random.default_rng(7)
        data = rng.integers(0, 256, 960000, dtype=np.uint8)
        name = "synthetic random"
        scales = None

    print(f"# KIS adjacency / structure baseline")
    print(f"\nfile: `{name}`  n={data.size}  (adjacent pairs = {data.size-1})")
    if scales is not None:
        print(f"fp16 scales carried alongside: {scales.size} values")

    c0 = counts(data)
    j0 = joint(data)
    h0 = H(c0)
    mi0 = MI(j0)
    bias = mi_bias(data.size - 1)
    print(f"\nbaseline  H={h0:.6f} / {M:.3f}  support={int((c0>0).sum())}")
    print(f"          I(W[i];W[i+1]) = {mi0:.6f} bits   (plug-in bias floor {bias:.6f})")
    print(f"          net above floor = {mi0-bias:+.6f} bits")
    x = data.astype(np.float64)
    for lag in (1, 2, 3, 4, 8, 16):
        r = np.corrcoef(x[:-lag], x[lag:])[0, 1]
        print(f"          pearson lag {lag:2d} = {r:+.6f}")

    print("\n## 1) what the doubling map actually cuts")
    print("   x*2^j mod 256 keeps the LOW m-j bits (drops the TOP j bits).")
    print("   That is NOT x>>j (keep high bits). Both are projections, different partitions.")
    print("\n| j | H(x*2^j mod 256) | drop vs H(x) | H(x>>j) | drop (high-bit coarse-grain) | H(cut bits | kept) |")
    print("|---|---|---|---|---|---|")
    hj = h0
    for j in range(1, 5):
        a = (data.astype(np.int64) * (2 ** j)) % BINS
        ha = H(counts(a))
        hb = H(np.bincount((data >> j).ravel(), minlength=BINS >> j).astype(np.int64))
        cond = hj - hb
        print(f"| {j} | {ha:.6f} | {ha-h0:+.6f} | {hb:.6f} | {hb-h0:+.6f} | {cond:.6f} |")
        hj = hb

    print("\n## 2) relationship preserved or not, per transform")
    print("\n| transform | H | dH | rigid 1D | I(W[i];W[i+1]) | dMI | rigid 2D | pearson lag1 |")
    print("|---|---|---|---|---|---|---|---|")
    cases = [
        ("identity", lambda v: v),
        ("translate +1", lambda v: (v + 1) % 256),
        ("translate +48", lambda v: (v + 48) % 256),
        ("translate +192", lambda v: (v + 192) % 256),
        ("scale x3 (odd)", lambda v: (v * 3) % 256),
        ("scale x5 (odd)", lambda v: (v * 5) % 256),
        ("scale x7 (odd)", lambda v: (v * 7) % 256),
        ("scale x2 (frustum)", lambda v: (v * 2) % 256),
        ("scale x4 (frustum)", lambda v: (v * 4) % 256),
    ]
    for label, fn in cases:
        a = fn(data.astype(np.int64)).astype(np.uint8)
        c1, j1 = counts(a), joint(a)
        h1, m1 = H(c1), MI(j1)
        r1 = np.corrcoef(a[:-1].astype(np.float64), a[1:].astype(np.float64))[0, 1]
        print(f"| {label} | {h1:.6f} | {h1-h0:+.6f} | {rigid1(c0,c1)} | {m1:.6f} | {m1-mi0:+.6f}"
              f" | {rigid2(j0,j1)} | {r1:+.6f} |")

    print("\n## 3) the scale stream (fp16) — where the magnitude actually lives")
    if scales is not None:
        hs = H(np.bincount(scales.astype(np.int64), minlength=65536).astype(np.int64))
        xs = scales.astype(np.float64)
        rs = np.corrcoef(xs[:-1], xs[1:])[0, 1]
        uniq = int(np.unique(scales).size)
        print(f"\nH(scale as uint16) = {hs:.4f} bits   distinct values = {uniq}"
              f"   pearson lag1 = {rs:+.6f}")
        print(f"payload H = {h0:.4f} of 8 bits; scale H = {hs:.4f} of 16 bits")
    return 0


if __name__ == "__main__":
    sys.exit(main())
