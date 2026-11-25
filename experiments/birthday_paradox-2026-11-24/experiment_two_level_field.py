"""Two-level field cost law, measured on the real ANN corpus.

Carries the birthday-paradox / light-cone POC (POC_BD_PDXF.py) onto the live
system instead of a synthetic distribution.

  Stage 1  routing   : compare the query against K door centroids   -> K probes
  Stage 2  scan      : compare against the rows behind top-b doors  -> b*N/K probes

The POC minimised 1.1774*(sqrt(n) + sqrt(S/n)) and found a crossover near
n = 24-30 for S = 720.  That crossover IS the birthday scale of the state
space: sqrt(720) = 26.8.  Same law, real units: probes = K + b*N/K, minimum at
K* = sqrt(b*N) where the two stages cost the same.

Data (read-only; nothing written into I:/tools/ann):
  V         I:/tools/ann/data/memcore/ann_index.jsonl.V.npy   (54527 x 768 f32, unit)
  table     I:/tools/ann/data/memcore/kv_anchors.bin          (real K=64 centroids)

Faithfulness notes (stated, not hidden):
  * distances are exact squared L2, like anch_assign (NOT max-dot: centroid
    norms differ, so the two are not interchangeable).
  * k-means is a numpy port of anch_train (deterministic evenly-spaced init),
    trained on a 20k-row subsample for ITERS=15 instead of 25.
"""
import json
import os
import struct

import numpy as np

ANN = r"I:\tools\ann\data\memcore"
V_PATH = os.path.join(ANN, "ann_index.jsonl.V.npy")
A_PATH = os.path.join(ANN, "kv_anchors.bin")
OUT = os.path.join(os.path.dirname(os.path.abspath(__file__)),
                   "experiment_two_level_field.json")

K_SWEEP = [8, 16, 24, 32, 48, 64, 96, 144, 192, 256]
TRAIN_ROWS = 20000
ITERS = 15
NQ = 300
TOPK = 10
SEED = 42


def load_real_table():
    with open(A_PATH, "rb") as f:
        magic, K, dim, ntr, _cks = struct.unpack("<5I", f.read(20))
        assert magic == 0x414E4352, "not an ANCR table"
        C = np.frombuffer(f.read(K * dim * 4), dtype=np.float32).reshape(K, dim)
    return C, K, dim, ntr


def d2(X, C):
    """Exact squared L2, X[n,d] vs C[K,d] -> [n,K] float32."""
    return (X * X).sum(1)[:, None] - 2.0 * (X @ C.T) + (C * C).sum(1)[None, :]


