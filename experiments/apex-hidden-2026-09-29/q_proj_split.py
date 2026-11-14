#!/usr/bin/env python3
"""q_proj_split.py — how much of q_proj's row structure is MAGNITUDE vs SHAPE?

  ROW_SCALE = std(row)                      -> 1D series along the row axis
  ROW_SHAPE = (row - mean(row)) / std(row)  -> same tensor, magnitude removed

Then, for each, measure adjacent-row dependence with the same bias correction and
shuffle controls used everywhere else in this experiment line.

Secondary (owner's caution on the embedding): direct conditional MI per column with a
2-bin split, because 16 bins over 960 samples per column is pure bias.

usage: q_proj_split.py [layer ...]
"""
import json
import struct
import sys
import urllib.request
import numpy as np

REPO = "Qwen/Qwen2.5-0.5B-Instruct"
URL = f"https://huggingface.co/{REPO}/resolve/main/model.safetensors"
UA = {"User-Agent": "curl/8"}
NB = 16


def get_range(a, b):
    r = urllib.request.Request(URL, headers={**UA, "Range": f"bytes={a}-{b}"})
    with urllib.request.urlopen(r, timeout=180) as f:
        return f.read()


HDR = json.load(open("st_header.json"))
BASE = 8 + struct.unpack("<Q", get_range(0, 7))[0]


def fetch(name):
    info = HDR[name]
    nr, nc = info["shape"]
    o0 = BASE + info["data_offsets"][0]
    o1 = o0 + nr * nc * 2 - 1
    u = np.frombuffer(get_range(o0, o1), dtype=np.uint16).astype(np.uint32)
    return (u << 16).view(np.float32).reshape(nr, nc)


def H(c):
    n = c.sum()
    p = c[c > 0] / n
    return float(-(p * np.log2(p)).sum())


def MI(a, b, nb):
    j = np.bincount(a * nb + b, minlength=nb * nb).reshape(nb, nb).astype(np.float64)
    n = j.sum()
    if n == 0:
        return 0.0
    px = j.sum(1) / n
    py = j.sum(0) / n
    p = j / n
    nz = p > 0
    return float((p[nz] * np.log2(p[nz] / (px[:, None] * py[None, :])[nz])).sum())


def bias(n, nb):
    return (nb - 1) ** 2 / (2.0 * n * np.log(2))


def qbin(x, nb, lo=None, hi=None):
    lo = x.min() if lo is None else lo
    hi = x.max() if hi is None else hi
    e = np.linspace(lo, hi, nb + 1)[1:-1]
    return np.searchsorted(e, x.astype(np.float64)).astype(np.int32)


def series_stats(label, s, rng):
    """ROW_SCALE is a 1D sequence: how smooth is it along the row axis?"""
    n = s.size
    print(f"\n  {label}  n={n}  min {s.min():.6g}  max {s.max():.6g}"
          f"  ratio {s.max()/max(s.min(),1e-12):.1f}x")
    q = qbin(s, NB)
    print(f"    H = {H(np.bincount(q, minlength=NB)):.4f} bits of {np.log2(NB):.2f}"
          f"   bias floor(lag1) = {bias(n-1, NB):.6f}")
    print(f"    {'lag':>4s} {'raw MI':>9s} {'corrected':>10s} {'pearson':>9s} {'spearman':>9s}")
    for lag in (1, 2, 4, 8, 16, 32, 64):
        if lag >= n // 4:
            continue
        raw = MI(q[:-lag], q[lag:], NB)
        bi = bias(n - lag, NB)
        pr = np.corrcoef(s[:-lag], s[lag:])[0, 1]
        rk = np.corrcoef(np.argsort(np.argsort(s[:-lag])),
                         np.argsort(np.argsort(s[lag:])))[0, 1]
        print(f"    {lag:4d} {raw:9.6f} {raw-bi:+10.6f} {pr:+9.6f} {rk:+9.6f}")
    sh = s.copy()
    rng.shuffle(sh)
    qs = qbin(sh, NB)
    raw = MI(qs[:-1], qs[1:], NB)
    print(f"    shuffle control lag1: {raw:.6f} corrected {raw-bias(n-1,NB):+.6f}")
    med = np.median(s)
    above = s > med
    flips = int((above[1:] != above[:-1]).sum())
    print(f"    above-median runs: {flips} flips in {n-1} steps"
          f" (random would be ~{(n-1)/2:.0f})"
          f"  mean run {n/max(flips,1):.2f} rows")
    return raw


