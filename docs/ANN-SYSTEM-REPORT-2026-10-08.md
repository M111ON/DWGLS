# ANN System Report — 2026-10-08

Status of the ANN (approximate nearest-neighbour) retrieval subsystem: what runs,
what was measured, where it brakes, and what to build next. Everything marked
**[measured]** was produced on this box; **[concept]** is an idea with no code;
**[extrapolated]** is a linear projection from measured points, not a result.

---

## 1. What works today (verified)

**A live search server on real data.** `tools/gguf_lazy_serve.c` serves
`POST /v1/state/search` on port 8096, backed by an embed sidecar
(`multilingual-e5-small`, 384-dim) on 18095. Index = `build/kv_index.jsonl`,
**2,881 chunks / 121 docs**. Six live queries returned 200 with the correct
documents (scores 0.85–0.88). Repeat queries are deterministic (same sids +
scores). Cached queries answer in 0.016–0.16 s.

**Two test groups cover it** (all green in `make test` 154/154 + TIER2 4/4):
- `test_anchor_route`, `test_anchor_routed`, `test_anchor_determinism`,
  `test_anchor_tess` (KV group) — router, placement, determinism, tile serve.
- `test_lazy_serve_search` (TIER1) — response framing, topk fail-loud, recall
  vs an independent cosine oracle, and an **opt-in live-HTTP section**
  (`LZ_SERVE_ADDR=host:port`) that asserts the real response bytes parse and
  topk>8 returns 400. Hermetic by default (no socket, no flake).

**Tombstone persistence** (commit `820ec9e`). The fog latch
(`core/geo_wang_latch.h`) records which routes were walked; every walk appends a
fingerprint to the P3 route log and boot replays it. Proven live: cold boot
`replayed=0/shut=6` → kill → warm boot first query `replayed=25/shut=7` (the
tombstone came back from the file alone).

---

## 2. Architecture (the pieces)

| layer | file | job |
|---|---|---|
| **route** | `core/anchor_route.h` | anchors RANK: nearest-centroid L2, top-b bucket route |
| **place** | `kv_index.order` | geo_jump placement: stride-37 slot perm, bucket-major |
| **serve** | `tools/gguf_lazy_serve.c` | HTTP, index cache, candidate scan, fog receipt |
| **tombstone** | `core/geo_wang_latch.h` + P3 log | "enter by walking": lit set = fingerprint |
| **refine** | `tools/maze_walk_cli.c` | kNN graph over 2560 fine anchors, greedy descent (built, not wired) |

The route→place→serve chain is production. The refine layer exists as a
standalone tool and is not connected to the server.

---

## 3. Benchmarks [measured]

### Live index (2,881 docs)
- Anchor route **does not prune**: a query reports `items_scored 2881 / 2881`,
  `truncated 1`. Cost is O(N) at exact recall.

### SIFT1M, n=1000 queries, recall@10 (`docs/ANN-CLIMATE-CAMPAIGN-2026-09-24.md`)
| method | recall@10 | scan% | ms/q | index | build |
|---|---|---|---|---|---|
| maze b128 (ours) | 0.9617 | 5.32 | 32.1 | 0.44 MB | ~140 s |
| flat top-C + top-2 overlap (champion) | 0.9657 | 2.20 | — | 2× bytes | 21 s |
| HNSW-M32/ef128 (faiss) | **0.9894** | — | **0.38** | ~300 MB | ~356 s |

We **lose latency ~84×**, **win index memory ~680×** and build time. Ceiling is
the hard-bucket wall, not a tuning miss.

### Capacity / scaling (`experiments/ann-capacity-2026-10-08/`)
| N | parse_ms | scan_ms | us/item | mem_MB |
|---|---|---|---|---|
| 2,881 | 23.7 | 1.38 | 0.478 | 4.26 |
| 11,524 | 104.2 | 5.58 | 0.484 | 17.06 |
| 46,096 | 421.3 | 25.75 | 0.559 | 68.23 |
| 92,192 | 860.1 | 56.04 | 0.608 | 136.45 |

- **scan_ms is linear in N**; us/item nearly flat (0.478 → 0.608).
- **parse is linear** (~0.0093 ms/entry) but paid once (load-once cache).
- **memory ~1.48 MB per 1,000 entries** at dim 384.
- Extrapolated: 1M entries ≈ 1.48 GB resident, ~9.3 s parse (once), ~0.6 s/query.

---

## 4. Brake points — where it stops, and why

These are the walls. Each is **[measured]**, not assumed.

**B1 — Hard-bucket wall (exact recall).** `docs/ANN-PRUNING-LIMIT-2026-10-08.md`:
exact recall costs 100% of the index. The selection rule is *coverage*
(`anchor in walked OR anchor2 in walked`); with each entry carrying 2 anchors, one
walked bucket already covers 24–30% of the index, and 8 buckets cover everything.
Better centroids do not fix it: k-means fit 0.934 → 0.944 moves exact cost only
100% → ~60%. **This is data geometry, not tuning.**

