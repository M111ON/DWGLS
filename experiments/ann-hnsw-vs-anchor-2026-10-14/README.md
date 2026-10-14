# ANN: HNSW vs anchor+frustum, and the CAPO shift — measured (2026-10-14)

**Question (owner):** the project chose to build its own ANN system over HNSW.
Is that right? Which system actually *works* and is worth continuing — one that
integrates with the existing gear/fan structure on the sacred **20736 = 144²**
equation?

## Answer in one line

**HNSW wins on recall+latency but DIES under this workload's lifecycle** (climate
distribution shift + session delete + atomicity). The **anchor route + frustum
descent on the 20736 field** is the system that survives it: regrow is 5× cheaper
than an HNSW rebuild, and a **CAPO address shift is transparent to search**
(top-k unchanged, 0.006 s, no restart). The quadtree×octfold geometric hybrid is
**rejected** (2-D projection loses 88% of a 71-D signal).

## The two systems compared

| | HNSW | anchor route + frustum (this system) |
|---|---|---|
| recall@10 | 0.99 | 0.94 (K=8 top-2) |
| query | 1.26 ms | ~16 ms |
| index | ~110 MB | ~256 KB |
| rebuild on shift | **49.6 s** + 2 instances (blue-green) | **regrow 9.9 s**, atomic swap |
| delete (session expiry) | rebuild to reclaim | perm regenerate (ms) |
| CAPO shift | full rebuild | **relabel 0.006 s** |
| sits on 20736 | no | yes (frustum span = 144<<level, position % 20736) |

**Why HNSW loses here** (docs/ANN-CLIMATE-CAMPAIGN-2026-09-24.md §6): under a
~12 h climate the distribution shifts, sessions expire, and an atomic swap needs
a second instance. HNSW pays a rebuild every cycle *before any query runs*.
Break-even ≈ 6,800 queries/rebuild; below it, or when shifting, HNSW loses.

## Measured results (real corpus: 61,241 × 384 e5-small, unit-norm)

Corpus = `I:/tools/ann/data/memcore/ann_index.jsonl` (see `index_eval/load_data.py`).

### 1. anchor regrow vs HNSW rebuild  (`anchor_capo/ann_regrow_vs_rebuild.c`)
| step | time |
|---|---|
| train (k-means K=8, 25 iters) | 9.533 s |
| assign top-2 | 0.342 s |
| perm | 0.012 s |
| save (atomic rename) | 0.001 s |
| **TOTAL regrow** | **9.888 s** |
| HNSW rebuild (measured, M16) | 49.6 s |
| **ratio** | **regrow 5× cheaper** |

recall@10 = 0.936 (anchor top-2 + exact rerank in bucket).

### 2. CAPO shift vs anchor/frustum  (`anchor_capo/ann_capo_shift.c`)
| piece | keyed on | CAPO cost | rebuild? |
|---|---|---|---|
| anchor decision (centroids) | **vector** (384-D) | 0 s | no — recall 0.9485 → 0.9485 identical |
| frustum descent | address (20736) | 0 s | no — 1000/1000 routes valid after shift |
| placement perm | address (MOD-37) | 0.006 s | no — regenerate ms-scale |
| **TOTAL** | | **0.006 s** | vs retrain 8.663 s = **1444× cheaper** |

CAPO = `addr = (root + capo) % GEO_FULL` (`core/geo_chord.h`); **payload bytes
never move** (`core/clim_record.h`). It is the same "climate slide" as §6.

### 3. S4 derived-table vs S5 trained — S4 FAILS  (`anchor_capo/ann_s4_*.c`)
| | build | recall@10 | bucket occupancy | top-2 coverage |
|---|---|---|---|---|
| **S5 trained** (k-means) | 11.6 s | 0.9485 | 3443–17679 (ratio 5.1) | **34%** |
| S4 derived (normalized) | ~0 s | 0.9865 ⚠️ | **56,667/61,241 one bucket** | **94%** |
| S4 derived K=64 | 0.001 s | 0.7485 | — | — |

**S4's apparent 0.9865 is a COLLAPSE ARTIFACT**, not a win: the derived table
(ac_build_table, pseudo-random from RDH patterns) routes every query to one
bucket, so top-2 scans 94% of the corpus. `include/anchor_centroid.h` warns of
exactly this. **Use S5 (trained).** S4 is a dead end as a search route.

### 4. End-to-end CAPO serve  (`anchor_capo/ann_capo_serve.c`)
Mirrors `lz_mem_search` (anch_route top-b → bucket walk → cosine → top-k), then
applies a CAPO shift mid-flight and re-serves the same 200 queries:
```
SERVE ROUND 1 (before CAPO): 16.35 ms/q
CAPO shift applied        : 0.000000 s (61,241 addresses relabeled, bytes never move)
placement perm regenerated: 0.006000 s
SERVE ROUND 2 (after CAPO) : 15.07 ms/q   (no retrain, no restart)

top-5 identical before/after CAPO: 200/200   -> PASS
CAPO total 0.006 s vs retrain 9.050 s = 1508x cheaper
```

