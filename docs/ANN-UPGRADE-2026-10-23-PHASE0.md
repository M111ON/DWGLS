# ANN Upgrade — Phase 0 receipts (2026-10-23)

Companion to `docs/ANN-UPGRADE-PLAN-2026-10-22.md`. This file records what was
actually built and measured, not what was intended.

## 1. What changed in `I:\tools\ann\src\ann_search.c`

| Change | Why |
|---|---|
| Phase 0 canary (`ANN_CANARY=1`) | The Option B decision needs numbers, not opinions: per-query cache/align/buckets/note, boot cost, and a one-shot full re-assignment sweep. |
| `GET /v1/canary` | Serves the counters + bucket-size skew (the phase-2 gate). |
| `ANN_LABELS` default flipped to **derive** | Sidecars were observed drifting faster than any writer refreshed them; derive was proven identical. `ANN_LABELS=stamp` opts back in for A/B. |
| Append-only contract for the table | `n < ntr` still refuses (different corpus); `n >= ntr` routes — growth is legal (`#7565`). |
| `.order` mismatch → derive, not refuse | A stale `.order` used to kill the route **silently** (fell to full scan, plausible hits, no error). Now it derives and increments `order_stale`. |
| Route self-heal in the loader thread | When the row/label binding is dead, restamp (re-route, never rewrite a vector), throttled to 1/min, index-mtime quiet, logged as an event. |
| `/health` additions | `labels`, `order_stale`, `heal_ok`, `heal_fail`. |

## 2. Live incident found while instrumenting

Measured on the production data dir at 03:55–04:20:

```
ann_index.jsonl   rows growing continuously (62,857 -> 62,861 -> 62,866)
ann_index.stamp   ABSENT
ann_index.order   62,862 rows (5 more than the index parsed)
ann_anchors.bin   ntr = 62,857
```

Three layers, three different row counts. The process on `:8096` still routed
**only because its in-memory cache predated the drift**; any restart would have
served a full scan while looking healthy. This is the H1/H2 failure class from
`#7563`, observed live rather than reasoned about.

Consequence: no amount of sidecar writing fixes this — a writer appends every
few minutes. The structure that survives is "derive from rows + table".

## 3. Measurements (receipts)

**Option B boot gate** (`ANN_CANARY_RECOMPUTE=1`, 62,851 rows, K=8, dim=384):

```
option_b reassign: 62,851 rows in 316.4 ms  = 5.03 us/row
```

**Boot, normal derive path:** `index_ms 34,487` (JSONL parse, unchanged) +
`labels-derive 325.3 ms`. Derive is ~1% of boot.

**Route identity** — 300-query set, live `:8096` (stamp, warm cache) vs new
binary (derive, no `.stamp`, stale `.order`, short `ntr`):

```
IDENTICAL 20/20  (hit sids and scores byte-equal)
```

Earlier A/B on a snapshot where all three files agreed: **20/20 identical**,
same bucket lists, same `mode`, same item counts.

**Bucket skew** (K=8, 62,861 rows):

```
counts [11929, 6995, 21532, 7467, 26532, 10793, 15975, 24499]
max 26532 (42.2%)  min 6995 (11.1%)  mean 245.6  cv 8.735
```

One bucket alone exceeds `LZ_SEARCH_BUDGET` (24,000). This is the measured
justification the phase-2 latch geometry (144 = 4 x 6 x 6) was parked for — and
it says the first move is **bucket balance**, not more faces.

**Note state:** `data/lz_route.log` = 419 B / 13 records; replay is cheap and
already works (`fog.replayed`).

## 4. Gates status (from the plan)

| Gate | Status |
|---|---|
| G1 boot without `.stamp` | PASS (route up, 20/20) |
| G2 boot without `.order` | PASS (same — derive never reads it) |
| G3 identity vs stamp mode | PASS 20/20 |
| G4 rows grow past `ntr` | PASS (append-only contract) |
| G5 index shrinks | PASS (refuses, loud: `routed:0` + diag) |
| G6 boot cost | PASS — 325 ms derive vs 34.5 s parse |
| G7 loud on inconsistency | PASS — `order_stale` / `heal_*` in `/health`, events in the canary log |

## 5. What is NOT done

- Restamp still re-parses the index (~50 s) instead of borrowing the loader's
  buffer. Correct first; borrow when the heal fires often.
- `lz_labels_derive`'s canary event reports `ms:0.0` (the measured value is in
  the boot event). Cosmetic, fix when touching it.
- No test harness file yet — the receipts above came from ad-hoc scripts in
  `%TEMP%`. Port to `scripts/` when the shape stops moving.
