# Freeze-and-park — can a frozen map absorb new data without moving an address?

2026-10-30 · `I:\DWGLS-native-fs\experiments\multiview-glove-wordnet-2026-10-30`
Script: `freeze_park.py` (new; nothing else was touched)

## Question

Owner's rule: build the map once from clean generic data, freeze it, pre-open the
routes, let later data land on the existing routes. The sibling workstream measured it
on memcore (`I:\tools\ann\experiments\container-2026-10-30\REPORT.md`):

```
frozen k-means   recall 0.7735   scan 6.2%     0/16000 address moved
retrained        recall 0.7750   scan 6.2%     15845/16000 moved
```

This run repeats it on the GloVe+WordNet fixture.

## Setup

```
corpus          data/glove_wordnet.npz   20,000 items x 50 dim, unit norm, cosine
split seed      20261030  (numpy default_rng permutation)
BASE            16,000      (map learned here, then frozen)
HELD             4,000      (parked later, never used to build the map)
K               64 centroids, kmeans seed 7 (the seed multiview_real.py uses)
k               10
base queries     399 of the fixture's 500 value_queries (the ones whose item is in BASE)
held queries     200, drawn from HELD with seed 11
```

The map is `argmax(X @ C.T)` on the frozen centroid matrix — identical to the value view
in `multiview_real.py`. Parking = assign each HELD item to its nearest frozen centroid
and append. No centroid is recomputed, no new bucket is opened.

Ground truth is brute-force cosine top-10, and it is reported two ways:

- **liveGT** — truth over every item now in the store (the sibling report's convention,
  and what a user actually gets). Used for the headline table.
- **baseGT** — truth frozen at the moment the map was learned. Isolates routing damage
  from new data legitimately outranking old answers.

## Result

| b | frozen recall | after-park recall | Δrecall | control recall | control churn |
|---|---|---|---|---|---|
| 2 | 0.7000 | 0.7043 | **+0.0043** | 0.7115 | 15215/16000 |
| 4 | 0.8195 | 0.8185 | **−0.0010** | 0.8346 | 15215/16000 |
| 6 | 0.8847 | 0.8850 | **+0.0003** | 0.8922 | 15215/16000 |

Control churn is b-independent (the partition does not depend on the probe count), so
the same number is repeated in every row. Paired bootstrap over the 399 queries,
4000 resamples, 95% CI of the mean difference:

```
b   after-park - control            after-park - frozen
2   -0.0073 [-0.0271, +0.0130]      +0.0043 [-0.0033, +0.0120]
4   -0.0160 [-0.0316, +0.0000]      -0.0010 [-0.0083, +0.0065]
6   -0.0073 [-0.0193, +0.0050]      +0.0003 [-0.0060, +0.0068]
```

Not separated from zero at b=2 and b=6. At b=4 the interval ends on zero
(`-0.0000`), i.e. borderline — the retrained map is ahead by 1.6 points and that is
the only place the gap is even close to real.

Scan fraction (mean candidates / 20,000) is matched, so the comparison is at equal cost:

```
b   parked map   retrained map
2   3.5%         3.6%
4   7.0%         7.1%
6   10.5%        10.7%
```

## Address churn

```
frozen    0 / 16000   (0.0%)     verified, not assumed
control   15215 / 16000 (95.1%)  raw cluster labels
control    8827 / 16000 (55.2%)  after the best relabelling of the control's buckets
```

The frozen 0 is checked by construction-independent means: after parking, every BASE
item's bucket is read back out of the post-park member lists and compared to the bucket
it had before (plus an assert that the centroid matrix is bit-identical), so a bug that
silently re-assigned anyone would surface as a non-zero count. The script asserts no
BASE item is missing from the parked map.

The 55.2% relabel-invariant number is the honest physical cost of retraining: even if
you renamed every new bucket to line up with the old ones (greedy max-overlap matching,
a lower bound — not an optimal assignment), more than half the items would still sit in
a different cell. The raw 95.1% is what a stored address experiences if bucket identity
is taken literally, which is how a pointer behaves.

## Coverage — can the frozen map absorb HELD at all?

```
held items landing in a pre-existing bucket   1.0000  (4000/4000, no new route opened)
empty frozen buckets                          0 / 64
held items per frozen bucket  min/median/max  31 / 59 / 119
frozen bucket size after park min/median/max  148 / 284 / 629   (was 117 / 228 / 510)
```

HELD queries (200, drawn from the 4,000 held items) — does the routed candidate set
contain *any* of the query's true cosine top-10? Coverage, not recall:

| b | any true neighbour in candidates | excluding the query itself |
|---|---|---|
| 2 | 1.000 | 0.990 |
| 4 | 1.000 | 1.000 |
| 6 | 1.000 | 1.000 |

Two of the 200 held queries have no answer outside themselves at b=2 (their top-10 is
entirely in buckets the query does not probe); one more probe fixes both. So 0% of held
queries are unanswerable at the operating point b>=4, 1.0% at b=2.

## Crowding, under a fixed answer set

Same truth (`baseGT`), before vs after parking:

```
b   park - frozen        95% CI
2   -0.0461              [-0.0561, -0.0363]
4   -0.0815              [-0.0935, -0.0704]
6   -0.1063              [-0.1188, -0.0947]
```

This is real and it is not a freeze defect. Recall@10 is a ten-slot competition: when
25% more items enter the same buckets, some of them outrank the old true neighbours and
push them out. The retrained map pays the same tax — which is why `liveGT` recall barely
moves and the frozen-vs-control gap stays at 0.7–1.6 points instead of 10.

## Verdict

Freezing costs nothing that can be measured. Parking 4,000 new items (+25%) into the
frozen map moves recall by −0.001 to +0.004 at b=2/4/6, all three intervals containing
zero; the retrained control is 0.7–1.6 points ahead, separated only at the edge of
resolution at b=4 and not separated at b=2 or b=6, at the same scan fraction. The price
of *not* freezing is 15,215 of 16,000 addresses invalidated — 55% of them still moved
after the most generous relabelling. A ≤0.016 recall difference does not buy 15,215
broken pointers, so the rule holds on this corpus: the map can be built once against
clean generic data and data can arrive afterwards without touching an address. The
observable cost of arrival is bucket growth (max bucket 510 → 629, scan 3.5% → 10.5%
at b=6) — the promotion trigger, not a recall problem.

## Limits

- One corpus, one split seed, one kmeans seed, K=64, no promotion level. Bucket growth
  is measured but not acted on; the sibling report's phase C covers split-on-full.
- GloVe 50-d is far less anisotropic than the e5 embeddings in the sibling run, which
  is why raw label churn here is 95% rather than 99% — do not read the two as the same
  partition stability.
- `baseGT` rows describe competition, not user-visible recall; `liveGT` rows are the
  user-visible number.
- Coverage uses the fixture's convention that the query item itself is part of its own
  cosine top-10; the "excluding the query" column removes that.

## Reproduce

```
cd I:\DWGLS-native-fs\experiments\multiview-glove-wordnet-2026-10-30
python freeze_park.py
```

Run twice on 2026-10-30, output byte-identical (`fc run1.txt run2.txt` → no
differences), 8.8 s wall on this machine. Defaults: `--K 64 --seed-split 20261030`.