### 5. octfold's role in ANN  (`anchor_capo/ann_octfold_role.py`)
octfold is a **60-slot carrier view** (LANGUAGES.md #10), not a 61k index. Its
principles were tested; none transfer as a search route:
| principle | result |
|---|---|
| P1 8-sector stride-3 as anchor assign | recall@10 **0.286** vs k-means 0.924 — address-based, collapses |
| P2 √2 scale ladder (finer zoom) | no cost win (hybrid-C already proved this) |
| P3 multi-view 2-of-3 union | mild edge vs single-top1, **no win at matched scan** |

**Real role:** octfold = **STRIDE-3 view of ring-24**, the *same* ring as the
fan24 KIS wheel (8 teeth, `FG_WHEEL_KIS=8`). It is a **carrier view / language**,
orthogonal to the search layer — not an index.

## Verdict

- **Search layer → anchor route (S5 trained) + frustum descent.** Survives
  climate + delete + CAPO; sits on 20736.
- **HNSW → rejected for this workload** (rebuild lifecycle), despite higher recall.
- **S4 derived-table → rejected** (routing collapse).
- **quadtree×octfold hybrid → rejected** (2-D projection; see REPORT-INDEX-EVAL.md).
- **octfold → carrier view only** (ring-24 stride-3), not an index.

## Files

```
index_eval/    — the original ANN index evaluation (HNSW/IVF/PQ/geometric hybrid)
  REPORT-INDEX-EVAL.md (../)          full delegated report (369 lines)
  RESULTS-INDEX-EVAL-INDEP.md (../)   independent prototype results
  load_data.py, diag.py               corpus load + diagnostics (held-out oracle)
  bench_ann.py, bench_geo.py, bench_pq.py   HNSW/IVF vs geometric vs PQ sweeps
  harvest.py, ann_eval_*.py           result harvest + standalone prototypes
anchor_capo/   — the anchor+frustum / CAPO work (this session)
  ann_regrow_vs_rebuild.c             (1) regrow vs HNSW rebuild
  ann_capo_shift.c                    (2) CAPO shift, 3 address-coupled pieces
  ann_capo_serve.c                    (4) end-to-end CAPO serve proof
  ann_s4_test.c, ann_s4_occupancy.c   (3) S4 derived-table (FAILS — collapse)
  ann_capo_test.py, ann_climate_test*.py, ann_delete_fixed.py   HNSW lifecycle probes
  ann_update_cost.py, ann_shift_test.py                        update-cost / shift probes
  ann_octfold_role.py                 (5) octfold role in ANN
REPORT-INDEX-EVAL.md        delegated index evaluation (full)
RESULTS-INDEX-EVAL-INDEP.md independent index evaluation
```

Data (`X.f32` 94 MB, `*.npy`, `res_*.json`) is **regenerable** and not committed
(see `.gitignore`); regenerate per the commands below.

## Reproduce

```sh
# corpus (once; writes X.npy in index_eval/)
python index_eval/load_data.py                 # -> X.npy (61,241 x 384)

# index evaluation (needs faiss + hnswlib)
python index_eval/bench_ann.py                 # brute/IVF/HNSW sweeps
python index_eval/bench_geo.py                 # quadtree + octfold hybrid sweeps
python index_eval/harvest.py                   # -> res_*.json

# anchor / CAPO (needs the real corpus as X.f32; core/ headers from the repo)
/i/Anaconda/python -c "import numpy as np; np.load('index_eval/X.npy').astype('float32').tofile('anchor_capo/X.f32')"
gcc -O2 -std=c11 -I ../core -o anchor_capo/ann_regrow  anchor_capo/ann_regrow_vs_rebuild.c -lm
gcc -O2 -std=c11 -I ../core -o anchor_capo/ann_capo    anchor_capo/ann_capo_shift.c -lm
gcc -O2 -std=c11 -I ../core -o anchor_capo/ann_capo_serve anchor_capo/ann_capo_serve.c -lm
gcc -O2 -std=c11 -I ../core -I ../include -o anchor_capo/ann_s4 anchor_capo/ann_s4_test.c -lm
gcc -O2 -std=c11 -I ../core -I ../include -o anchor_capo/ann_occ anchor_capo/ann_s4_occupancy.c -lm
./anchor_capo/ann_regrow anchor_capo/X.f32 61241 384
./anchor_capo/ann_capo_shift anchor_capo/X.f32 61241 384
./anchor_capo/ann_capo_serve anchor_capo/X.f32 61241 384
./anchor_capo/ann_s4 anchor_capo/X.f32 61241 384
./anchor_capo/ann_occ anchor_capo/X.f32 61241 384
```

## Honest scope

- Query vectors are **real index rows** (in-distribution), an upper bound on
  difficulty — same caveat as `docs/ANN-CAPACITY-*`.
- Latency is single-threaded (ranking holds; absolute scales with cores).
- The fork `I:/tools/ann` has **no address layer** (no 20736/CAPO/frustum) —
  only anchor + placement perm. CAPO wiring would land in the upstream
  `tools/ann_pipe_real.c` path, not the fork.
- This does not change the ANN direction; it confirms it (anchor+frustum over
  HNSW for this lifecycle).
