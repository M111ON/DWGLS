#!/usr/bin/env python3
"""position_profile.py — separate POSITION PROFILE from CROSS-BLOCK CORRELATION.

Two different things can both produce the lag-32 MI seen in the baseline:

  A) position profile    : slot j of a block has its own distribution, and two draws
                           from the SAME slot are still independent given j
  B) cross-block corr.   : block b slot j actually predicts block b+1 slot j

They are separated by conditioning on the slot:

    I(V ; pos)                     -> how strong the profile is (A)
    mean_j I(X[b,j] ; X[b+1,j])    -> real cross-block correlation (B), 0 under A

Shuffle block order keeps position mod 32 aligned, so it cannot tell A from B.
Shuffle inside block destroys both. Only the fixed-slot test discriminates.

Same bin edges and same shuffle control are used for every measurement.

usage: position_profile.py <gguf> <offset> <nblocks> [q8]
"""
import sys
import numpy as np

NBINS = 16


def ent(c):
    n = c.sum()
    p = c[c > 0] / n
    return float(-(p * np.log2(p)).sum())


def mi_pair(a, b, nbins=NBINS):
    j = np.bincount(a * nbins + b, minlength=nbins * nbins).reshape(nbins, nbins).astype(np.float64)
    n = j.sum()
    if n == 0:
        return 0.0
    px = j.sum(1) / n
    py = j.sum(0) / n
    p = j / n
    nz = p > 0
    return float((p[nz] * np.log2(p[nz] / (px[:, None] * py[None, :])[nz])).sum())


def bias(n, nx=NBINS, ny=NBINS):
    return (nx - 1) * (ny - 1) / (2.0 * n * np.log(2))


def main():
    path, off, nblocks = sys.argv[1], int(sys.argv[2]), int(sys.argv[3])
    q8 = len(sys.argv) > 4 and sys.argv[4] == "q8"
    with open(path, "rb") as f:
        f.seek(off)
        raw = f.read(nblocks * 34)
    blob = np.frombuffer(raw, dtype=np.uint8).copy()
    nb = blob.size // 34
    blocks = blob[: nb * 34].reshape(nb, 34)
    if not q8:
        print("expects Q8_0 blocks")
        return 1
    pay = blocks[:, 2:].astype(np.int16)          # (nb, 32) int8 payload
    vals = pay.ravel()
    print(f"tensor slice: {nb} Q8_0 blocks x 32 = {vals.size} values\n")

    # fixed bin edges from the pooled distribution, used everywhere
    edges = np.quantile(vals.astype(np.float64),
                        np.linspace(0, 1, NBINS + 1)[1:-1])
    q = np.searchsorted(edges, vals.astype(np.float64))
    qb = q.reshape(nb, 32)
    pos = np.tile(np.arange(32), nb)

    print("## A) position profile — does slot j have its own distribution?\n")
    hv = ent(np.bincount(q, minlength=NBINS))
    h_cond = 0.0
    per_slot = []
    for j in range(32):
        c = np.bincount(qb[:, j], minlength=NBINS)
        per_slot.append(ent(c))
        h_cond += c.sum() / q.size * ent(c)
    i_prof = hv - h_cond
    print(f"  H(value)          = {hv:.6f} bits")
    print(f"  H(value | slot)   = {h_cond:.6f} bits")
    print(f"  I(value ; slot)   = {i_prof:.6f} bits   <- profile strength")
    print(f"  bias floor        = {bias(q.size, NBINS, 32):.6f}")
    print(f"  per-slot H: min {min(per_slot):.4f}  max {max(per_slot):.4f}"
          f"  spread {max(per_slot)-min(per_slot):.4f}")

    print("\n## B) cross-block correlation at a FIXED slot\n")
    print("   under a pure position profile this must be ~0: slot is held fixed,")
    print("   so only a real block-to-block dependence can survive.\n")
    same, misaligned = [], []
    rng = np.random.default_rng(17)
    order = np.arange(nb - 1)
    shuffled = order.copy()
    rng.shuffle(shuffled)
    for j in range(32):
        a = qb[:-1, j]
        b = qb[1:, j]
        same.append(mi_pair(a, b))
        a2 = qb[:-1, j]
        b2 = qb[1:, (j + 1) % 32]
        misaligned.append(mi_pair(a2, b2))
    n_pairs = nb - 1
    print(f"  mean I(X[b,j] ; X[b+1,j])      = {np.mean(same):.6f}  (bias {bias(n_pairs):.6f})")
    print(f"  mean I(X[b,j] ; X[b+1,j+1])    = {np.mean(misaligned):.6f}   <- misaligned control")
    print(f"  max over slots                 = {np.max(same):.6f}  (slot {int(np.argmax(same))})")
    print(f"  min over slots                 = {np.min(same):.6f}  (slot {int(np.argmin(same))})")

    print("\n## C) same tests after shuffling the block order\n")
    qs = qb[shuffled]
    same_s, mis_s = [], []
    for j in range(32):
        same_s.append(mi_pair(qs[:-1, j], qs[1:, j]))
        mis_s.append(mi_pair(qs[:-1, j], qs[1:, (j + 1) % 32]))
    print(f"  mean I(X[b,j] ; X[b+1,j])   = {np.mean(same_s):.6f}")
    print(f"  mean I(X[b,j] ; X[b+1,j+1]) = {np.mean(mis_s):.6f}")

    print("\n## D) reference: the raw lag-32 MI the baseline reported\n")
    flat = qb.ravel()
    print(f"  I(X[i] ; X[i+32]) pooled over all slots = {mi_pair(flat[:-32], flat[32:]):.6f}")
    print(f"  I(X[i] ; X[i+1])                        = {mi_pair(flat[:-1], flat[1:]):.6f}")
    print(f"  bias floor (n={flat.size-32})            = {bias(flat.size-32):.6f}")

    print("\n## verdict\n")
    if np.mean(same) - bias(n_pairs) < 0.01:
        print("  cross-block correlation at fixed slot is at the noise floor")
        print("  => the lag-32 MI is a POSITION PROFILE, not block-to-block dependence")
    else:
        print(f"  cross-block correlation at fixed slot = {np.mean(same)-bias(n_pairs):+.6f} bits")
        print("  => there is real dependence between consecutive blocks, not only a profile")
    print(f"  profile strength I(value;slot) = {i_prof:.6f} bits")
    return 0


if __name__ == "__main__":
    sys.exit(main())
