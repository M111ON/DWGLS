# ZONE-2 status — BCR / HNSW / multi-map (2026-11-14)

**Status: PARKED.** Nothing in Zone-2 has a production consumer, and nothing is broken.

## What Zone-2 is

`core/bcr.h`, `core/bcr_mm.h`, `core/bfs_hnsw.h`, spatial-locality bench — from commit
"Add spatial locality benchmark, deterministic HNSW, BCR (AVL + multi-map inverted index)"
(`9bcfc92`) and ses_e2e2ea0f5ffe6GXUVp0fTK7uF2. They are **storage / address primitives**
that dock into the Zone-1 host (`BfsSystem`, ses_e35616249ffevYnpa0pYnQ6A2c). They are a
different level from the ANN service on :8096 and should not be compared to it directly.

## What is proven (on their own address space — no DB involved)

| item | result |
|---|---|
| `bcr` | 76.3x vs flat, lossless address roundtrip |
| `bcr_mm` | multi-map posting list passes |
| `bfs_hnsw` | recall@10 0.960, 5.99x, on its own grid |
| `spatial_locality` | bijection over 20,736 cells |

These are valid for the layer they measure. Per #6365 this layer is judged by topology
roundtrip and unique span, not by retrieval recall.

## What was tested and failed

One placement only: **PCA-2D of the real nomic 768-d vectors onto a 144x144 grid**, used as
the router (`tools/probe_grid_real_2026-11-14.py` in the ann repo, 54,373 x 768, 200 queries,
truth = exact cosine top-10 excluding self).

| scan | recall@10 (grid) | random control |
|---|---|---|
| 0.7% | 0.0995 | 0.0025 |
| 1.8% | 0.1805 | 0.0050 |
| 8.6% | 0.4860 | 0.0290 |
| 20.6% | 0.7795 | 0.0700 |
| 54.6% | 0.9770 | 0.2245 |

The grid carries real structure (far above the random control) but it is clearly worse
than the existing anchor route on the same corpus (live: 0.9295 @ 21.9% scan; K=56 anchors,
top-4 buckets: 0.791 @ 9.64%). **Verdict: the PCA-2D placement does not pass the gate.**
This is a statement about that placement, not about Zone-2 as a whole.

Not tested: any other placement (for example choosing two meaningful dimensions of the
field, or a different cell definition), and the C-level router harness
(`bfs_hnsw_search` on a real distribution).

## Do not

- Do not wire `bfs_hnsw` / `bcr` as a retrieval router on a PCA-2D placement.
- Do not quote the recall numbers above as production recall; they are 200 self-lookup queries.
- Do not measure Zone-2 with retrieval recall or scan% alone.

## To unpark

Someone has to define the placement: which dimensions of the field form the address and
what a cell is. Then run the router harness in C on the real distribution, plus a
topology / unique-span check, with a negative control (random queries) alongside.

## Notes on earlier numbers

The first "arm B" row on the Z2-a card was invalid (it read `kv_anchors.bin` from offset 0;
the file has a 5-float header). It was re-measured with the header skipped; the verdict
did not change.
