#!/usr/bin/env python3
"""bit_cost_and_lags.py — (a) which bit each projection actually cuts and what it costs,
(b) a lag spectrum: where in the weight does structure actually live?

Corrects an earlier labelling error: x*2^j mod 256 merges values differing in the TOP
bits, so it cuts bit 7 first; x>>j cuts bit 0 first. The two chains are different
projections and must not be described as one "cheap/expensive bit" ordering.

usage: bit_cost_and_lags.py [file] [offset] [len] [q8]
"""
import sys
import numpy as np

BINS = 256


def H(c):
    n = c.sum()
    p = c[c > 0] / n
    return float(-(p * np.log2(p)).sum())


def cond_entropy(x, kept_mask, bit):
    """H(bit | the bits in kept_mask) computed exactly from the joint histogram."""
    key = np.zeros(x.size, dtype=np.int64)
    for j in kept_mask:
        key = key * 2 + ((x >> j) & 1)
    b = (x >> bit) & 1
    k = int(key.max()) + 1
    j2 = np.bincount(key * 2 + b, minlength=k * 2).reshape(k, 2).astype(np.float64)
    h = 0.0
    n = j2.sum()
    for row in j2:
        s = row.sum()
        if s:
            h += s / n * H(row)
    return h


def MI(x, lag, q=BINS):
    a = x[:-lag].astype(np.int64)
    b = x[lag:].astype(np.int64)
    j = np.bincount(a * q + b, minlength=q * q).reshape(q, q).astype(np.float64)
    n = j.sum()
    px = j.sum(1) / n
    py = j.sum(0) / n
    p = j / n
    nz = p > 0
    return float((p[nz] * np.log2(p[nz] / (px[:, None] * py[None, :])[nz])).sum())


def main():
    path = sys.argv[1] if len(sys.argv) > 1 else None
    if path:
        off = int(sys.argv[2]) if len(sys.argv) > 2 else 0
        ln = int(sys.argv[3]) if len(sys.argv) > 3 else 1020000
        with open(path, "rb") as f:
            f.seek(off)
            raw = f.read(ln)
        blob = np.frombuffer(raw, dtype=np.uint8).copy()
        name = path.split("\\")[-1]
        if len(sys.argv) > 4 and sys.argv[4] == "q8":
            nb = blob.size // 34
            blocks = blob[: nb * 34].reshape(nb, 34)
            data = blocks[:, 2:].ravel().copy()
        else:
            data = blob
    else:
        rng = np.random.default_rng(7)
        data = rng.integers(0, 256, 960000, dtype=np.uint8)
        name = "synthetic random"

    print(f"### {name}  n={data.size}\n")
    print("## A) which bit each projection cuts, and its exact cost\n")
    print("| bit | H(bit) alone |")
    print("|---|---|")
    for j in range(7, -1, -1):
        c = np.bincount((data >> j) & 1, minlength=2)
        print(f"| b{j} | {H(c):.6f} |")

    print("\nchain 1 — `x*2^j mod 256` (merges values differing in the TOP bits;")
    print("          partition is by the LOW 8-j bits, so each cut bit is conditioned on the LOW ones)")
    print("\n| j | bits cut | cumulative drop | increment = H(cut bit | kept LOW bits) |")
    print("|---|---|---|---|")
    for j in range(1, 5):
        bit = 8 - j
        kept = list(range(0, bit))          # kept bits are the LOW ones
        cond = cond_entropy(data, kept, bit)
        cum = H(np.bincount(((data.astype(np.int64) * (2 ** j)) % BINS).ravel(), minlength=BINS).astype(np.int64)) - H(np.bincount(data, minlength=BINS).astype(np.int64))
        print(f"| {j} | b{bit} | {cum:+.6f} | {cond:.6f} |")

    print("\nchain 2 — `x>>j` (merges values differing in the BOTTOM bits;")
    print("          partition is by the HIGH bits, so each cut bit is conditioned on the HIGH ones)")
    print("\n| j | bits cut | cumulative drop | increment = H(cut bit | kept HIGH bits) |")
    print("|---|---|---|---|")
    h0 = H(np.bincount(data, minlength=BINS).astype(np.int64))
    for j in range(1, 5):
        bit = j - 1
        kept = list(range(bit + 1, 8))      # kept bits are the HIGH ones
        cond = cond_entropy(data, kept, bit)
        cum = H(np.bincount((data >> j).ravel(), minlength=BINS >> j).astype(np.int64)) - h0
        print(f"| {j} | b{bit} | {cum:+.6f} | {cond:.6f} |")

    print("\n## B) lag spectrum — where does structure live?\n")
    print("`real` = MI(original) - MI(shuffled, same N, same estimator).")
    print("Lags chosen to hit Qwen2.5-0.5B structure: 32 = Q8_0 block payload, 34 = block stride,")
    print("896 = hidden_size, 4864 = intermediate_size.\n")
    print("| lag | MI original | MI shuffled | real |")
    print("|---|---|---|---|")
    rng = np.random.default_rng(11)
    lags = list(range(1, 9)) + [16, 32, 33, 34, 35, 64, 128, 256, 512, 896, 897, 1792, 4864]
    for lag in lags:
        if lag >= data.size // 4:
            continue
        a = MI(data, lag)
        sh = data.copy()
        rng.shuffle(sh)
        b = MI(sh, lag)
        mark = " **" if (a - b) > 0.02 else ""
        print(f"| {lag} | {a:.6f} | {b:.6f} | {a-b:+.6f}{mark} |")

    print("\n## C) is the lag-32 structure the block SCALE, or the model?\n")
    if len(sys.argv) > 4 and sys.argv[4] == "q8":
        nblk = blob.size // 34
        blocks = blob[: nblk * 34].reshape(nblk, 34)
        sc = blocks[:, :2].copy().view(np.uint16).ravel().astype(np.float64)
        print(f"block scales: n={sc.size}")
        for lag in (1, 2, 4, 8):
            r = np.corrcoef(sc[:-lag], sc[lag:])[0, 1]
            print(f"  pearson of scales lag {lag} = {r:+.6f}")
        rng2 = np.random.default_rng(3)
        sh = sc.copy()
        rng2.shuffle(sh)
        print(f"  control (shuffled scales) lag1 = {np.corrcoef(sh[:-1], sh[1:])[0,1]:+.6f}")
        print("\n  MI at lag 32 split by whether the two blocks share a scale:")
        same, diff = [], []
        for i in range(0, min(nblk - 1, 20000)):
            a = blocks[i, 2:]
            b = blocks[i + 1, 2:]
            if blocks[i, 0] == blocks[i + 1, 0] and blocks[i, 1] == blocks[i + 1, 1]:
                same.append((a, b))
            else:
                diff.append((a, b))
        for label, grp in (("same scale", same), ("diff scale", diff)):
            if not grp:
                print(f"  {label}: no pairs")
                continue
            a = np.concatenate([g[0] for g in grp]).astype(np.int64)
            b = np.concatenate([g[1] for g in grp]).astype(np.int64)
            j = np.bincount(a * 256 + b, minlength=65536).reshape(256, 256).astype(np.float64)
            n = j.sum()
            px = j.sum(1) / n
            py = j.sum(0) / n
            p = j / n
            nz = p > 0
            mi = float((p[nz] * np.log2(p[nz] / (px[:, None] * py[None, :])[nz])).sum())
            print(f"  {label}: pairs={len(grp)}  MI={mi:.6f}")
    return 0


if __name__ == "__main__":
    sys.exit(main())
