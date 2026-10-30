# Two-hop expansion — does a second graph hop pay for itself? (2026-10-30)

`expansion2.py` extends `expansion.py` (which is unchanged) with one extra hop:

```
anchors = top-m cosine neighbours of q (query excluded)
hop1    = ∪ neighbours(anchor)   \ {q}
hop2    = ∪ neighbours(b) for b in hop1
cand    = {q} ∪ hop1 ∪ hop2
```

Hop-1 rows reproduce `RESULTS-2026-10-30.md` exactly (0.276 / 0.319 / 0.388 / 0.479),
so the harness is comparable. 20,000 words, 80,849 edges, max degree 517, 500 relation
and 500 value queries, full run 69 s.

## Relation queries — co-location of true WordNet neighbours

| m | hop | scan % | cost × | co-location | mean cand | p95 | max |
|---|---|---|---|---|---|---|---|
| 5 | 1 | 0.19 | 1.00 | 0.276 | 38 | 115 | 422 |
| 5 | **2** | 2.65 | **14.05** | **0.448** | 529 | 1,599 | 3,467 |
| 10 | 1 | 0.38 | 1.00 | 0.319 | 76 | 215 | 698 |
| 10 | **2** | 4.61 | **12.12** | **0.518** | 923 | 2,401 | 4,226 |
| 20 | 1 | 0.71 | 1.00 | 0.388 | 143 | 403 | 889 |
| 20 | **2** | 7.30 | **10.25** | **0.623** | 1,460 | 3,576 | 4,873 |
| 50 | 1 | 1.63 | 1.00 | 0.479 | 326 | 869 | 1,596 |
| 50 | **2** | 12.57 | **7.73** | **0.728** | 2,515 | 4,949 | **6,177** |

Hop 2 does raise the ceiling (0.479 → 0.728) — but it costs 7.7–14× the candidates.
So the only question that matters is whether hop 2 beats hop 1 at the **same** scan %.
It does not:

| scan % | 2-hop | 1-hop at that cost |
|---|---|---|
| ~2.6 | m=5 → 0.448 | m=100 → **0.568** (2.94%) |
| ~4.6 | m=10 → 0.518 | m=150 → **0.616** (4.08%) |
| ~7.3 | m=20 → 0.623 | m=200 → **0.654** (5.12%) |
| ~12.6 | m=50 → 0.728 | m=600 → **0.756** (11.37%) |

1-hop sweep used for the right column:

| m | scan % | co-location | val recall@10 |
|---|---|---|---|
| 75 | 2.32 | 0.522 | 0.208 |
| 100 | 2.94 | 0.568 | 0.228 |
| 150 | 4.08 | 0.616 | 0.256 |
| 200 | 5.12 | 0.654 | 0.275 |
| 400 | 8.59 | 0.722 | 0.332 |
| 600 | 11.37 | **0.756** | 0.358 |

## Value queries — recall@10, cosine ranking inside the candidate set

| m | hop | scan % | recall@10 |
|---|---|---|---|
| 5 | 1 | 0.19 | 0.053 |
| 5 | 2 | 2.65 | 0.375 |
| 10 | 1 | 0.38 | 0.085 |
| 10 | 2 | 4.61 | **0.602** |
| 20 | 1 | 0.71 | 0.126 |
| 20 | 2 | 7.30 | 0.602 |
| 50 | 1 | 1.63 | 0.183 |
| 50 | 2 | 12.57 | 0.602 |

At ~4.6% scan, 2-hop gives 0.602 and 1-hop gives 0.256. The 0.602 is **flat** across
m=10/20/50 because it is a saturation, not a coincidence: of the 1,492 cosine top-10
neighbours missed by the m=10 2-hop ball (out of 5,000), **1,474 (99%) are isolated
nodes** with no edges at all. They are unreachable at any hop count, so extra hops buy
nothing. Whole-corpus ceiling for the same metric is 0.90 (the query itself fills one
gt slot), so 0.602 is well short of 0.90 but no expansion can close more than the
isolated-node share allows.

## Cost and the hub tail

The mean hides the real cost. Candidate-set size at m=50, hop 2: mean 2,515,
**p95 4,949 (25% of the corpus), max 6,177 (31% of the corpus)** — one query touches a
third of the index. Driven by the hub of degree 517: per-query cost is *not* bounded by
`m × mean_degree (8.08)`, it is bounded by `m × max_degree` on the queries that land on
hubs. Any production use of 2-hop needs a degree cap or a visited budget, or the worst
case is a full scan.

Cost ratio (2-hop mean / 1-hop mean): m=5 **14.05×**, m=10 **12.12×**, m=20 **10.25×**,
m=50 **7.73×**. The ratio falls with m because hop 1 is already eating the ball.

## Verdict

**For relation queries the second hop is not worth it; at equal scan it always loses.**
Read against cost, 1-hop m=100 (0.568 @ 2.94%) beats 2-hop m=5 (0.448 @ 2.65%),
1-hop m=150 (0.616) beats 2-hop m=10 (0.518), and 1-hop m=600 (0.756 @ 11.37%) beats
2-hop m=50 (0.728 @ 12.57%) while scanning less and with a max set of ~2.5k instead of
6,177. Two ways to spend a candidate budget, and the graph hop spends it worse: hop 1
from good anchors lands on words that are adjacent to the query's vector neighbourhood,
hop 2 lands on words two edges away that are proportionally less often a true neighbour.
Co-location grows roughly with the log of candidate count, cost grows with m × degree —
better to raise m than to walk further. If you do use hop 2, m=5 is the only defensible
setting (14× cost for +0.17 co-location — still dominated by 1-hop m=100 at lower cost).
The one place hop 2 clearly wins is **value** queries: 0.602 @ 4.6% versus 0.256 for
1-hop @ 4.1%, because a 2-hop ball around vector-local anchors stays vector-local and
the ranking is cosine anyway; it saturates only because the missing items are isolated
nodes. So the split the corpus wants is 1-hop + large m for relation, 2-hop for value —
and 0.728–0.756 remains the realistic relation ceiling at ~12% scan, still far below
the partition route (0.551 @ 20%) being worth reconsidering.

## Commands run

```
cd I:\DWGLS-native-fs\experiments\multiview-glove-wordnet-2026-10-30
python expansion2.py          # 69 s, the numbers above
```

`expansion.py` was not re-run: its table is quoted from `RESULTS-2026-10-30.md`, and the
hop-1 rows here reproduce it to three decimals through a separate code path. Nothing was
committed. Note the working tree already carried uncommitted edits to `expansion.py`,
`build_fixture.py` and `multiview_real.py` (an `--npz` argument) when this run started;
they are not mine and the hop-1 definition used here is the one those files implement.
No run was impractically slow at these m; m=50 hop 2 is the heaviest
(~2.5k candidates/query, 500 queries, plus 500 value rankings).