- Bucket balance (phase 2) is **open** with measured skew as its input.
- `.stamp`/`.order` writers still run (restamp/regrow). They are caches now;
  deleting the write path is a later commit, once nothing reads them.

---

## Phase 2 — bucket balance, measured and shipped (2026-10-23, same day)

### The structural defect

`src/ann_search.c` picked the bucket count with a fixed rule:

```c
int dim = a[0].dim, K = n >= 64 ? 8 : n >= 32 ? 4 : 2;
```

Eight buckets for any corpus >= 64 rows. At 62,868 rows that produced buckets
of 7,152..25,693 rows while `LZ_SEARCH_BUDGET` was 24,000 — **one bucket alone
can exceed the walk budget**, so the walk truncates on every query and recall
is decided by where the cut lands, not by the geometry.

A second, quieter defect: `LZ_ANCH_MAXK` was 8 and several route buffers are
`int t2[LZ_ANCH_MAXK]`. Raising K without raising that constant overflows the
stack in `lz_route_cached`, i.e. the old binary cannot serve a K>8 table at all.
`LZ_ANCH_MAXK` is now 64 (regrow stack arrays 256 B); the table format already
allowed `ANCHR_MAXK` (512).

### Metric correction first

`/v1/canary` bucket_skew divided by `ANCHR_MAXK` (512 slots) instead of K, so
`mean`/`cv`/`min` were diluted by ~500 zero buckets (reported min=0 always).
Now the statistics are taken over the K live buckets and the response carries
`"k"`. Numbers below are post-fix and not comparable to the first-version ones.

### Skew vs K (62,868 rows, counts include anchor + anchor2)

| K | min | p50 | p90 | max | max % of rows | cv | regrow (train) |
|---|-----|-----|-----|-----|---------------|----|----------------|
| 8  | 7,152 | 16,779 | 25,693 | **25,693** | 40.9% | 0.357 | 9.4 s (8.9 s) |
| 16 | 511 | 8,471 | 12,175 | 15,661 | 24.9% | 0.468 | 17.5 s (16.7 s) |
| 24 | 1,833 | 5,165 | 7,794 | **9,081** | 14.4% | 0.350 | 26.1 s (25.0 s) |
| 32 | 315 | 3,903 | 5,720 | 8,459 | 13.5% | 0.471 | 34.3 s (32.9 s) |

cv does not improve — the embedding manifold is simply lumpy. What improves is
the **largest** bucket, which is what the budget has to cover.

### Recall vs K (numpy brute oracle, budget 24,000)

60 queries:

| K | scored | trunc | R@5 | R@10 |
|---|--------|-------|-----|------|
| 8  | 24,000 | 60/60 | 0.9333 | 0.9333 |
| 16 | 24,000 | 60/60 | 0.9767 | 0.9700 |
| 24 | 12,635 | 0 | 0.9667 | 0.9600 |
| 32 | 14,133 | 0 | 0.9567 | 0.9550 |

300 queries (the gate sample):

| K | scored | trunc | R@5 | R@10 |
|---|--------|-------|-----|------|
| 8  | 24,000 | 300/300 | 0.9533 | 0.9367 |
| 24 | 16,329 | 0 | **0.9680** | **0.9613** |
| 24 (auto rule, 62,869 rows) | 16,446 | 0 | 0.9693 | 0.9597 |

K=24 wins on both axes: +1.5 pp R@5, +2.5 pp R@10, and ~32% fewer rows scored
because the walk stops truncating instead of being cut at the budget.

### The rule now in code

```c
/* ANN_ANCHOR_K overrides; otherwise: target ~3000 rows/bucket
 * (budget/8), rounded up to a multiple of 4 (144 = 4*6*6 latch geometry),
 * clamped to [8, 48]. 62.8k rows -> 24. */
int want = (n + 2999) / 3000;
want = ((want + 3) / 4) * 4;
```

