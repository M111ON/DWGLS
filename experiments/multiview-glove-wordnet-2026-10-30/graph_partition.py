#!/usr/bin/env python3
"""graph_partition.py — does a genuinely graph-aware partition beat kmeans-on-smoothed?

The earlier result: a graph-smoothed kmeans view only lifted relation co-location from
0.396 to 0.407, because one round of neighbour averaging does not preserve graph
structure at cluster granularity. This tests the real thing:

  louvain   : community detection on the WordNet graph (structure only, no vectors)
  spectral  : normalised-adjacency eigenvectors + kmeans, K=64 (structure only)

Both ignore the GloVe vectors entirely, so if either beats 0.407 the relation view is
worth building; if neither does, relation queries need traversal, not a partition.

Co-location = fraction of a word's true WordNet neighbours that survive into the scanned
candidate set. It is the ceiling for any reranker placed after routing.

    python graph_partition.py [--maxb 6]
"""
import argparse
import os

import numpy as np
import scipy.sparse as sp
from scipy.sparse.linalg import eigsh

ROOT = os.path.dirname(os.path.dirname(os.path.dirname(os.path.abspath(__file__))))
NPZ = os.path.join(ROOT, "data", "glove_wordnet.npz")


def build_adj(n, edges):
    r = np.concatenate([edges[:, 0], edges[:, 1]])
    c = np.concatenate([edges[:, 1], edges[:, 0]])
    A = sp.coo_matrix((np.ones(len(r), dtype=np.float32), (r, c)), shape=(n, n)).tocsr()
    A.data[:] = 1.0
    return A


def centroids(X, assign, K):
    C = np.zeros((K, X.shape[1]), dtype=np.float32)
    for k in range(K):
        m = assign == k
        if m.any():
            C[k] = X[m].mean(axis=0)
    C /= np.maximum(np.linalg.norm(C, axis=1, keepdims=True), 1e-9)
    return C


def evaluate(name, X, assign, K, val_q, rel_q, nbr, gt_val, k=10, maxb=6):
    mem = [np.where(assign == c)[0] for c in range(K)]
    mem = [m for m in mem if len(m)]
    C = centroids(X, assign, K)[[c for c in range(K) if len(np.where(assign == c)[0])]]
    K = len(mem)
    print("\n%s — %d buckets, size min/mean/max = %d/%d/%d"
          % (name, K, min(len(m) for m in mem),
             int(np.mean([len(m) for m in mem])), max(len(m) for m in mem)))
    print("%-3s %-8s | %-9s | %-9s" % ("b", "scan%", "val recall", "rel coloc"))

    def cands(qv, b):
        top = np.argsort(-(qv @ C.T))[:b]
        return set(np.concatenate([mem[c] for c in top]).tolist())

    for b in range(1, maxb + 1):
        rv, rc = [], []
        scanned = 0
        for q in val_q:
            qv = X[q]
            cs = cands(qv, b)
            scanned += len(cs)
            a = np.fromiter(cs, dtype=np.int64, count=len(cs))
            s = X[a] @ qv
            top = set(a[np.argsort(-s)[:k]].tolist())
            rv.append(len(top & gt_val[q]) / k)
        for q in rel_q:
            cs = cands(X[q], b)
            truth = set(nbr[q])
            if truth:
                rc.append(len(cs & truth) / len(truth))
        n = float(len(val_q))
        print("%-3d %-8.1f | %-9.3f | %-9.3f"
              % (b, 100 * scanned / (n * len(X)), np.mean(rv), np.mean(rc)))


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--maxb", type=int, default=6)
    ap.add_argument("--K", type=int, default=64)
    args = ap.parse_args()

    d = np.load(NPZ, allow_pickle=True)
    X = d["X"].astype(np.float32)
    edges = d["edges"].astype(np.int64)
    n = len(X)
    nbr = [[] for _ in range(n)]
    for a, b in edges:
        nbr[a].append(int(b))
        nbr[b].append(int(a))
    val_q = [int(q) for q in d["value_queries"]]
    rel_q = [int(q) for q in d["relation_queries"]]
    gt_val = {q: set(np.argsort(-(X @ X[q]))[:10].tolist()) for q in val_q}
    print("items=%d edges=%d value_q=%d relation_q=%d" % (n, len(edges), len(val_q), len(rel_q)))

    A = build_adj(n, edges)

    # --- louvain (structure only)
    import networkx as nx
    G = nx.Graph()
    G.add_nodes_from(range(n))
    G.add_edges_from(map(tuple, edges.tolist()))
    comms = nx.community.louvain_communities(G, seed=7)
    comms = sorted(comms, key=len, reverse=True)
    assign_l = np.full(n, -1, dtype=np.int64)
    for i, c in enumerate(comms):
        for v in c:
            assign_l[v] = i
    print("louvain communities: %d (largest %d, singletons %d)"
          % (len(comms), len(comms[0]), int((assign_l < 0).sum())))
    assign_l[assign_l < 0] = len(comms) - 1
    evaluate("LOUVAIN", X, assign_l, assign_l.max() + 1, val_q, rel_q, nbr, gt_val, maxb=args.maxb)

    # --- spectral (structure only)
    deg = np.asarray(A.sum(axis=1)).ravel()
    dinv = 1.0 / np.sqrt(np.maximum(deg, 1e-9))
    D = sp.diags(dinv)
    M = (D @ A @ D).astype(np.float32)
    kk = min(args.K + 1, n - 2)
    vals, vecs = eigsh(M, k=kk, which="LA")
    emb = vecs[:, 1:args.K + 1]
    emb /= np.maximum(np.linalg.norm(emb, axis=1, keepdims=True), 1e-9)
    rng = np.random.default_rng(3)
    C = emb[rng.choice(n, size=args.K, replace=False)].copy()
    for _ in range(30):
        a = np.argmax(emb @ C.T, axis=1)
        newC = C.copy()
        for c in range(args.K):
            m = a == c
            if m.any():
                newC[c] = emb[m].mean(axis=0)
        newC /= np.maximum(np.linalg.norm(newC, axis=1, keepdims=True), 1e-9)
        if np.allclose(newC, C, atol=1e-6):
            C = newC
            break
        C = newC
    assign_s = np.argmax(emb @ C.T, axis=1)
    evaluate("SPECTRAL", X, assign_s, args.K, val_q, rel_q, nbr, gt_val, maxb=args.maxb)


if __name__ == "__main__":
    main()