def train(X, K, iters=ITERS):
    n = X.shape[0]
    C = X[np.arange(K) * n // K].copy()          # anch_train init
    for _ in range(iters):
        lab = np.argmin(d2(X, C), axis=1)
        for k in range(K):
            m = lab == k
            if m.any():
                C[k] = X[m].mean(axis=0)
    return C


def main():
    V = np.asarray(np.load(V_PATH, mmap_mode="r"), dtype=np.float32)
    N, D = V.shape
    print(f"corpus V {N} x {D} (unit norm)")

    Creal, Kreal, dim, ntr = load_real_table()
    print(f"real table K={Kreal} dim={dim} ntr={ntr}")

    rng = np.random.default_rng(SEED)
    qidx = rng.choice(N, size=NQ, replace=False)
    Q = V[qidx]

    dots = Q @ V.T
    dots[np.arange(NQ), qidx] = -np.inf           # leave-one-out (#7697)
    oracle = np.argpartition(-dots, TOPK, axis=1)[:, :TOPK]

    # ---- the cone, with the real membership rule ---------------------------
    # The live roster holds topb=2 memberships per row (/health: rows 130550
    # = 2 x 65275), so a row is reachable as soon as ONE of its two doors is
    # walked. Sizes are memberships, not argmin counts.
    TOPB = 2

    def cone(C, K):
        """Per-query door order, membership sizes, and the b-curve.

        rows are DISTINCT: a row is walked at radius b if any of its
        memberships sits in the first b doors. walked counts memberships
        (what a non-deduping walk would touch)."""
        mem = np.argpartition(d2(V, C), TOPB, axis=1)[:, :TOPB]   # [N,TOPB]
        sizes = np.bincount(mem.ravel(), minlength=K).astype(np.float64)
        qd = np.argsort(d2(Q, C), axis=1)                        # doors nearest-first
        rank = np.empty_like(qd)
        np.put_along_axis(rank, qd, np.arange(K)[None, :].repeat(NQ, 0), axis=1)
        ra = np.stack([rank[qi][mem].min(1) for qi in range(NQ)])  # best door rank/row
        r = ra[np.arange(NQ)[:, None], oracle]                   # ... for true neighbours
        bs = np.arange(1, K + 1)
        rec = (r[:, :, None] < bs[None, None, :]).sum(1).mean(0) / TOPK
        hist = np.stack([np.bincount(ra[qi], minlength=K + 1) for qi in range(NQ)])
        distinct = np.cumsum(hist, axis=1)[:, :K].mean(0)
        walked = np.cumsum(sizes[qd], axis=1).mean(0)
        return mem, sizes, bs, rec, distinct, walked, r

    mem_r, sizes_r, bs_r, rec_r, rows_r, walked_r, r_or = cone(Creal, Kreal)
    print(f"\n[real K=64] door membership min/med/mean/max = "
          f"{sizes_r.min():.0f}/{np.median(sizes_r):.0f}/"
          f"{sizes_r.mean():.0f}/{sizes_r.max():.0f}  "
          f"(total {sizes_r.sum():.0f} = {sizes_r.sum() / N:.2f} x N)")
    print(f"[real K=64] imbalance max/mean = {sizes_r.max() / sizes_r.mean():.2f}")

    # ---- cone aperture: how much of the true top-10 the route can even see --
    aperture = {}
    for b in (1, 2, 3, 5, 8, 16, 24):
        frac = (r_or < b).mean(1)
        all10 = (r_or < b).all(1)
        aperture[b] = {"recall_ceiling": float(frac.mean()),
                       "all10_in_cone": float(all10.mean()),
                       "rows_walked_mean": float(rows_r[b - 1])}
        print(f"[cone b={b:2d}] ceiling {frac.mean():.4f}  "
              f"all-top10-inside {all10.mean():.3f}  "
              f"rows walked {rows_r[b - 1]:.0f}")

    # ---- K sweep: recall-per-probe frontier --------------------------------
    tr = V[rng.choice(N, size=TRAIN_ROWS, replace=False)]
    sweep = []
    for K in K_SWEEP:
        C = train(tr, K)
        mem, sizes, bs, rec, rows, walked, _r = cone(C, K)
        at = {}
        for target in (0.90, 0.95, 0.99):
            ok = np.flatnonzero(rec >= target)
            at[int(target * 100)] = (
                {"b": int(bs[ok[0]]), "rows": float(rows[ok[0]])} if ok.size else None)
        sweep.append({"K": K, "recall_by_b": rec.tolist(), "rows_by_b": rows.tolist(),
                      "thresholds": at,
                      "door_max": int(sizes.max()), "walked_at_b": walked.tolist(),
                      "door_max_over_mean": float(sizes.max() / sizes.mean())})
        t = " ".join(f"r{k}=" + (f"{v['rows']:.0f}rows/b{v['b']}"
                                  if v else "n/a") for k, v in at.items())
        print(f"[K={K:3d}] {t} | door max/mean {sizes.max() / sizes.mean():.2f}")

    # cheapest measured way to each recall target, across every K
    print("\ncheapest (K,b) per recall target:")
    for target in (0.90, 0.95, 0.99):
        cand = [(s["thresholds"][int(target * 100)]["rows"], s["K"],
                 s["thresholds"][int(target * 100)]["b"])
                for s in sweep if s["thresholds"][int(target * 100)]]
        if cand:
            r_, k_, b_ = min(cand)
            print(f"  recall>={target}: K={k_} b={b_} rows={r_:.0f} "
                  f"({r_ / N * 100:.1f}% of corpus)")

    # the deployed table on the same axes
    real_at = {}
    for target in (0.90, 0.95, 0.99):
        ok = np.flatnonzero(rec_r >= target)
        real_at[int(target * 100)] = (
            {"b": int(bs_r[ok[0]]), "rows": float(rows_r[ok[0]])} if ok.size else None)
    print(f"\n[real K=64] " + " ".join(
        f"r{k}=" + (f"{v['rows']:.0f}rows/b{v['b']}" if v else "n/a")
        for k, v in real_at.items()))

    # ---- light-cone budget curve at the real table -------------------------
    # radius = doors the cone admits (b); budget = rows examined.
    curve = []
    for b in (1, 2, 3, 4, 6, 8, 12, 16, 24, 32, 64):
        curve.append({"b": b, "rows_examined_mean": float(rows_r[b - 1]),
                      "frac_of_corpus": float(rows_r[b - 1] / N),
                      "recall": float(rec_r[b - 1])})
        print(f"[cone b={b:2d}] rows {rows_r[b - 1]:6.0f} "
              f"({rows_r[b - 1] / N * 100:5.1f}%) recall@10 {rec_r[b - 1]:.4f}")

    # timelike / overflow / spacelike split of the top-10, at the deployed b=2
    tot = NQ * TOPK
    inside = (r_or < 2).sum() / tot
    outside = (r_or < 8).sum() / tot
    split = {"found_inside": inside,
             "recoverable_b8": outside - inside,
             "spacelike_residual": 1.0 - outside}
    print(f"\n[b=2 cone] top-10 inside {split['found_inside']:.3f} | "
          f"recoverable by b=8 {split['recoverable_b8']:.3f} | "
          f"outside b=8 (spacelike residual) {split['spacelike_residual']:.3f}")

    # ---- birthday arithmetic: the field's own optimum ----------------------
    print("\nbirthday scale of a state space M:")
    for M in (720, 20736):
        s = M ** 0.5
        print(f"  M={M:6d}  sqrt(M)={s:7.2f}  k50=1.1774*sqrt(M)={1.1774 * s:7.2f}"
              f"  two-level optimum n*={s:.0f}")

    res = {"corpus": [N, D], "real_table": {"K": Kreal, "dim": dim, "ntr": ntr},
           "door_memberships": {"min": int(sizes_r.min()),
                                "median": int(np.median(sizes_r)),
                                "mean": float(sizes_r.mean()),
                                "max": int(sizes_r.max()),
                                "total": int(sizes_r.sum()), "topb": TOPB},
           "aperture_real_table": aperture, "k_sweep": sweep,
           "real_table_frontier": real_at,
           "cone_budget_curve": curve, "split_b2": split,
           "birthday": {str(M): {"sqrt": M ** 0.5, "k50": 1.1774 * M ** 0.5}
                        for M in (720, 20736)}}
    with open(OUT, "w", encoding="utf-8") as f:
        json.dump(res, f, indent=2)
    print(f"\nwrote {OUT}")


if __name__ == "__main__":
    main()
