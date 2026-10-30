# GloVe dimension sweep — does a bigger vector help routing? (2026-10-30)

Follow-up to `RESULTS-2026-10-30.md` (all of whose numbers are the 50d run). Same corpus, same
graph, same queries — only the GloVe dimension changes.

## Fixtures

`build_fixture.py` gained `--dim` (default 50). 50d keeps writing `data/glove_wordnet.npz`;
the others write `data/glove_wordnet_{dim}d.npz`. Vectors come from
`data/glove.6B.{dim}d.txt`, unzipped from `data/glove.6B.zip` on first use.

The four fixtures are **provably the same corpus**, not just similar — word set, edges and
query sets are byte-identical across dimensions:

```
dim  items  words_md5  edges  uniq  edge_md5  vq   rq
 50  20000  f0fb3875   80849  80849 0e10eb05  500  500
100  20000  f0fb3875   80849  80849 0e10eb05  500  500
200  20000  f0fb3875   80849  80849 0e10eb05  500  500
300  20000  f0fb3875   80849  80849 0e10eb05  500  500
```

`words_md5` = md5 of the ordered 20,000-word array; `edge_md5` = md5 of the edge set sorted by
endpoint (build order varies because it comes from a Python set). Same build also reported
`mean degree 8.08, isolated 6195` for all four. So every difference below is the dimension.

One measured side effect of dimension, from the build step:

| dim | overlap(cosine top-10, WordNet neighbours) |
|---|---|
| 50 | 0.055 |
| 100 | 0.063 |
| 200 | 0.069 |
| 300 | 0.069 |

Higher-dimensional GloVe puts a word's WordNet neighbours slightly closer in cosine. Small,
but it is the thing that later shows up in expansion.

## Value queries — recall@10, view A vs UNION, at matched b

Scan cost is not exactly equal because bucket balance drifts with dimension (view A max bucket
grows 678 → 779 and min shrinks 144 → 119 from 50d to 300d, so the same `b` scans *more* at
higher dim). That biases this table **in favour of** the higher dimensions, and they still lose.

| b | dim | scanA% | A | UNION | UNION scan% ≤ |
|---|---|---|---|---|---|
| 2 | 50 | 3.6 | **0.706** | **0.831** | 7.7 |
| 2 | 100 | 3.7 | 0.710 | 0.833 | 8.7 |
| 2 | 200 | 3.8 | 0.680 | 0.799 | 8.8 |
| 2 | 300 | 4.1 | 0.645 | 0.787 | 10.3 |
| 4 | 50 | 7.2 | **0.839** | **0.918** | 15.1 |
| 4 | 100 | 7.3 | 0.842 | **0.925** | 16.9 |
| 4 | 200 | 7.7 | 0.798 | 0.891 | 17.4 |
| 4 | 300 | 8.3 | 0.773 | 0.890 | 20.4 |
| 6 | 50 | 10.8 | **0.898** | **0.954** | 22.4 |
| 6 | 100 | 11.0 | 0.897 | 0.951 | 24.4 |
| 6 | 200 | 11.5 | 0.859 | 0.928 | 25.3 |
| 6 | 300 | 12.5 | 0.832 | 0.930 | 29.6 |
| 12 | 50 | 21.1 | **0.959** | **0.985** | 43.7 |
| 12 | 100 | 21.4 | 0.955 | 0.983 | 45.4 |
| 12 | 200 | 22.1 | 0.931 | 0.968 | 47.2 |
| 12 | 300 | 24.3 | 0.920 | 0.971 | 53.8 |

(UNION scan% is `scanA% + scanB%`, an upper bound on `|cA ∪ cB|`.)

Value recall is **flat from 50d to 100d and falls steadily after that**: at b=2, 0.706 → 0.645
(−0.06) while scanning more; at b=6, 0.898 → 0.832 (−0.07) at 1.7 points more scan. To reach the
same ~0.93, 300d needs b=13–14 (26–28% scan) where 50d needs b=9 (16.0%) — 1.6–1.7× the cost to
land no higher.

## Relation queries — co-location, view A / view B / expansion

