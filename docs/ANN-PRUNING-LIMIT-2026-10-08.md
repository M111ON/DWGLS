# ANN Anchor Routing — Pruning Limit (measured 2026-10-08)

## Question

The anchor route is meant to be a pruning accelerator: walk a few centroid
buckets, score only their members instead of the whole index. Measured on the
live 2881-entry index it prunes nothing at exact recall. Is that a tuning
problem or a structural one?

## Answer (technical)

Structural. The route's selection rule is **coverage**, not partition:

    candidate  <=>  (entry.anchor in walked) OR (entry.anchor2 in walked)

Every entry carries TWO anchors (2881/2881 have anchor2). Bucket load by that
rule is 341–860 entries of 2881, against an ideal share of 360. So one walked
bucket already covers ~24–30% of the index, and two cover up to 61% — the
accelerator is paying full index cost while reporting a short bucket list.

### Measured curves (8 queries, ground truth = full cosine, local Python)

Recall vs fraction of index actually scored:

| topb | recall@8 | %index scored |
|------|----------|---------------|
| 1 | 0.578 | 23.8% |
| 2 | 0.703 | 38.3% |
| 4 | 0.875 | 61.9% |
| 6 | 0.984 | 86.3% |
| 7 | 1.000 | 100.0% |

Exact recall costs 100% of the index. There is no operating point that is both
exact and cheap.

### Raising K does not help — and it is not a centroid-quality problem

k-means on the real 384-d vectors with k-means++ seeding, 10 restarts,
100 iters (well beyond the 25 the server uses). `fit` = mean cosine to own
centroid, confirming the clusters are genuinely tight:

| K | fit | topb for recall@8=1.0 | %index |
|---|---|----------------------|--------|
| 8 | 0.934 | 8 | 100% |
| 32 | 0.938 | 23 | 71.9% |
| 64 | 0.941 | 50 | 78.1% |
| 128 | 0.944 | 77 | 60.2% |

Better-fitting centroids (0.934 -> 0.944) move the exact-recall cost only from
100% to ~60%. Because each query's true top-8 is spread across many clusters
(a query's nearest-neighbour set is not cluster-local), the union of walked
buckets must stay large. This is a property of the data geometry, not of the
centroid fit.

### Dropping the anchor2 clause helps a little

Primary-only selection (`anchor in walked`, ignore anchor2) makes the buckets a
true partition:

| topk | rule | topb for recall=1.0 | %index scored |
|------|------|--------------------|---------------|
| 3 | OR | 7 | 100.0% |
| 3 | primary only | 7 | 86.7% |
| 8 | OR | 7 | 100.0% |
| 8 | primary only | 8 | 100.0% |

So it buys ~13 points at topk=3 and nothing at topk=8. Not sufficient alone.

## What this means

The 8-bucket route cannot be both exact and pruning on this corpus. Useful
positions are:

1. **Exact, no prune** (current shipped behaviour, headroom=4). Correct, fast
   only because the index is cached in RAM. Scales badly: cost is O(N).
2. **Prune, approximate** (topb < 7). e.g. topb=4 scores 62% for recall 0.875.
   Legitimate for retrieval-if-you-can, not for exact search.
3. **Prune exactly by other means** — the coverage rule is the blocker; a
   tighter candidate rule (partition + bounded expansion, or two-stage:
   bucket route then refine) is what would actually pay.

Do not "fix" this by raising K: measured, it does not buy exact pruning.

## Reproduce

Tracked scripts in `bench/` (run from the repo root; the embed sidecar must be
up on 127.0.0.1:18095 and the serve on 8096):
- `bench/ann_route_sweep.py` — recall/%scored vs topb, via the live server
- `bench/ann_route_rule.py` — coverage rule vs primary-only
- `bench/ann_route_kmeans.py` — k-means++ sweep K=8..128 with fit quality
  (needs numpy; independent of the server for clustering, uses the embed
  sidecar only for the 8 queries)

All compute ground truth locally (plain cosine over `build/kv_index.jsonl`),
independent of the server's own scoring.

Scratch copies of the intermediate harnesses (`isolate2.py`, `sweep_recall.py`,
`kmeans_prune.py`, `prune_hypotheses.py`, `isolate_headroom.py`) lived in
`I:/Hermes/cache/scratch/` and are pruned after 24h; the three above are the
durable ones.