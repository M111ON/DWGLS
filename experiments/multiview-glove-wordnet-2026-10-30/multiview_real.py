#!/usr/bin/env python3
"""multiview_real.py — two GENUINELY different views on GloVe+WordNet.

The earlier test (index_eval/ann_eval_multiview.py) used two kmeans seeds over the same
vectors as its two "views". Two seeds of one clustering mostly agree, so the views could
never disagree and the relation result was an artifact of that, not of routing.

Here:
  view A (value)    : kmeans over the GloVe vectors
  view B (relation) : kmeans over graph-smoothed vectors — each node's vector averaged
                      with its WordNet neighbours, then renormalised. Same K, same
                      carrier, but the partition follows the graph instead of cosine.

Two different questions, because the two classes need two different metrics:

  value query    — routing must deliver the exact cosine top-k.
                   metric: recall@10 with cosine ranking.
  relation query — cosine can never rank a WordNet neighbour highly (measured overlap
                   between cosine top-10 and the neighbour set is 0.055). Ranking is the
                   graph's job; routing's job is only to put the neighbours in the
                   candidate set. metric: CO-LOCATION = fraction of true neighbours that
                   survive into the scanned candidates. That is the ceiling any
                   graph-aware reranker could reach.

    python multiview_real.py [--K 64] [--maxb 6]
"""
import argparse
import os

import numpy as np

ROOT = os.path.dirname(os.path.dirname(os.path.dirname(os.path.abspath(__file__))))
NPZ = os.path.join(ROOT, "data", "glove_wordnet.npz")


def kmeans(X, K, seed=7, iters=30):
    rng = np.random.default_rng(seed)
    C = X[rng.choice(X.shape[0], size=K, replace=False)].copy()
    for _ in range(iters):
        a = np.argmax(X @ C.T, axis=1)
        newC = C.copy()
        for k in range(K):
            m = a == k
            if m.any():
                newC[k] = X[m].mean(axis=0)
        newC /= np.maximum(np.linalg.norm(newC, axis=1, keepdims=True), 1e-9)
        if np.allclose(newC, C, atol=1e-6):
            C = newC
            break
        C = newC
    return C


def smoothed(X, edges):
    """One round of mean-aggregation over the relation graph, renormalised."""
    S = X.copy()
    cnt = np.ones(len(X), dtype=np.float32)
    for a, b in edges:
        S[a] += X[b]
        S[b] += X[a]
        cnt[a] += 1
        cnt[b] += 1
    S /= cnt[:, None]
    S /= np.maximum(np.linalg.norm(S, axis=1, keepdims=True), 1e-9)
    return S


def members_of(X, C, K):
    a = np.argmax(X @ C.T, axis=1)
    return [np.where(a == c)[0] for c in range(K)]


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--K", type=int, default=64)
    ap.add_argument("--maxb", type=int, default=6)
    args = ap.parse_args()
    k = 10

    d = np.load(NPZ, allow_pickle=True)
    X = d["X"].astype(np.float32)
    edges = d["edges"]
    nbr = [[] for _ in range(len(X))]
    for a, b in edges:
        nbr[a].append(int(b))
        nbr[b].append(int(a))
    val_q = [int(q) for q in d["value_queries"]]
    rel_q = [int(q) for q in d["relation_queries"]]
    print("items=%d dim=%d edges=%d value_q=%d relation_q=%d K=%d"
          % (len(X), X.shape[1], len(edges), len(val_q), len(rel_q), args.K))

    CA = kmeans(X, args.K, seed=7)
    CB = kmeans(smoothed(X, edges), args.K, seed=11)
    memA, memB = members_of(X, CA, args.K), members_of(X, CB, args.K)
    print("bucket size A min/mean/max = %d/%d/%d   B min/mean/max = %d/%d/%d" % (
        min(len(m) for m in memA), int(np.mean([len(m) for m in memA])), max(len(m) for m in memA),
        min(len(m) for m in memB), int(np.mean([len(m) for m in memB])), max(len(m) for m in memB)))

    gt_val = {q: set(np.argsort(-(X @ X[q]))[:k].tolist()) for q in val_q}

    def cands(qv, C, mem, b):
        top = np.argsort(-(qv @ C.T))[:b]
        return set(np.concatenate([mem[c] for c in top]).tolist())

    print()
    print("VALUE queries — recall@10 with cosine ranking (higher is better)")
    print("%-3s %-7s %-8s | %-7s %-7s %-7s" % ("b", "scanA%", "scanB%", "A", "B", "UNION"))
    for b in range(1, args.maxb + 1):
        rA, rB, rU = [], [], []
        sa = sb = 0
        for q in val_q:
            qv = X[q]
            cA = cands(qv, CA, memA, b)
            cB = cands(qv, CB, memB, b)
            sa += len(cA)
            sb += len(cB)
            for cand, out in ((cA, rA), (cB, rB), (cA | cB, rU)):
                a = np.fromiter(cand, dtype=np.int64, count=len(cand))
                s = X[a] @ qv
                top = set(a[np.argsort(-s)[:k]].tolist())
                out.append(len(top & gt_val[q]) / k)
        n = float(len(val_q))
        print("%-3d %-7.1f %-8.1f | %-7.3f %-7.3f %-7.3f" % (
            b, 100 * sa / (n * len(X)), 100 * sb / (n * len(X)),
            np.mean(rA), np.mean(rB), np.mean(rU)))

    print()
    print("RELATION queries — co-location of true WordNet neighbours in the scanned set")
    print("(ceiling for any graph reranker; cosine ranking itself cannot do this)")
    print("%-3s %-7s %-8s | %-7s %-7s %-7s" % ("b", "scanA%", "scanB%", "A", "B", "UNION"))
    for b in range(1, args.maxb + 1):
        cA_l, cB_l, cU_l = [], [], []
        sa = sb = 0
        for q in rel_q:
            qv = X[q]
            cA = cands(qv, CA, memA, b)
            cB = cands(qv, CB, memB, b)
            sa += len(cA)
            sb += len(cB)
            truth = set(nbr[q])
            if not truth:
                continue
            cA_l.append(len(cA & truth) / len(truth))
            cB_l.append(len(cB & truth) / len(truth))
            cU_l.append(len((cA | cB) & truth) / len(truth))
        n = float(len(rel_q))
        print("%-3d %-7.1f %-8.1f | %-7.3f %-7.3f %-7.3f" % (
            b, 100 * sa / (n * len(X)), 100 * sb / (n * len(X)),
            np.mean(cA_l), np.mean(cB_l), np.mean(cU_l)))


if __name__ == "__main__":
    main()