| b | dim | scanA% | scanB% | A | B | UNION |
|---|---|---|---|---|---|---|
| 1 | 50 | 1.7 | 1.9 | 0.147 | 0.173 | 0.238 |
| 1 | 100 | 1.7 | 2.1 | 0.152 | 0.158 | 0.227 |
| 1 | 200 | 1.8 | 2.1 | 0.149 | 0.183 | 0.252 |
| 1 | 300 | 1.8 | 2.4 | 0.153 | 0.176 | 0.246 |
| 2 | 50 | 3.5 | 3.7 | 0.226 | 0.251 | 0.333 |
| 2 | 100 | 3.5 | 4.2 | 0.230 | 0.244 | 0.329 |
| 2 | 200 | 3.7 | 4.2 | 0.231 | 0.266 | 0.349 |
| 2 | 300 | 3.8 | 5.0 | 0.237 | 0.251 | 0.351 |
| 4 | 50 | 7.0 | 7.2 | 0.328 | 0.336 | 0.439 |
| 4 | 100 | 6.9 | 8.0 | 0.333 | 0.356 | 0.446 |
| 4 | 200 | 7.3 | 8.1 | 0.320 | 0.350 | 0.443 |
| 4 | 300 | 7.6 | 9.7 | 0.323 | 0.355 | 0.454 |
| 6 | 50 | 10.4 | 10.6 | 0.396 | 0.407 | 0.507 |
| 6 | 100 | 10.3 | 11.4 | 0.394 | 0.431 | 0.516 |
| 6 | 200 | 10.7 | 11.6 | 0.379 | 0.421 | 0.515 |
| 6 | 300 | 11.3 | 13.9 | 0.383 | 0.424 | 0.522 |
| 12 | 50 | 20.4 | 20.6 | **0.532** | 0.551 | 0.635 |
| 12 | 100 | 20.1 | 21.0 | 0.517 | 0.572 | 0.640 |
| 12 | 200 | 20.6 | 21.7 | 0.504 | 0.558 | 0.637 |
| 12 | 300 | 22.0 | 24.8 | 0.497 | 0.554 | 0.637 |

Expansion (`m=50` anchors + one graph hop), the mechanism that actually works:

| dim | scan% | relation co-location | value recall@10 |
|---|---|---|---|
| 50 | 1.63 | 0.479 | 0.183 |
| 100 | 1.69 | 0.549 | 0.203 |
| 200 | 1.64 | 0.565 | 0.222 |
| 300 | 1.57 | **0.575** | 0.213 |

## What this says

**A larger dimension does not help value routing — it hurts it.** At matched b, recall@10 falls
monotonically from 50d/100d to 300d (0.706 → 0.645 at b=2, 0.898 → 0.832 at b=6), and since the
same b scans more at 300d the loss is not a cost artefact. 50d and 100d are within noise of each
other; the good region is 50–100d and there is no reason to pay 6× the bytes for 300d. **It does
not help the partition-based relation view either**: view A is flat-to-slightly-worse
(0.226 → 0.237 at b=2, but 0.532 → 0.497 at b=12) and view B is flat-to-mixed (0.551 → 0.554 at
b=12, unchanged at b=2). **It does help anchor-based expansion, once, and then saturates**: co-location
m=50 goes 0.479 → 0.549 → 0.565 → 0.575 at essentially the same 1.6% scan, i.e. +0.10 absolute
(+20% relative) at 300d, with about 70% of the gain already at 100d. The mechanism is the overlap
number in the fixture table above: higher-dim GloVe packs true WordNet neighbours into the top-m
cosine anchors more often, so the hop starts from better anchors — the graph still does the work,
it just gets a better entry point. Practical read: if the relation path is expansion, 100d is the
cheap win (+0.07 co-location for 2× the vector bytes); 300d buys another 0.026 for 3× on top of
that and costs 0.06 value recall. Nothing here suggests dimension is a lever for the value path.

## Commands

Run from `experiments/multiview-glove-wordnet-2026-10-30/`. 50d fixture was reused as-is and
never rewritten (mtime unchanged); the three new fixtures were built from the same code path.

```
python build_fixture.py --max-words 20000 --dim 100
python build_fixture.py --max-words 20000 --dim 200
python build_fixture.py --max-words 20000 --dim 300

python multiview_real.py --K 64 --maxb 14 --npz ..\..\data\glove_wordnet.npz
python expansion.py                                  --npz ..\..\data\glove_wordnet.npz
python multiview_real.py --K 64 --maxb 14 --npz ..\..\data\glove_wordnet_100d.npz
python expansion.py                                  --npz ..\..\data\glove_wordnet_100d.npz
python multiview_real.py --K 64 --maxb 14 --npz ..\..\data\glove_wordnet_200d.npz
python expansion.py                                  --npz ..\..\data\glove_wordnet_200d.npz
python multiview_real.py --K 64 --maxb 14 --npz ..\..\data\glove_wordnet_300d.npz
python expansion.py                                  --npz ..\..\data\glove_wordnet_300d.npz
```

Raw stdout of each run: `data/_run_{50,100,200,300}d_{mv,ex}.txt` (gitignored).
Wall clock: build ~40 s (300d ~90 s), `multiview_real` 99 s at 100d, `expansion` 23 s at 100d;
300d runs are ~2× that, still minutes not hours.

## Open

- The value-recall decline is measured, not explained. Hubness / anisotropy of higher-dim GloVe
  is the obvious candidate (a few vectors dominate cosine top-10, so kmeans buckets stop
  aligning with exact-NN neighbourhoods) but it was not tested here.
- Expansion was only probed at m=50. Whether the 50d→300d gap widens or closes at small m
  (cheaper operating point) is unmeasured.
