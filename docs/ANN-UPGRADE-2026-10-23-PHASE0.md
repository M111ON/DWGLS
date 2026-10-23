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
