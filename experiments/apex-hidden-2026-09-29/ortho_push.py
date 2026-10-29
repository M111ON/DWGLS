#!/usr/bin/env python3
"""ortho_push.py — push along the CROSSING axis (not the fold ladder).

Owner spec 2026-10-30: ladder 1 < 4 < 16, "add 3" gives
    1+3  <  4+(4*3)  <  16+(16*3)        i.e.  v -> v*(1+dv)
so the push is proportional to the level value, and dv = k-1 is exactly one rung up.
Push one side +dv, the other side -dv (equal and opposite). Consequences:

    A + B = 2v        mean preserved  ->  the pivot / waist never moves
    A - B = 2*dv*v    the whole push lands in the difference -> gates double

The sign is a function of POSITION (which side of the crossing axis), never of the
value, so the push is value-blind and reversible with zero side information.

Per level this measures: pooled entropy, per-side entropy, mutual information
I(value; side), mean drift, and the exact roundtrip.

usage: ortho_push.py <file> [offset] [length] [k]
"""
import sys, math
import numpy as np

BINS = 256


def hist(x):
    return np.bincount(x.ravel(), minlength=BINS).astype(np.float64)


def entropy(h):
    n = h.sum()
    if n == 0:
        return 0.0
    p = h[h > 0] / n
    return float(-(p * np.log2(p)).sum())


def mi(h_all, h0, h1):
    """I(value; side) = H(pooled) - weighted H(each side)."""
    n0, n1 = h0.sum(), h1.sum()
    n = n0 + n1
    if n == 0:
        return 0.0
    h_cond = (n0 * entropy(h0) + n1 * entropy(h1)) / n
    return entropy(h_all) - h_cond


def split_sides(a):
    """Crossing axis = row parity of the 144-wide field; falls back to position parity."""
    if a.size % 2:
        a = a[:-1]
    return a[0::2], a[1::2]


def run(name, data, k=4, nowrap=False):
    dv = k - 1
    print(f"\n=== {name}  n={data.size}  k={k}  dv={dv}  nowrap={nowrap} ===")
    ladder = [k ** i for i in range(0, 4)]          # 1, 4, 16, 64  (level values)
    print(f"ladder v: {ladder}")
    print(f"push     : v*(1+{dv}) = {[v*(1+dv) for v in ladder]}   v*(1-{dv}) = {[v*(1-dv) for v in ladder]}")

    base0, base1 = split_sides(data.copy())
    h0, h1 = hist(base0), hist(base1)
    h_all = h0 + h1
    print(f"baseline : H={entropy(h_all):.4f}  H0={entropy(h0):.4f}  H1={entropy(h1):.4f}"
          f"  I(val;side)={mi(h_all,h0,h1):.4f}  mean={data.mean():.4f}")

    ok_all = True
    for v in ladder:
        d = dv * v
        if nowrap:                                  # structural truth: no mod-256 fold-back
            a = data.astype(np.int16).copy()
            a[0::2] += d
            a[1::2] -= d
            back = a.copy()
            back[0::2] -= d
            back[1::2] += d
            exact = np.array_equal(back, data.astype(np.int16))
            s0, s1 = split_sides(a)
            lo, hi = int(a.min()), int(a.max()) + 1
            g0 = np.bincount((s0 - lo).ravel(), minlength=hi - lo).astype(np.float64)
            g1 = np.bincount((s1 - lo).ravel(), minlength=hi - lo).astype(np.float64)
            hall = g0 + g1
            base_d = base0.mean() - base1.mean()
            inv_sum = abs((s0.mean() + s1.mean()) - 2 * data.mean()) < 1e-6
            inv_diff = abs((s0.mean() - s1.mean()) - (base_d + 2 * d)) < 1e-6
            ok_all &= exact
            print(f"  v={v:3d} d={d:4d} | H={entropy(hall):.4f}"
                  f"  I(val;side)={mi(hall, g0, g1):.4f}  <- bits moved into position"
                  f"  sum_inv={inv_sum} diff_inv={inv_diff}  exact={exact}")
            continue
        a = data.copy()
        a[0::2] = (a[0::2].astype(np.int32) + d) & 0xFF
        a[1::2] = (a[1::2].astype(np.int32) - d) & 0xFF
        back = a.copy()
        back[0::2] = (back[0::2].astype(np.int32) - d) & 0xFF
        back[1::2] = (back[1::2].astype(np.int32) + d) & 0xFF
        exact = np.array_equal(back, data)

        s0, s1 = split_sides(a)
        g0, g1 = hist(s0), hist(s1)
        hall = g0 + g1
        drift = a.mean() - data.mean()
        # the invariants the spec claims, checked on the values themselves
        mean_a, mean_b = s0.mean(), s1.mean()
        base_d = base0.mean() - base1.mean()
        inv_sum = abs((mean_a + mean_b) - 2 * data.mean()) < 1e-6
        inv_diff = abs((mean_a - mean_b) - (base_d + 2 * d)) < 1e-6
        ok_all &= exact
        info = mi(hall, g0, g1)
        print(f"  v={v:3d} d={d:4d} | H={entropy(hall):.4f} ({entropy(hall)-entropy(h_all):+.4f})"
              f"  I(val;side)={info:.4f}  <- bits moved into position"
              f"  mean_drift={drift:+.2e}"
              f"  sum_inv={inv_sum} diff_inv={inv_diff}  exact={exact}")
    print(f"  roundtrip all levels: {'PASS' if ok_all else 'FAIL'}")
    return ok_all


def main():
    if len(sys.argv) < 2:
        rng = np.random.default_rng(7)
        ok = True
        ok &= run("synthetic random", rng.integers(0, 256, 64 * 144, dtype=np.uint8))
        ok &= run("synthetic smooth", (np.sin(np.arange(64 * 144) / 37.0) * 60 + 128)
                  .astype(np.uint8))
        print(f"\n{'PASS' if ok else 'FAIL'}")
        return 0 if ok else 1

    path = sys.argv[1]
    off = int(sys.argv[2]) if len(sys.argv) > 2 else 0
    ln = int(sys.argv[3]) if len(sys.argv) > 3 else 64 * 144
    k = int(sys.argv[4]) if len(sys.argv) > 4 else 4
    flags = set(sys.argv[5:])
    with open(path, "rb") as f:
        f.seek(off)
        raw = f.read(ln)
    data = np.frombuffer(raw, dtype=np.uint8).copy()
    if "q8" in flags:
        # Q8_0 block = 2-byte fp16 scale + 32 int8; keep the int8 payload only.
        # Requires off to be block-aligned (off % 34 == 0).
        idx = np.arange(data.size)
        data = data[(idx % 34) >= 2]
        print(f"q8 de-block: {data.size} int8 values (scales dropped)")
    print(f"file={path} off={off} len={len(data)}")
    ok = run("real weights", data, k, nowrap="nowrap" in flags)
    print(f"\n{'PASS' if ok else 'FAIL'}")
    return 0 if ok else 1


if __name__ == "__main__":
    sys.exit(main())
