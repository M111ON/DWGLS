#!/usr/bin/env python3
"""freeze_park.py — does a FROZEN k-means map absorb new items without moving any address?

Protocol mirrors I:/tools/ann/experiments/container-2026-10-30/REPORT.md (phase B/C):
the partition is learned once on BASE, frozen, and later items are assigned to their
nearest frozen centroid. Ground truth is brute-force cosine, recomputed over the rows
that exist at the moment of measurement (same convention as phase C there).

  python freeze_park.py [--K 64] [--seed-split 20261030] [--npz ../../data/glove_wordnet.npz]
"""
import argparse
import os

import numpy as np

from multiview_real import kmeans

HERE = os.path.dirname(os.path.abspath(__file__))
ROOT = os.path.dirname(os.path.dirname(HERE))
NPZ = os.path.join(ROOT, "data", "glove_wordnet.npz")

SEED_KMEANS = 7          # same seed multiview_real.py uses for its value view
SEED_HELD_Q = 11
K_TOP = 10
N_BASE, N_HELD = 16000, 4000


def assign(X, C):
    return np.argmax(X @ C.T, axis=1)


def members(X, C, rows):
    """Assign rows[..] to the frozen centroids; return labels + GLOBAL index per bucket."""
    a = assign(X[rows], C)
    return a, [rows[a == c] for c in range(len(C))]


def gt_top10(X, rows, qi, k=K_TOP):
    """Brute-force cosine top-k among `rows` for query row qi."""
    s = X[rows] @ X[qi]
    return rows[np.argsort(-s)[:k]]


def recall(X, qs, C, mem, b, gt, k=K_TOP):
    """recall@k: rank the scanned candidates by cosine, count hits in gt."""
    out = []
    for q in qs:
        top = np.argsort(-(X[q] @ C.T))[:b]
        cand = np.concatenate([mem[c] for c in top]) if len(top) else np.array([], int)
        if len(cand) == 0:
            out.append(0.0)
            continue
        s = X[cand] @ X[q]
        got = cand[np.argsort(-s)[:k]]
        out.append(len(set(got.tolist()) & set(gt[q].tolist())) / float(k))
    return out


def boot_ci(a, b, seed=5, iters=4000):
    """95% CI of the mean paired difference a - b, resampling queries."""
    rng = np.random.default_rng(seed)
    d = np.asarray(a, dtype=float) - np.asarray(b, dtype=float)
    m = np.array([rng.choice(d, size=len(d), replace=True).mean() for _ in range(iters)])
    return float(np.percentile(m, 2.5)), float(np.percentile(m, 97.5))


