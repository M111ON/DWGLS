#!/usr/bin/env python3
"""expansion.py — partition vs expansion for relation queries.

graph_partition.py showed a purely structural partition (louvain / spectral) is WORSE at
relation co-location than the plain vector partition (0.430 vs 0.532 at ~20% scan).
So the relation view is not a partition question. This tests the other mechanism:

  find the query's top-m cosine neighbours, then take ONE graph hop from them.

Candidates = {q} ∪ anchors ∪ neighbours(anchors). Scan cost is tiny (~m*degree items)
and does not grow with corpus size, unlike a partition scan.

    python expansion.py [--npz data/glove_wordnet.npz]
"""
import argparse
import os

import numpy as np

ROOT = os.path.dirname(os.path.dirname(os.path.dirname(os.path.abspath(__file__))))
NPZ = os.path.join(ROOT, "data", "glove_wordnet.npz")
K = 10


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--npz", default=NPZ, help="fixture to score (default: the 50d one)")
    args = ap.parse_args()
    d = np.load(args.npz, allow_pickle=True)
    X = d["X"].astype(np.float32)
    edges = d["edges"]
    n = len(X)
    nbr = [set() for _ in range(n)]
    for a, b in edges:
        nbr[a].add(int(b))
        nbr[b].add(int(a))
    rel_q = [int(q) for q in d["relation_queries"]]
    val_q = [int(q) for q in d["value_queries"]]
    gt_val = {q: set(np.argsort(-(X @ X[q]))[:K].tolist()) for q in val_q}
    print("items=%d edges=%d relation_q=%d value_q=%d" % (n, len(edges), len(rel_q), len(val_q)))
    print()
    print("%-4s %-8s | %-11s | %-13s" % ("m", "scan%", "rel coloc", "val recall@10"))

    for m in (1, 2, 3, 5, 10, 20, 50):
        rc, rv, scanned = [], [], 0
        for q in rel_q:
            sims = X @ X[q]
            sims[q] = -np.inf
            anchors = np.argsort(-sims)[:m]
            cand = {q}
            for a in anchors:
                cand |= nbr[a]
            cand.discard(q)
            scanned += len(cand)
            truth = nbr[q]
            if truth:
                rc.append(len(cand & truth) / len(truth))
        for q in val_q:
            sims = X @ X[q]
            sims[q] = -np.inf
            anchors = np.argsort(-sims)[:m]
            cand = {q}
            for a in anchors:
                cand |= nbr[a]
            cand.discard(q)
            a = np.fromiter(cand, dtype=np.int64, count=len(cand))
            s = X[a] @ X[q]
            top = set(a[np.argsort(-s)[:K]].tolist())
            rv.append(len(top & gt_val[q]) / K)
        print("%-4d %-8.2f | %-11.3f | %-13.3f"
              % (m, 100.0 * scanned / (len(rel_q) * n), np.mean(rc), np.mean(rv)))


if __name__ == "__main__":
    main()