**B2 — Routing cap `LZ_ANCH_MAXK = 8`.** The serve walks at most 8 buckets
(`ANCHR_MAXK = 512` in the header, but the serve clamps to 8). Routing quality is
**fixed** while N grows — a capacity ceiling, not a speed one.

**B3 — Budget auto-raises to N.** `else if (nent > budget) budget = nent` — the
default 512 candidate budget is raised to cover the whole index, so the walk is a
full scan. This is why the live index reports 2881/2881.

**B4 — Per-bucket cap is the wrong accelerator.** `docs/FORAGE-GATE-2026-10-05.md`
§9: cap 512 → recall 0.6236, cap 2048 (= baseline) → 0.6501. Cutting members cuts
recall faster than it buys speed. Measured negative result.

**B5 — Line-buffer ceiling.** `char line[16384]` in the index parser. A dim-1024
entry (~8 KB of vector text) still fits but is close; a larger dim would need a
bigger buffer. Not a current limit, a future one.

**B6 — Maze does not beat HNSW on latency.** PLAN-HNSW-MAZE gate: beat HNSW ≥1
metric, lose ≤10% others. Strict gate **FAIL** on latency (84×). We compete on
index memory and build time, not query speed.

---

## 5. Concepts — parked, not built [concept]

Parked deliberately so the plain system stays stable first. Not in the build.

### 5.1 Tombstone warm path (`docs/TOMBSTONE-INDEX-2026-10-08.md`)
The tombstone persists but is **report-only**: it does not yet reorder the walk.
The intended use: a repeated query reads a route that is already shut → skip it →
faster. This is the **hot/cold axis**, orthogonal to the geometry axis (B1).
*Next lever:* route order reads the latch (shut-first + budget stop).

### 5.2 Pomato / Hourglass (`experiments/apex-hidden-2026-09-29/`)
- **Pomato** = the two outputs of one stem: above-ground (graph/HNSW refine) and
  below-ground (KV/field storage); the **tombstone is the stem** where the walk
  meets the store. Not two sides — three, with the tombstone as the joint.
- **Hourglass** = the waist between two fields that correct each other. Measured
  in `experiments/apex-hidden-2026-09-29/`: fold 64→16→4→1 with signed gates is
  **lossless** (T1_bad=0), zeroing the apex breaks 48/64 (proves the apex carries
  load), and the twin is a **reflection** (mirror∘mirror = identity, 16/64 same +
  48 at exactly −2·g3). A correct/verify primitive — **not a search primitive**.
  Open question: what are the two fields, and what is the shared waist?

### 5.3 Field bridge: `12 × 6 × 8 × 6 × 6 = 20736 = 12⁴`
(`experiments/field-bridge-2026-10-08/`) The storage ladder and the tesseract
tensor ladder are the same product: `18 × 8 × 144 = 20736` because `18 = 3×6` and
`144 = 4×6×6`. Every running product hits a real constant (72 = wl choice, 576 =
`TD_HALF`, 3456 = `FRUSTUM_DATA_SZ`, 20736 = `GEO_FULL`). **Boundary:** there is
**no code** that composes frustum faces into a cube, and none is required;
`6 == 6` is a direction/face **rule identity**, not assembly. The bridge is an
observation, not an implementation. Open question: does making it explicit buy a
shared address (one walk, no translation), or is it arithmetic coincidence?

---

## 6. Development directions (ordered)

1. **Wire the tombstone warm path** (cheap, unlocks 5.1). Route order reads the
   latch: shut branches first, OPEN after, budget stop. Then **measure warm vs
   cold recall separately** to prove the tombstone does not touch novel recall.
2. **Decide the operating point** from B1/B4: exact-no-prune (correct, O(N)) vs
   prune-approximate (topb=4 → recall 0.875 at 62% scan). This is a product
   choice, not a code change.
3. **Wire the refine layer** (`maze_walk_cli`) as a second stage after the anchor
   route — the two-stage route→graph idea. This is the only path that attacks
   latency (B6) rather than memory.
4. **Prove or drop the field bridge** (5.3) with one measurement, not a diagram.
5. **Log compaction** for the P3 route log (CLEAR epoch) so it does not grow
   unbounded.

**Do not** raise `ANCHR_MAXK` expecting exact pruning — measured, it does not
help (B1). **Do not** tune per-bucket caps for speed — measured negative (B4).

---

## 7. Reproduce

- Suite: `make test` (TIER1 154 + TIER2 4).
- Capacity probe: `experiments/ann-capacity-2026-10-08/capacity_probe.c`
  (`gcc -O2 -Wall -I. -Icore -o build/capacity_probe capacity_probe.c -lm`).
- Field-bridge probes: `experiments/field-bridge-2026-10-08/*.c` (same build line).
- Live serve: embed on 18095, then
  `LZ_EMBED_HOST=127.0.0.1 LZ_EMBED_PORT=18095 LZ_ROUTE_LOG=build/lz_route.log
   ./build/gguf_lazy_serve <gguf> --serve 8096 build/backend_cpu`.
- Bench scripts: `bench/ann_route_*.py`, `tools/forage_real.c`,
  `tools/maze_walk_cli.c`.