def matched_churn(a_frozen, a_ctrl, K):
    """Moved-bucket count after the best relabelling of the control clusters.
    Lower bound on the physical cost: even if buckets were renamed to line up,
    how many items still sit in a different partition cell."""
    M = np.zeros((K, K), dtype=int)
    for f, c in zip(a_frozen, a_ctrl):
        M[f, c] += 1
    taken, fmap = set(), {}
    for f in np.argsort(-M.sum(axis=1)):
        for c in np.argsort(-M[f]):
            if int(c) not in taken:
                fmap[int(f)] = int(c)
                taken.add(int(c))
                break
    return int(sum(fmap[int(a_frozen[i])] != a_ctrl[i] for i in range(len(a_frozen))))


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--K", type=int, default=64)
    ap.add_argument("--seed-split", type=int, default=20261030)
    ap.add_argument("--npz", default=NPZ)
    args = ap.parse_args()
    K = args.K

    d = np.load(args.npz, allow_pickle=True)
    X = d["X"].astype(np.float32)
    n = len(X)

    perm = np.random.default_rng(args.seed_split).permutation(n)
    base = np.sort(perm[:N_BASE])
    held = np.sort(perm[N_BASE:N_BASE + N_HELD])
    assert len(base) == N_BASE and len(held) == N_HELD and not (set(base) & set(held))
    print("items=%d dim=%d  split seed=%d  base=%d held=%d" %
          (n, X.shape[1], args.seed_split, len(base), len(held)))

    # 1) the frozen map: learned on BASE only, then never touched again
    C = kmeans(X[base], K, seed=SEED_KMEANS)
    a_base, mem_base = members(X, C, base)
    base_bucket = a_base.copy()

    # base queries: the fixture's value queries that live in BASE
    vq = [int(q) for q in d["value_queries"] if int(q) in set(base.tolist())]
    print("base value queries=%d (of %d)" % (len(vq), len(d["value_queries"])))

    gt_base = {q: gt_top10(X, base, q) for q in vq}
    print("frozen bucket size min/median/max = %d/%d/%d" % (
        min(len(m) for m in mem_base),
        int(np.median([len(m) for m in mem_base])),
        max(len(m) for m in mem_base)))

    # 2) park HELD: nearest frozen centroid, append. No new route is opened.
    a_held = np.argmax(X[held] @ C.T, axis=1)
    mem_park = [np.concatenate([m, held[a_held == c]]) if np.any(a_held == c)
                else m for c, m in enumerate(mem_base)]

    # verify churn rather than assume it: read each BASE item's bucket back out of the
    # post-park map and compare with the address it had before parking
    base_pos = np.full(n, -1, dtype=int)
    base_pos[base] = np.arange(len(base))
    post = np.full(len(base), -1, dtype=int)
    for c in range(K):
        sel = mem_park[c][np.isin(mem_park[c], base)]
        post[base_pos[sel]] = c
    assert (post >= 0).all(), "a BASE item went missing from the parked map"
    assert np.array_equal(assign(X[base], C), a_base), "frozen centroid table changed"
    churn_frozen = int(np.sum(post != base_bucket))

    # 3) recall before / after parking.  Two ground truths:
    #    base-only  = truth frozen at the moment the map was learned (routing damage only)
    #    live       = truth over every item now in the store (what a user would get)
    gt_live = {q: gt_top10(X, np.arange(n), q) for q in vq}

    # 4) control: retrain on BASE+HELD, the cost of NOT freezing
    C2 = kmeans(X, K, seed=SEED_KMEANS)
    a2 = assign(X, C2)
    _, mem_ctrl = members(X, C2, np.arange(n))
    ctrl_base = a2[base]
    churn_ctrl = int(np.sum(ctrl_base != base_bucket))
    churn_ctrl_matched = matched_churn(base_bucket, ctrl_base, K)

    # 5) held queries: would the frozen map's candidates contain the true top-10 at all?
    rng = np.random.default_rng(SEED_HELD_Q)
    hq = [int(h) for h in rng.choice(held, size=min(200, len(held)), replace=False)]
    gt_held = {q: gt_top10(X, np.arange(n), q) for q in hq}

    rows = []
    for b in (2, 4, 6):
        r_before = recall(X, vq, C, mem_base, b, gt_base)
        r_after_base = recall(X, vq, C, mem_park, b, gt_base)
        r_after_live = recall(X, vq, C, mem_park, b, gt_live)
        r_ctrl = recall(X, vq, C2, mem_ctrl, b, gt_live)
        scan = float(np.mean([len(np.concatenate([mem_park[c]
                    for c in np.argsort(-(X[q] @ C.T))[:b]])) for q in vq])) / n
        ctrl_scan = float(np.mean([len(np.concatenate([mem_ctrl[c]
                    for c in np.argsort(-(X[q] @ C2.T))[:b]])) for q in vq])) / n
        cov_any, cov_noself = [], []
        for q in hq:
            cand = set(np.concatenate([mem_park[c]
                        for c in np.argsort(-(X[q] @ C.T))[:b]]).tolist())
            truth = set(gt_held[q].tolist())
            cov_any.append(1.0 if cand & truth else 0.0)
            cov_noself.append(1.0 if cand & (truth - {q}) else 0.0)
        rows.append((b, np.mean(r_before), np.mean(r_after_base),
                     np.mean(r_after_live), np.mean(r_ctrl), scan,
                     np.mean(cov_any), np.mean(cov_noself), ctrl_scan,
                     boot_ci(r_after_live, r_ctrl), boot_ci(r_after_live, r_before),
                     boot_ci(r_after_base, r_before)))

    print()
    print("%-2s %-11s %-11s %-11s %-11s %-7s %-8s %-8s %s" %
          ("b", "frozen", "park(baseGT)", "park(liveGT)", "control", "scan%",
           "cov", "cov-self", "park-control [95% CI]"))
    for b, rb, ra, rl, rc, sc, ca, cn, csc, ci, ci2, ci3 in rows:
        print("%-2d %-11.4f %-11.4f %-11.4f %-11.4f %-7.1f %-8.3f %-8.3f "
              "%+.4f [%+.4f, %+.4f]  park-frozen %+.4f [%+.4f, %+.4f]  ctrl scan %.1f%%" %
              (b, rb, ra, rl, rc, 100 * sc, ca, cn, rl - rc, ci[0], ci[1],
               rl - rb, ci2[0], ci2[1], 100 * csc))
    print()
    print("crowding under a FIXED answer set (same truth, baseGT): park - frozen")
    for b, rb, ra, rl, rc, sc, ca, cn, csc, ci, ci2, ci3 in rows:
        print("%-2d %+.4f [%+.4f, %+.4f]" % (b, ra - rb, ci3[0], ci3[1]))

    print()
    print("address churn  frozen  %d/%d   control  %d/%d (raw labels)  %d/%d (best relabelling)" %
          (churn_frozen, len(base), churn_ctrl, len(base), churn_ctrl_matched, len(base)))

    per = np.array([int(np.sum(a_held == c)) for c in range(K)])
    print("held per frozen bucket  min/median/max = %d/%d/%d  empty buckets=%d/%d  "
          "landed in pre-existing bucket=%.4f" %
          (per.min(), int(np.median(per)), per.max(), int((per == 0).sum()), K,
           1.0))
    print("frozen bucket size after park  min/median/max = %d/%d/%d" % (
        min(len(m) for m in mem_park), int(np.median([len(m) for m in mem_park])),
        max(len(m) for m in mem_park)))


if __name__ == "__main__":
    main()
