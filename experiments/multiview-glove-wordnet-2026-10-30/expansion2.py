#!/usr/bin/env python3
"""expansion2.py — does a SECOND graph hop improve relation co-location?

expansion.py measured one hop: anchors = top-m cosine neighbours of the query,
candidates = {q} ∪ neighbours(anchors). Best was 0.479 co-location at m=50,
1.63% scan. RESULTS-2026-10-30.md leaves open whether hop 2 closes the gap.

This keeps hop 1 exactly as expansion.py defines it (union of the anchors'
neighbour sets, query removed) and adds hop 2 = neighbours of hop 1:

    hop1 = ∪ nbr[anchor]  \\ {q}
    hop2 = ∪ nbr[b] for b in hop1
    cand = {q} ∪ hop1 ∪ hop2

Reported per m: hop-1 baseline, hop-2, scan %, relation co-location (fraction of
the query's true WordNet neighbours inside the candidate set), value recall@10
(cosine ranking restricted to the candidate set) and the candidate-set size
distribution (mean / p95 / max — the graph has hubs, so the mean hides the tail).

    python expansion2.py
"""
import os

import numpy as np

ROOT = os.path.dirname(os.path.dirname(os.path.dirname(os.path.abspath(__file__))))
NPZ = os.path.join(ROOT, "data", "glove_wordnet.npz")
K = 10
MS = (5, 10, 20, 50)


def main():
    d = np.load(NPZ, allow_pickle=True)
    X = d["X"].astype(np.float32)
    edges = d["edges"]
    n = len(X)
    nbr = [set() for _ in range(n)]
    for a, b in edges:
        nbr[a].add(int(b))
        nbr[b].add(int(a))
    nbr = [np.fromiter(s, dtype=np.int64, count=len(s)) if s else np.empty(0, np.int64)
           for s in nbr]
    rel_q = [int(q) for q in d["relation_queries"]]
    val_q = [int(q) for q in d["value_queries"]]
    gt_val = {q: set(np.argsort(-(X @ X[q]))[:K].tolist()) for q in val_q}
    print("items=%d edges=%d relation_q=%d value_q=%d max_degree=%d"
          % (n, len(edges), len(rel_q), len(val_q), max(len(a) for a in nbr)))
    print()

    def hops(q, m):
        sims = X @ X[q]
        sims[q] = -np.inf
        anchors = np.argpartition(-sims, m)[:m]
        hop1 = set()
        for a in anchors:
            hop1.update(nbr[a].tolist())
        hop1.discard(q)
        hop2 = set()
        for b in hop1:
            hop2.update(nbr[b].tolist())
        return hop1, hop2

    def vec(q, s):
        a = np.fromiter(s, dtype=np.int64, count=len(s))
        return X[a] @ X[q]

    hdr = "%-3s %-5s %-8s %-8s %-13s %-13s %-8s %-8s %-8s"
    print(hdr % ("m", "hop", "scan%", "cost x", "rel coloc", "val recall@10",
                 "mean", "p95", "max"))
    rows = {}
    for m in MS:
        for hop in (1, 2):
            rc, rv, sizes = [], [], []
            for q in rel_q:
                h1, h2 = hops(q, m)
                cand = set(h1) if hop == 1 else (set(h1) | h2 | {q})
                sizes.append(len(cand))
                truth = set(nbr[q].tolist())
                if truth:
                    rc.append(len(cand & truth) / len(truth))
            for q in val_q:
                h1, h2 = hops(q, m)
                cand = set(h1) if hop == 1 else (set(h1) | h2 | {q})
                cand.discard(q)  # match expansion.py: the query is never a candidate
                s = vec(q, cand)
                a = np.fromiter(cand, dtype=np.int64, count=len(cand))
                top = set(a[np.argsort(-s)[:K]].tolist())
                rv.append(len(top & gt_val[q]) / K)
            sizes = np.array(sizes)
            base = rows.get((m, 1), None)
            cost = sizes.mean() / base[0] if base else 1.0
            rows[(m, hop)] = (sizes.mean(), np.percentile(sizes, 95), sizes.max(),
                              float(np.mean(rc)), float(np.mean(rv)))
            mean, p95, mx, r, v = rows[(m, hop)]
            print("%-3d %-5d %-8.2f %-8.2f %-13.3f %-13.3f %-8.1f %-8.0f %-8d"
                  % (m, hop, 100.0 * mean / n, cost, r, v, mean, p95, mx))
        print()

    print("cost ratio (2-hop mean size / 1-hop mean size):")
    for m in MS:
        a = rows[(m, 1)][0]
        b = rows[(m, 2)][0]
        print("  m=%-3d %.2fx   (%.0f -> %.0f candidates, max %.0f)"
              % (m, b / a, a, b, rows[(m, 2)][2]))
    print()

    # Cost-matched control: 1 hop at the scan levels 2 hops lands on.
    print("cost-matched 1-hop sweep (is hop 2 beating hop 1 at the same scan%?):")
    print("%-6s %-8s %-13s %-13s" % ("m", "scan%", "rel coloc", "val recall@10"))
    for m in (75, 100, 150, 200, 400, 600):
        rc, rv, sizes = [], [], []
        for q in rel_q:
            h1, h2 = hops(q, m)
            sizes.append(len(h1))
            truth = set(nbr[q].tolist())
            if truth:
                rc.append(len(h1 & truth) / len(truth))
        for q in val_q:
            h1, h2 = hops(q, m)
            s = vec(q, h1)
            a = np.fromiter(h1, dtype=np.int64, count=len(h1))
            top = set(a[np.argsort(-s)[:K]].tolist())
            rv.append(len(top & gt_val[q]) / K)
        print("%-6d %-8.2f %-13.3f %-13.3f"
              % (m, 100.0 * np.mean(sizes) / n, np.mean(rc), np.mean(rv)))
    print()

    # Why value recall stops at ~0.6 with 2 hops, over all 500 value queries:
    # the missed cosine neighbours are nodes the graph cannot reach at all.
    iso = 0
    missed = 0
    for q in val_q:
        h1, h2 = hops(q, 10)
        ball = h1 | h2
        gone = gt_val[q] - ball - {q}
        missed += len(gone)
        iso += sum(len(nbr[t]) == 0 for t in gone)
    print("value queries: %d of %d cosine top-10 neighbours absent from the m=10 "
          "2-hop ball; %d of them (%.0f%%) are isolated nodes"
          % (missed, K * len(val_q), iso, 100.0 * iso / missed))


if __name__ == "__main__":
    main()
