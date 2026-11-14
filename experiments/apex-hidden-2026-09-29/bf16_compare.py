#!/usr/bin/env python3
"""bf16_compare.py — BF16 original vs Q8_0 dequantized, same rows, same bins, same control.

Answers the question that matters: does the positional/adjacency structure exist in the
weights BEFORE quantization?

usage: bf16_compare.py <gguf> <row_start> <nrows> <bf16.npy>
"""
import sys
import numpy as np

NB = 16


def H(c):
    n = c.sum()
    p = c[c > 0] / n
    return float(-(p * np.log2(p)).sum())


def MI(a, b, nb=NB):
    j = np.bincount(a * nb + b, minlength=nb * nb).reshape(nb, nb).astype(np.float64)
    n = j.sum()
    px = j.sum(1) / n
    py = j.sum(0) / n
    p = j / n
    nz = p > 0
    return float((p[nz] * np.log2(p[nz] / (px[:, None] * py[None, :])[nz])).sum())


def bias(n, nb=NB):
    return (nb - 1) ** 2 / (2.0 * n * np.log(2))


def lag_spec(x, lags, rng):
    sh = x.copy()
    rng.shuffle(sh)
    out = []
    for lag in lags:
        a, b = MI(x[:-lag], x[lag:]), MI(sh[:-lag], sh[lag:])
        out.append((lag, a, b, a - b))
    return out


def profile(x, slot=32):
    """I(value ; position mod slot) — is there a per-slot distribution?"""
    hv = H(np.bincount(x, minlength=NB))
    hc = 0.0
    for j in range(slot):
        c = np.bincount(x[j::slot], minlength=NB)
        hc += c.sum() / x.size * H(c)
    return hv - hc


def main():
    gguf, row0, nrows, npypath = sys.argv[1], int(sys.argv[2]), int(sys.argv[3]), sys.argv[4]
    tensor_start = 5947744              # from gguf_probe.py for output.weight
    rowlen, rowbytes = 896, 952
    off = tensor_start + row0 * rowbytes
    with open(gguf, "rb") as f:
        f.seek(off)
        raw = f.read(nrows * rowbytes)
    blk = np.frombuffer(raw, dtype=np.uint8).reshape(-1, 34)
    sc = blk[:, :2].copy().view(np.float16).ravel().astype(np.float32)
    q = blk[:, 2:].astype(np.int8).astype(np.float32)
    dq = (q * sc[:, None]).reshape(nrows, rowlen)

    bf = np.load(npypath)
    print(f"Q8_0 dequantized {dq.shape}   BF16 {bf.shape}")
    print(f"range  Q8_0 {dq.min():.6g}..{dq.max():.6g}   BF16 {bf.min():.6g}..{bf.max():.6g}")
    r = np.corrcoef(dq.ravel(), bf.ravel())[0, 1]
    print(f"pearson(Q8_0 dequant, BF16) = {r:.8f}   max|diff| = {np.abs(dq-bf).max():.3e}")
    print(f"  -> same tensor confirmed: {'YES' if r > 0.999 else 'NO'}\n")

    # identical bin edges from the BF16 distribution, applied to both
    edges = np.quantile(bf.ravel().astype(np.float64), np.linspace(0, 1, NB + 1)[1:-1])
    qb = np.searchsorted(edges, bf.ravel().astype(np.float64))
    qq = np.searchsorted(edges, dq.ravel().astype(np.float64))

    print("## entropy and profile\n")
    print(f"  {'':10s} {'H(bits)':>10s} {'I(V;slot mod 32)':>18s}")
    for label, x in (("BF16", qb), ("Q8_0", qq)):
        print(f"  {label:10s} {H(np.bincount(x, minlength=NB)):10.6f} {profile(x):18.6f}")

    print("\n## lag spectrum, same bin edges, same shuffle control\n")
    print(f"  {'lag':>4s} | {'BF16 real':>10s} | {'Q8_0 real':>10s} | difference")
    rng = np.random.default_rng(23)
    lb = lag_spec(qb, [1, 2, 4, 8, 16, 32, 64, 896], rng)
    lq = lag_spec(qq, [1, 2, 4, 8, 16, 32, 64, 896], rng)
    for (lag, ab, _, rb), (_, aq, _, rq) in zip(lb, lq):
        print(f"  {lag:4d} | {rb:+10.6f} | {rq:+10.6f} | {rq-rb:+.6f}")
    print(f"\n  bias floor (n={qb.size}) = {bias(qb.size-1):.6f}")

    print("\n## row-to-row correlation at the same column (lag 896)\n")
    for label, m in (("BF16", bf), ("Q8_0", dq)):
        rl = np.corrcoef(m[:-1].ravel(), m[1:].ravel())[0, 1]
        print(f"  {label:6s} pearson(lag 896) = {rl:+.6f}")
    return 0


if __name__ == "__main__":
    sys.exit(main())