def matrix_stats(label, mat, rng):
    nr, nc = mat.shape
    q = qbin(mat.ravel(), NB)
    n = q.size
    lag = nc
    base = MI(q[:-lag], q[lag:], NB)
    bi = bias(n - lag, NB)

    qr = q.reshape(nr, nc)[rng.permutation(nr)].ravel()
    mi_rows = MI(qr[:-lag], qr[lag:], NB)

    m = q.reshape(nr, nc).copy()
    for i in range(nr):
        m[i] = m[i][rng.permutation(nc)]
    qc = m.ravel()
    mi_cols = MI(qc[:-lag], qc[lag:], NB)

    qf = q.copy()
    rng.shuffle(qf)
    mi_flat = MI(qf[:-lag], qf[lag:], NB)

    print(f"\n  {label}  shape {nr}x{nc}  n={n:,}  H={H(np.bincount(q, minlength=NB)):.4f}")
    print(f"    lag {lag} raw {base:.6f}  corrected {base-bi:+.6f}"
          f"   | permute rows {mi_rows-bi:+.6f}"
          f" | permute cols/row {mi_cols-bi:+.6f}"
          f" | shuffle all {mi_flat-bi:+.6f}")
    return base - bi, mi_rows - bi, mi_cols - bi


def main():
    layers = [int(x) for x in sys.argv[1:]] or [0, 12]
    rng = np.random.default_rng(41)

    for L in layers:
        key = f"model.layers.{L}.self_attn.q_proj.weight"
        m = fetch(key)
        print(f"\n{'='*78}\n### {key}   {m.shape}")
        sd = m.std(axis=1)
        mu = m.mean(axis=1)
        shape = (m - mu[:, None]) / np.maximum(sd[:, None], 1e-12)

        total, _, _ = matrix_stats("ORIGINAL", m, rng)
        shp, shp_rows, shp_cols = matrix_stats("ROW_SHAPE (row normalised)", shape, rng)

        print(f"\n  --- ROW_SCALE series (std of each row) ---")
        series_stats("ROW_SCALE", sd, rng)
        print(f"\n  --- ROW_MEAN series (mean of each row) ---")
        series_stats("ROW_MEAN", mu, rng)

        print(f"\n  --- decomposition of the row-stride signal (lag {m.shape[1]}) ---")
        print(f"    total  (original)   {total:+.6f} bits")
        print(f"    shape  (scale removed) {shp:+.6f} bits   -> shape share {100*shp/total:.1f}%")
        print(f"    scale  (magnitude only) {total-shp:+.6f} bits   -> scale share"
              f" {100*(total-shp)/total:.1f}%")
        print(f"    shape still dies under row permutation? {shp_rows:+.6f}"
              f"   survives? {'YES' if shp_rows > 0.5*shp else 'NO'}")
        print(f"    shape still needs column pairing?      {shp_cols:+.6f}"
              f"   {'YES' if shp_cols > 0.5*shp else 'NO'}")

    # secondary: direct per-column conditional MI on the embedding (owner's caution)
    print(f"\n{'='*78}\n### embed_tokens: per-column conditional MI, 2-bin split")
    e = fetch("model.embed_tokens.weight")[29466:30426]
    nr, nc = e.shape
    med = np.median(e, axis=0)
    b = (e > med).astype(np.int32)
    vals = []
    for j in range(nc):
        vals.append(MI(b[:-1, j], b[1:, j], 2))
    vals = np.array(vals)
    bi = bias(nr - 1, 2)
    print(f"    mean over {nc} columns: I(V[b,j];V[b+1,j]) = {vals.mean():.6f}"
          f"   bias {bi:.6f}   corrected {vals.mean()-bi:+.6f}")
    print(f"    columns above 0.01 corrected: {(vals - bi > 0.01).sum()} of {nc}")
    rng2 = np.random.default_rng(7)
    sh = b.copy()
    for j in range(nc):
        rng2.shuffle(sh[:, j])
    vals_s = np.array([MI(sh[:-1, j], sh[1:, j], 2) for j in range(nc)])
    print(f"    shuffle control           = {vals_s.mean():.6f}"
          f"   corrected {vals_s.mean()-bi:+.6f}")
    print(f"    => per-column row-to-row dependence:"
          f" {'NONE (column profile only)' if vals.mean()-bi < 0.01 else 'PRESENT'}")
    return 0


if __name__ == "__main__":
    sys.exit(main())
