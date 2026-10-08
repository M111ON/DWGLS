# ANN capacity & scaling — measured (2026-10-08)

**Question:** how much data can the ANN index hold, and does more data cost speed?

## Answer in one line

Capacity is bounded by **memory** (~1.48 MB per 1,000 entries at dim 384) and by
a hard routing cap (`LZ_ANCH_MAXK = 8`). Speed is **linear in N** at exact recall
— doubling the index doubles query time. The anchor route is supposed to prune
that, but on the current data it prunes nothing (measured), so it is O(N) today.

## Measured scan curve (dim 384, min of 20 reps, synthetic clean data)

| N | parse_ms | scan_ms | us/item | mem_MB |
|---|---|---|---|---|
| 2,881 | 23.7 | 1.38 | 0.478 | 4.26 |
| 5,762 | 55.1 | 2.77 | 0.481 | 8.53 |
| 11,524 | 104.2 | 5.58 | 0.484 | 17.06 |
| 23,048 | 210.1 | 11.91 | 0.517 | 34.11 |
| 46,096 | 421.3 | 25.75 | 0.559 | 68.23 |
| 92,192 | 860.1 | 56.04 | 0.608 | 136.45 |

Reads:
- **scan_ms is linear in N** (1.38 → 56.04 for 32× data). Query cost scales with
  the index unless routing prunes.
- **us/item is nearly flat** (0.478 → 0.608) — per-entry cost is a hardware
  constant; the mild rise is cache pressure as the working set leaves L2/L3.
- **parse is linear too** (~0.0093 ms/entry) but paid **once** (load-once cache,
  reload only on mtime change) — not per query.
- **memory ~1.48 MB / 1,000 entries** = 384 floats (1.5 KB) + norm + pointer.

## The two hard caps (from the code, not guessed)

1. **`LZ_ANCH_MAXK = 8`** (`tools/gguf_lazy_serve.c:273`) — the anchor route walks
   at most 8 buckets. `ANCHR_MAXK = 512` exists in `core/anchor_route.h` but the
   serve clamps to 8. So routing quality is **fixed** while N grows.
2. **budget auto-raises to N** (`:471`, `else if (nent > budget) budget = nent`) —
   the default 512 candidate budget is raised to cover the whole index, so the
   walk is effectively **full scan**. Confirmed live: a query on the 2881 index
   reports `items_scored 2881 / 2881`, `truncated 1`.

## Why it does not prune today (measured, not opinion)

`docs/ANN-PRUNING-LIMIT-2026-10-08.md`: exact recall costs 100% of the index. The
selection rule is *coverage* (`anchor in walked OR anchor2 in walked`); with 2881
entries each carrying 2 anchors, one walked bucket already covers 24–30% of the
index, and 8 buckets cover everything. Better centroids do not fix it (k-means
fit 0.934 → 0.944 moves exact cost only 100% → ~60%). This is the data geometry,
not a tuning miss.

## Practical capacity limits

| limit | value | where |
|---|---|---|
| memory per 1k entries (dim 384) | ~1.48 MB | measured |
| memory at 1M entries (dim 384) | ~1.48 GB resident | extrapolated (linear) |
| parse time at 1M | ~9.3 s (once) | extrapolated |
| scan at 1M | ~0.6 s/query | extrapolated (linear) |
| routing cap | 8 buckets | `LZ_ANCH_MAXK` |
| parse line buffer | 16384 B | `char line[16384]` — a dim-1024 entry (~8 KB vec) still fits, but is close |

So: **hundreds of thousands of entries are fine in memory** on this box; **speed
is the real wall**, and it is linear until routing prunes — which it does not.

## Reproduce

`experiments/ann-capacity-2026-10-08/capacity_probe.c` — standalone, not in the
build:
`gcc -O2 -Wall -I. -Icore -o build/capacity_probe capacity_probe.c -lm && ./build/capacity_probe`

(The scan loop writes to a `volatile` sink; without it the optimizer elided the
loop and `scan_ms` read 0.000.)

## What would change the curve

- Raise `LZ_ANCH_MAXK` — does not help: measured, exact recall still needs 100%.
- **Prune, accept approximate recall** — topb=4 scores 62% of the index for
  recall 0.875 (a real operating point if approximate is allowed).
- **Tombstone warm path** — makes *repeat* queries cheap (already persisted), but
  novel queries still pay the linear scan. Not yet wired to the route order.
- **Graph refine** (`maze_walk_cli`) — the two-stage route→graph idea; not wired.