The 48 cap binds at ~144k rows; beyond that the corpus needs placement
(#7565/#7166 territory), not a wider table. `ANN_ANCHOR_K` remains for A/B.

### Applied to live :8096

- Binary swapped to the current build (backup `build/ann_search_batch_v1.exe`);
  keepalive restarted it.
- `POST /v1/state/regrow {"force":1}` -> `K=24`, `nent=62,869`,
  `regrow.ms=25,255` (load 0.7 / train 24,171.8 / route 966.0 / rewrite 5.1 /
  save 0.8).
- `/health`: `routed:1`, `labels:derive`, `k:24`, `stamps_live:62,869`,
  `order_stale:0`.
- `/v1/canary` skew: `k=24 cv=0.39 min=1,957 p50=5,135 p90=7,664 max=9,781`
  (was max 26,533 at K=8).
- Live query `"memcore anchor bucket routing"`: 5 hits, top score 0.8673,
  cold 590 ms / warm 35 ms. (Warm path reorders after the first query writes
  its latch stamp — expected, not new.)

### Still open

- Auto-regrow on growth: the index gained 8 rows while this work ran; today a
  restart self-heals only if `ntr <= n` and the *table* is still current. A
  watcher that regrows when `n` crosses a threshold (and once when the rule's
  K would change) is the next structural piece.
- The 4 owner decisions in `ANN-UPGRADE-EXT-2026-10-22.md` are untouched.
- `.stamp`/`.order` write path still exists (caches now, nothing depends on
  them for correctness).

---

## Cold-path watcher (2026-10-23)

Implemented in the supervisor, not the server — `I:/tools/ann/ann_keepalive.py`
(the scheduled task `ann-keepalive`, every 5 min). No C change, no new moving
part, and the regrow runs outside any request.

- `k_for(n)` mirrors the C rule exactly: `round_up_to_4(ceil(n/3000))`,
  clamped `[8,48]`, with the old `<64` ladder preserved.
- `_regrow_decision(health, state)` is pure and decides from the **server's**
  live `k`, never from stored state:
  - `routed == 0` -> regrow (the only repair for a dead route)
  - `k_for(n) != health.k` -> regrow (K breakpoint crossed)
  - `n < last_n` -> log "rebuilt?" and watch, do not regrow
  - otherwise no action
- `.anchor_state.json` holds `{n, k, at, note}`; a 10-minute gap throttles it.
- Failure is loud: `note` and the log carry `REGROW FAILED: routed=… k=…` or
  `REGROW ERROR: …`; success records the regrow's own `ms`.
- `--selfcheck` (18 `k_for` cases + 6 decisions), `--plan` (decision only),
  `--force-regrow` (full path).

Receipts:

```
selfcheck ok: 18 k_for cases + 6 decisions
{"act": false, "why": "in contract (n=62869 k=24)", ...}
watch: regrow because forced by --force-regrow
watch: regrow ok (30772 ms), n=62874 k=24
{"act": false, "why": "in contract (n=62874 k=24)", ...}
```

The index grew 62,857 -> 62,874 during this session, which is the condition the
watcher exists for. Next real trigger is n = 72,001 (K 24 -> 28).
## Phase 0.5 — watcher + live instrumentation (2026-10-23, later same day)

### Canary is now always-on (counters), stream opt-in
Counters (`queries/cache_hit/cache_miss/warm_taken/align[4]/buckets_visited/
buckets_shut/skipped_rows/note_events/scored_sum/trunc/budget/k_used`) accumulate
in memory unconditionally; only the per-query file line needs `ANN_CANARY=1`.
`GET /v1/canary` therefore reports REAL service traffic with no flag.

`trunc` excludes warm queries by construction: the warm path sets
`budget = cov` (its own coverage), so scoring up to it is not a cut. Only a cold
walk that exhausts the global cap counts. (The first version counted both and
reported a false truncation on live at budget=6471.)

### Cold-path watcher (`ann_keepalive.py`) — #7560 compliance
Regrow runs in the supervisor, never inside a request:
`_maybe_regrow()` each `--once` (scheduled every 5 min), trigger set =
`route_dead` | `routed==0` | labels stamp-stale | `n < ntr` (shrink) |
`_target_k(n)` changed (K breakpoints 24k/48k/72k/96k/120k/144k).
K rule mirrors the C exactly: `clamp(round_up4(ceil(n/3000)), 8, 48)`.
Guards: state file `ann_keepalive_state.json`, 900 s min gap (a forced regrow is
~25 s train + ~56 s first-request reload; a storm must not loop), POST
`/v1/state/regrow {"force":1}`, then verify `routed==1` and the new K; every
outcome logged with a timestamp. Flags: `--plan` (dry), `--force-regrow` (manual).

Verified: `--plan` seeded at n=62869 k=24 and correctly declined work; `--once`
reported healthy. The act-path is wired but has not yet fired on real growth
(next breakpoint is 72k rows).

### Live state after this pass
`:8096` rebuilt from current source (`ann_search_batch_v1/v2.exe` kept):
`entries=62874 k=24 labels=derive routed=1 stale=0`; 4 real queries -> 3 hits
each, top scores .8557/.8542/.8838/.9082, `trunc=0 warm=4 scored=52835
(13.2k/query) notes=6`.

### Still open
- 4 owner decisions (unchanged: remembered, not asked).
- Latch 3-axis geometry (phase 2): skew data now exists (`cv=0.53`, max/mean 4.1
  at K=24) but no consumer has been chosen.
- `fog.replayed=0` on live — the note file the service reads is empty while the
  P3 log lives elsewhere; note-as-authority is still only half wired.

## Phase 2b — K density (2026-10-23, measured then shipped)

Rotation probe (20,000 real rows, real K=24 table, argmax dot = `anch_assign`):

| transform | labels changed | max bucket | cv |
|---|---|---|---|
| orthogonal, applied to query AND centroid | **0 / 20,000** | 2,704 (=baseline) | .9381 (=baseline) |
| centroid only | 18,163 | 10,198 | 2.73 |
| query only | 19,826 | 7,113 | 2.19 |

Conclusion: a rigid rotation is a coordinate change, not a partition change (inner
product is invariant under the orthogonal group) — it moves **zero** collisions.
Rotating one side is not a rotation of the space, it is a different table, and it
made skew worse. Rotation is only meaningful for **storage labelling** (the
`bucket % 144` latch cell at `ann_search.c:588`), never for bucket occupancy.

K ladder (300q, budget 24000, numpy brute oracle):

| config | scored (last q) | R@1 | R@5 | R@10 | trunc |
|---|---|---|---|---|---|
| K=8 | 24,000 | — | .9533 | .9367 | 300/300 |
| K=24 | 16,329 | — | .9680 | .9613 | 0 |
| K=64 | 10,264 | .9833 | .9627 | .9623 | 0 |
| K=64 topb=5 | 12,411 | .9867 | .9307 | .9303 | 0 |

- More K = cheaper walk, recall unchanged; routing cost is O(K*dim) = 0.2 MFLOP
  at K=512, negligible beside walking a bucket. The fence was never compute.
- topb stays 3: extra buckets spend the budget on farther rows and cost @5/@10
  (R@1 improves instead).
- Shipped rule: `K = round_up4(ceil(n/1000))`, clamp [8, LZ_ANCH_MAXK=64].
  `ann_keepalive.k_for` mirrors it. At 62.8k -> 64; cap binds from 63k.

Live after swap (`build/ann_search_batch.exe`, backup `_v3`):

    entries=62881 k=64 routed=1 labels=derive stale=0
    bucket skew max 26,533 (K=8) -> 10,725 (K=24) -> 5,303 (K=64)

Scale note (arithmetic, not measured): at 10M rows the blockers are
vector RAM (10M x 384 x f32 = 15.4 GB) and derive cost (~5 us/row = ~50 s),
not K. 10M / 64 = 156k rows per bucket, so placement/hierarchy is the next
lever — not a bigger table.
## 2026-10-23 — Wolfram danger-zone / caridoid zonation in the frustum (probe, NOT shipped)

Question (owner): split zones inside the frustum with the 3-circle / caridoid figure, maybe
as an indirect Hilbert-Peano cursor. Two probes on real data, no service change.

### Probe 1 — does the zonation carry signal? (30,000 rows, real K=64 table)
Projection = PCA-2 of the row vectors (13.4% of variance). Zone code = 3 wedges x 2
half-planes + centre.
- neighbour rows share zone code: **0.6997** vs random 0.2375 (**2.9x**)
- neighbour rows share anchor bucket: 0.5600 (for reference)
- walk-only-same-zone recall ceiling: **0.6997**; with the centre circle admitted: **0.9463**

Verdict: real signal, but as a HARD filter the ceiling is below the shipped bucket path
(K=64 gives R@5 .9627 at 10.3k rows). It cannot replace buckets.

### Probe 2 — zone as bucket selector (200 queries, exact top-10 over 62,888 rows)
Simplified harness (top-3 buckets vs all buckets whose centroid shares the query zone):
- top3-bucket: 3,945 rows scored, R@1 .9800, R@5 .7980, R@10 .7705
- zone-match: 10,004 rows scored, R@1 .9800, R@5 .9150, R@10 .8970

CAVEAT (my bug, stated rather than hidden): the zone function collapsed to 3 sectors +
a noise "core" because the projection was L2-normalised, so `r < 1.0` was degenerate and
the 6-wedge split never fired (`sc.max() < 0.5` is almost never true). So probe 2 measures
a 3-sector split, NOT the full caridoid. Also the harness omits the engine's dual
assignment (anchor + anchor2, ~6 buckets), which is why bucket-only reads .798 here but
.9627 in the C bench.

### What that means
1. Zonation is a coarser partition of the same distance space: it can order work, it
   cannot beat ranking. Use it as a walk-order hint, never as a filter.
2. The only place it composes with existing geometry for free: the 6 wedges = 6 faces,
   i.e. the `bucket % 144` latch cell (= 4 x 6 x 6). A wrong zone then costs one extra
   cell visit, not a lost neighbour — that is the low-risk use.
3. Indirect Hilbert-Peano: the 6 wedge ids give a 1-D sector cursor 0..5, which is the
   Peano order already used by the anchor perm. No new geometry needed.
4. "Danger zone" is measurable and must be measured before use: the band where
   |cos margin| is small is where labels flip. Query fraction in that band is the cost
   of any zone-based decision.

### Next (gated, not started)
Derive P2 at regrow and append to ann_anchors.bin (format version bump); zone-order the
pruned bucket list (zone match first, then score); gate = R@5 >= .9627 at scored <= 10.3k.
Until that gate runs inside the C engine, no zonation code ships.
## Zone-order policy (2026-10-23) — implemented, measured INERT at current scale

Code (I:/tools/ann/src/ann_search.c, env `ANN_WALK_ORDER=zone`, OFF by default):
- `lz_zone_build()` derives two projection axes from the centroid table by power
  iteration + deflation (no stored state, rebuilt on table load).
- `lz_zone_of(x,y)` = 6 wedges (3 sector dots, 0/120/240 deg) + centre.
- The reorder happens on `rb[]` only: buckets matching the query's zone are moved
  first, nothing is removed. `rm` becomes `anchor+zone` when the order changed.
- Placement is right after the graft block, before the warm path, so both the
  warm and cold walks see the new order.

Why it cannot help yet (structural, not a measurement gap):
- Bucket order only matters when the budget CUT actually happens.
- At K=64 the walk scores ~10.3k rows against a 24k budget (43%), i.e. the cut
  never fires; every bucket the route picked is fully walked, so order is a
  no-op by construction. Confirmed: the earlier topb=5 run only degraded recall
  once it pushed scored toward the cap (12.4k) — the same mechanism.
- The zone partition itself is sound (probe: neighbours share a zone 0.6997 vs
  0.2375 random) but its hard-filter ceiling (0.6997, 0.9463 with the core
  circle) stays below the bucket path (0.9627), so filter use is refused.

Revisit when: n grows ~2.3x (walk reaches the 24k cut at K=64) or the budget is
lowered. Gate then: R@5 >= 0.9627 at scored <= 10.3k, against the same 300q
oracle. Note the local box has ~1.5 GB free RAM, which blocks the Python oracle
runs at 62.9k rows (92 MB ndarray allocation fails); run the gate on a machine
with headroom or add a C-side oracle.

## Session 2 (2026-10-23) — sanitize 503 class closed; hourglass pair = 1-XOR pinned

Full handoff: `I:/tools/ann/docs/HANDOFF-2026-10-23-ann-sanitizer-hourglass.md`.

### A. 503 / 500 class (ann commit bd1ce04, 9 files)
- `lz_sanitize` asked the vocab bitmap B before NFKC; B claims all 2682 NFKC-delta
  codepoints are in vocab, so raw delta codepoints reached the tokenizer.
- Fix: NFKC first, gate output codepoints, drop only unrepresentable — `src/ann_search.c:407`.
- Bitmap now derived, not guessed: `scripts/probe_vocab.py` → `scripts/nfkc_vocab.json`
  (**13,829 representable / 52,170 dropped**); `scripts/gen_nfkc_table.py` →
  `src/nfkc_table.h` (**2808 entries**), refuses to write if printable ASCII drops.
- Same-hour catch: U+0020 is representable only through sentencepiece's
  U+0020 → U+2581 remap; the raw lookup stripped every space and concatenated the
  query. Pinned by `tests/space_probe.py` (spaced vs unspaced twin must differ).
- No-regression: `tests/ab_text_parity.py` vs `ann_search_prev.exe` = **12/12
  identical top-5** with A/control **12/12** run first; `bench_warm_recall.py`
  byte-identical across builds (R@1 1.0000, R@5 0.9200, 10501/9647 scored);
  `make test` ALL PASS. Live `:8096`: entries 62913, routed=1, k=64, embed ok.

### B. Hourglass / frustum-pair shift algebra — test_addr_orbit.c T12–T15, 35/35
| oracle | result |
|---|---|
| T12 | `(i<<1) % 20736` → only **10,368 images** (even modulus) ⇒ arithmetic shift is not a permutation |
| T13 | `20736 = 2^8 × 81`; rot8 on `u = i/81` bijective over the whole field, exact inverse, rot8^8 = identity |
| T14 | `d ^ 1` involution over `[0,12)`: **6 pairs, 0 fixed points** ⇒ centre crossing = 1 XOR, composes to `d ^ (k&1)` |
| T14 | `d ^ 6` (face+3) in range for only **8/12** — `12 = 4×3` is not a power of two |
| T15 | k crossings collapse to one XOR — no table, no walk |

Already locked in the same file (not re-derived): ladder `2^a × 3^b` (T4),
`face = d>>1 / orient = d&1` (T6), latch pool `144 × 72 = 10368` with reserved apex
`{0, 10367}` (T8). Geometry source: hexagon side 2, centre `G = 0`, 6 sectors =
3 hourglasses, opposite pair = point reflection `v → −v` (A↔D, B↔E, C↔F from the
protocol coordinates), inner hexagon r=1 = homothety ÷2, mid ring r=√3 (⌀2
semicircles). `core/isometric_map.h:106` `IM_CROSS` is the same self-inverse pairing.

### C. Still open
- Ann latch is still flat: `src/ann_search.c:622` `int c = bucket % 144;` — relayout
  to `(pair << 1) | end` is where the 1-XOR becomes live in the serve path.
- Pool math decides the 3-pair question: wang6 + 3 gen = **5,440 anchors**; 6 faces
  and anchor = **32,640 > 10,368** (unaffordable); 1 bit and anchor = **5,440**** ≤
  10,368 (affordable) ⇒ the 1-bit fingerprint is the only one that fits at that scale.
- G2.5 receipt still missing: prove closing `+X` does not close `−X`
  (`tools/pattern_probe.c:163` `FACE_ID(a,d)` + 6-bit mask is the starting point).
- `I:/tools/ann` main was 1 commit ahead of `origin/main` at handoff (bd1ce04).

### D. Session 3 (2026-10-23) — latch cell geometry landed; G2.5 receipt PASS

**Item 1 — latch relayout (done).** `geo_wang_latch.h` now carries the cell
geometry `cell = (pair << 1) | end`:
`wl_cell_pair` `cell>>1`, `wl_cell_end` `cell&1`, `wl_cell_of(flat)` (negative-safe
`flat % 144`), and `wl_cell_cross(cell) = cell ^ 1` — self-inverse, `T14`'s 6-pair /
0-fixed involution. `src/ann_search.c:621` `lz_fog_id` now reads
`wl_id(wl_cell_of(bucket), 0u)`. Because `wl_cell_of` is the flat cell itself, the
map is the **identity at K ≤ 144**, so the gate ("300q recall unchanged + occupancy
unchanged") is satisfied **by construction — the same bucket lands on the same id**,
not merely measured; the 144-cell axis was never the thing that moved. The 1-XOR is
what the relayout buys: the centre crossing is now one XOR (`wl_cell_cross`) instead
of a table. Both `geo_wang_latch.h` copies were kept byte-identical (ann `include/`,
DWGLS `core/`).

**Item 3 — G2.5 receipt (done).** `tests/test_wang_latch.c` (both copies):
- **T7** — `wl_cell_of` is a bijection on `[0,144)` (144/144, wraps `144→0`), and
  `wl_cell_cross` is an involution with **72 pairs / 0 fixed points / no self-pair**.
- **T8** — closing one end of a pair leaves the other **open** (ends independent),
  closed `+X` ⇒ `−X` open and the mirror; `wl_cell_cross` reaches exactly the partner.

**Finding (T8, structural).** The reserved major-input id `0` is `wl_id(0,0)` = the
`+X` end of pair 0 ⇒ **1 of 144 ends is major-only** (not minor-latchable); 143/144
ends are minor. The G2.5 independence holds at the **cell** level. **Scale caveat:**
the pool math (5,440 anchors) gives 1 latch id per anchor; `2 ends × 5,440 = 10,880 >
10,368` ⇒ at full frustum scale the two ends cannot have separate latch ids and the
collapse becomes real. That anchor-level trade is the remaining decision, not a cell
problem.

**Item 2 — 3-pair decision: decided by arithmetic.** 6 faces/anchor = 32,640 >
10,368 (unaffordable); 1 bit/anchor = 5,440 ≤ 10,368 (affordable) ⇒ 1-bit fingerprint.

**Item 4 — push:** `origin/main == main` (bd1ce04 already pushed).

**Receipts:** DWGLS `test_wang_latch` **ALL PASS (T1–T8)**; ann `test_wang_latch`
**ALL PASS (T1–T8)**; `test_addr_orbit` **ALL PASS (T12–T15)**; ann server rebuilt
clean.

**Item 5 — warm-skip wiring (done).** `ann/src/ann_search.c` now computes
`shut = own || cross` in one helper (`lz_fog_shut`), so coverage and row ordering
use the same hourglass-aware predicate. The walk/P3 log still closes only the
bucket actually traversed; the partner is a read-side warm hint, not a fabricated
fingerprint. The live binary was rebuilt and swapped as `build/ann_search_batch.exe`;
the old binary was retained as a timestamped backup and the duplicate unbound
process from the restart race was stopped.

**Item 5 gate receipt.** `tests/ab_door_live.py --queries 300` ran against the
rebuilt binary and a 62,934-row live-index copy: R@5 **1.0000 → 1.0000**, R@10
**1.0000 → 1.0000**, paired better/same/worse **0/300/0**. ON mode was `door` for
300/300 queries. `items_scored` was 62,934 in both arms and p50 was 35.61 ms OFF
vs 39.39 ms ON, so this receipt proves recall neutrality, not a speedup.
Unit regression remains green: ann `test_wang_latch` **ALL PASS (T1–T8)**.

**Still open:** the pairing choice — identity (shipped; pair = adjacent cells,
live at K=64, log-compatible) vs swap-half (`(c%72)<<1 | (c/72)&1`; pair = opposite
halves, unreachable at K ≤ 72, and would
require re-deriving `lz_route.log`).

### Pairing choice — resolved for the live epoch (2026-10-24)

The fork closes here; no new derivation was needed, only the mapping functions
already recorded above were re-checked against the live configuration (K = 64).

- **identity (shipped)** — bucket `c` → cell `(pair = c>>1, end = c&1)`. For
  `c < 144` both ends of every pair are addressable, so the centre crossing
  (end flip = pair flip, one XOR via `wl_cell_cross = cell^1`) is expressible;
  the phase-2 gate held BY CONSTRUCTION at K ≤ 144, the live run is at K = 64,
  and the log is compatible. Shipped.
- **swap-half** — bucket `c` → cell `(pair = c%72, end = (c/72)&1)`. For
  `c < 72` every bucket lands on `end = 0`, so at the live K = 64 (all live
  buckets `< 72`) the end axis is degenerate and the crossing is UNREACHABLE.
  It could only engage at K > 72, and then it would require re-deriving
  `lz_route.log` anyway.

**Resolution (live epoch): identity stands.** Swap-half is parked — unreachable
at the live K and log-incompatible by construction; it stays a candidate only
for a K > 72 regime with a re-derived log. Full-frustum caveat unchanged and
still open: 2 ends × 5,440 anchors = 10,880 > 10,368 pool at full frustum scale
⇒ ends must share an id (anchor-level collapse = remaining decision).

## E. Session 4 (2026-10-25) — budget-limited A/B, reserved id, partner policy

Card `task-tcce8f5f2`. The previous A/B reported `items_scored 62,934` on BOTH
arms. That was not "the latch did nothing" — it was the FULL-SCAN fallback: a
scratch copy taken while the live index was appending pairs the index of one
moment with the anchors/order of another (`antr != nent`), the route layer
refuses the pair, and every query scores every row. Both arms were brute force,
so no gate was ever exercised. Three defects follow from that, all fixed here.

### E.1 Snapshot must be a coherent prefix, or refuse

`ann/bench_latch_ab.py::snapshot()` now reads `ntr` from the anchor header —
the number of rows the table actually covers — copies **only that prefix** of
the index, re-checks that the anchor header did not move during the copy, and
drops the `.order` sidecar unless it covers exactly that many rows (labels are
`derive` mode live, so the sidecar is not on the path). Verified live while the
index was growing: first attempt refused with
`anchors ntr=62967 != index rows=62969`, then pinned `rows=62969 K=64 dim=384
anchors_ntr=62969` and used it for all 5 runs.

The harness also carries a **VALID gate** that fails the run when the walk
degenerates into a full scan (`scored >= 0.9·N` or fallback on the majority),
so this class of silent nothing-measurement cannot be reported as a result
again.

### E.2 Reserved id 0 (the review's bucket-0 question)

`wl_id(cell,0)` for cell 0 IS `WL_RESERVED_LO` = 0, and `wl_traverse` refuses
reserved ids ⇒ **bucket 0 could never shut** (already observed 2026-10-20 via
`ANN_DOOR_TRACE`, which is why the door walk skipped it). Now
`wl_bucket_id(b) = wl_id(wl_cell_of(b), 1)` — choice 1 instead of 0: the cell
geometry (face/u/v) is untouched, every bucket is minor-latchable, and the
reserved apex is never addressed. Cell 143 → 10297 < `WL_RESERVED_HI` 10367.
A new id space is a new epoch, so the latch log tag moved `"wanglatc"` →
`"wanglatd"` (`WL_NAME_HASH`, mirrored in `scripts/p3_compact.py`): records of
the old epoch are ignored as foreign (no-op) instead of silently lighting bits
the new build never reads. Nothing is deleted, nothing is re-interpreted; a
deployment starts all-OPEN and re-learns. Receipts: `test_wang_latch` T9 (ids
in range, no reserved id, 144 distinct cells, bucket 0 latchable, 143 never the
major-output id) and T10 (G2.5 independence under the PRODUCTION ids) — ALL PASS.

### E.3 Partner policy — measured, not argued (G2.6)

The old skip test paired buckets by integer parity (`cell ^ 1`), which is a
property of k-means ids and carries no spatial meaning. `lz_pair_build()` now
derives the partner from the **centroid table** (highest cosine centroid, once
per table load, deterministic, ties → lowest id) and `ANN_PAIR` selects
`centroid` (default) | `parity` | `own`, so the choice is an arm in the A/B.
`/health` and every search response now report `fog.pair` (= K when built).

### E.4 Measurement (5 runs × 40+40 queries, pinned snapshot, budget 24,000)

Query sets: **A** = between-rows (mean of two index rows + 0.05σ noise, i.e. an
ambiguous route, not a self-query — the old set saturated at 1.0000), **B** =
near an A row (0.06σ) so it routes over A's buckets. Order matters and is the
same for every arm: the latch is process-global and engages from query 2, so
`OFF` (same sequence, warm path disabled) is the only valid baseline.

```
arm           A: scored  R@1 / R@5 / R@10   trunc   B: scored  R@1 / R@5 / R@10   warm-q   p50 B
OFF             10293     .625 / .670 / .703   0       10226    .825 / .755 / .725   0/40    13.8 ms
ON own           8882     .575 / .600 / .637  21        9810    .825 / .750 / .723   6/40    14.3 ms
ON parity        9053     .600 / .625 / .652  12       10226    .825 / .755 / .725   0/40    13.4 ms
ON centroid      9460     .600 / .645 / .665  10       10226    .825 / .755 / .725   0/40    12.9 ms
```

Identical in all 5 runs (deterministic seeding + one frozen snapshot).

**What the numbers say**

- **G2.2 (warm scores fewer): only `own` moves, by −4.1%** on block B
  (10,226 → 9,810) and −13.7% on block A — at the price of **truncating 21/40
  novel queries** and losing **R@1 −.050, R@5 −.070, R@10 −.066** on them.
  That is over-skip, measured, on ordinary traffic.
- **Pairing disables the warm path instead of widening it.** With a partner,
  every routed bucket ends up shut, `nshut == rnb`, and the branch's guard
  (skip only when *some* are shut) bails out: `warm-q 0/40`, scored identical
  to OFF. This is why G2.6 "passed" vacuously (both arms identical) and why
  the parity-vs-centroid question cannot be settled by this shape of test.
  The guard is correct (skipping all walked buckets would score nothing); it is
  simply incompatible with a partner-widened shut set.
- **Latency: no win.** 0.936×–1.039× of OFF on block B — inside noise, and the
  reason is visible in the counters: the path pays the fog/pair lookup on every
  query and then usually bails.
- **Recall loss is on the NOVEL block, not the repeat block.** Block B passes
  every tolerance; block A does not. The card's G2.3 ("novel recall unchanged")
  therefore **FAILS** as written — measured on the block the old gate never
  looked at.

### E.5 Verdict and defaults

- `lz_fog_shut` / `ANN_TOMBSTONE_WARM` stays **OFF by default** — it already is
  (env-gated, `g_warm_on = 0` unless `ANN_TOMBSTONE_WARM=1`), so the measured
  regression cannot reach live traffic. The review's proposal is adopted on
  evidence, not on caution.
- `ANN_PAIR` default = `centroid`: it is the only pairing with a defensible
  meaning, and with the warm path off it changes no behaviour at all.
- The pair table is **not** wired into the door path; the door keeps selecting
  the first routed bucket whose OWN tombstone is shut.
- To move past this, the skip rule has to become **budget-aware per bucket**
  (skip a shut bucket only while the open buckets can still fill the top-k)
  instead of the all-or-nothing `nshut < rnb` branch — that is the next
  experiment, not this one. G2.4 (isometric view receipt) remains an owner
  decision.
- Older review items still untouched: `lz_http_recv` short-read handling and
  the `strstr`-based JSON key lookup (a key match is not token-aware — a value
  containing `"vec"` or `"force"` matches too).

### E.6 Reproduction

```
cd I:/tools/ann
make latch-ab                      # 5 runs, 40 queries, refuses an inconsistent snapshot
python bench_latch_ab.py --runs 1 --queries 12 --verbose
make test                          # incl. test_wang_latch T1-T10
```
