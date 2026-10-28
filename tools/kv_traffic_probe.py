#!/usr/bin/env python3
"""kv_traffic_probe.py — does a real-pool row set actually retrieve its neighbours?

Ground truth here does NOT come from the vector space. It comes from the corpus:
the prompts were lifted from N consecutive messages of one app (pool file order),
so message t's neighbours are t-1 and t+1. That is a fact of the source, not of
the embedding, and it cannot be gamed by the model that produced the vectors.

The control is a random ranking over the same rows, so the reported number is
"better than chance by how much", never a bare percentage.

Self-exclusion is mandatory (#7697): with the query row left in the index,
cosine(q,q)=1.0 makes top-1 the query itself and every recall figure becomes a
tautology. The probe verifies that trap first, then measures with it removed.

Usage:
    python kv_traffic_probe.py --rows ann_rows.jsonl --sidecar kv_frames.jsonl [--k 3]
"""
from __future__ import annotations

import argparse
import json
import math
import random
import sys


def load(path):
    out = []
    with open(path, "rb") as f:
        for raw in f:
            line = raw.decode("utf-8", "replace").rstrip("\r\n ")
            if line.endswith("}"):
                out.append(json.loads(line))
    return out


def dot(a, b):
    return sum(x * y for x, y in zip(a, b))


def rank(q, rows, skip_sid):
    scored = [(dot(q, r["vec"]), r["sid"]) for r in rows if r["sid"] != skip_sid]
    scored.sort(reverse=True)
    return [s for _, s in scored]


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--rows", required=True)
    ap.add_argument("--sidecar", required=True)
    ap.add_argument("--k", type=int, default=3)
    ap.add_argument("--seed", type=int, default=7)
    args = ap.parse_args()

    rows = load(args.rows)
    plans = {p["sid"]: p for p in load(args.sidecar)}
    by_turn = {p["turn"]: p["sid"] for p in plans.values()}
    turns = sorted(by_turn)
    if len(rows) < 3:
        print("need at least 3 rows", file=sys.stderr)
        return 1

    # 1) the tautology, proven present before it is removed
    self_top1 = 0
    for r in rows:
        best = max(rows, key=lambda x: dot(r["vec"], x["vec"]))
        if best["sid"] == r["sid"]:
            self_top1 += 1

    # 2) adjacency recall with self excluded, plus a random control
    rnd = random.Random(args.seed)
    hit = {1: 0, args.k: 0}
    ctl = {1: 0, args.k: 0}
    chance = {1: 0.0, args.k: 0.0}
    queried = 0
    for t in turns:
        nbrs = {by_turn.get(t - 1), by_turn.get(t + 1)} - {None}
        if not nbrs:
            continue
        sid = by_turn[t]
        q = next(r["vec"] for r in rows if r["sid"] == sid)
        order = rank(q, rows, sid)
        queried += 1
        for k in hit:
            hit[k] += 1 if (set(order[:k]) & nbrs) else 0
            pool = [s for s in order]
            pick = set(rnd.sample(pool, min(k, len(pool))))
            ctl[k] += 1 if (pick & nbrs) else 0
            # exact chance: k draws without replacement from len(pool)
            n, m = len(pool), len(nbrs)
            chance[k] += 1.0 - (math.comb(n - m, k) / math.comb(n, k)
                                if n >= k and n - m >= k else 0.0)

    covered = sum(1 for p in plans.values() if p.get("anchor_turn") is not None)
    print("kv_traffic_probe: %d row(s), %d plan(s), %d queried" % (len(rows), len(plans), queried))
    print("  self top-1 (tautology trap)  %d/%d present before exclusion" % (self_top1, len(rows)))
    for k in sorted(hit):
        print("  adjacency recall@%d          %.4f  (random control %.4f, chance %.4f)"
              % (k, hit[k] / queried, ctl[k] / queried, chance[k] / queried))
    print("  resume plans                 %d/%d actionable" % (covered, len(plans)))
    return 0


if __name__ == "__main__":
    sys.exit(main())
