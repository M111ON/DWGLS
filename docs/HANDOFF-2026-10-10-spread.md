# HANDOFF 2026-10-10 (evening) — training = spread + the collapse fix

**Repo:** `I:\DWGLS-native-fs` · branch `feat/geo-native-fs` · HEAD **`9111e9f`**
**Session scope:** the S3 heat re-trial. A review corrected the first verdict
(verdict kept, reasoning fixed), ordered: count distinct buckets first,
find the spread mechanism, re-run after the fix, record training=spread.
All four done. Live systems untouched (scratch :8100/:8101 only, removed).

---

## 1. The corrected verdict (read this before the numbers)

- The first heat's verdict (S3 = cache-only) was RIGHT but for the WRONG
  reason: every derived table was collapsing to one bucket, so nothing
  about *storage strategy* had been tested at all. It tested collapse vs
  spread, and S5 won on spread.
- **7 vs 6 is noise** (N=30, single-bucket routing, 1 hit). Never cite it
  as "training buys 1 hit".
- **The seed-sweep unanimity (30/30 identical across 4 tables) is RETRACTED.**
  It is irreconcilable with routing math (different buckets must score
  different sets) and the hot-swap runs around it proved unreliable (below).
  Do not cite any number that came from a hot-swapped table. Trusted: only
  runs on freshly restarted instances (V0, V0n, S1n-fresh, V1n-fresh,
  S4n-fresh, S5p-fresh).
- **HOT-SWAP WARNING:** copying a new table over `ANN_ANCH_PATH` under a
  running server reloaded only intermittently (2/5 runs served stale clones
  with the previous table's exact numbers). Suspected route-cache
  key/torn-read interplay — NOT proven. Rule: restart per table, verify by
  health + distinct-bucket sanity, never trust a hot-swap.

## 2. Official finding: training = spread (read: scale, not quality)

- Derived rows (ac/geometric, values in [-1,1]) have norm **~11**; data and
  trained rows have norm **~0.9**. `anch_assign` compares RAW squared-L2
  with no normalization, so the norm term dominates: every query picks the
  min-norm row and the whole table routes ONE bucket. Verified by
  prediction: the collapse bucket IS the min-norm row each time
  (S1→7, V0→5, V1→7).
- Trained rows (k-means on real vecs) sit AT DATA SCALE (norm 0.9, pairwise
  cosine 0.93–0.99 — bunched, not spread!). Equal norms cancel the norm
  term, so tiny angular differences decide and queries FAN OUT (5 distinct
  buckets on 30 queries). Training buys data-scale placement, and fan-out
  follows — not "better centroids".
- Letters are innocent: 2000-anchor histogram over 52 letters is roughly
  uniform (24–53 vs ~38). The collapse lives purely in the L2-norm effect.
- Fix (`ac_normalize_table`, committed): unit-normalize all rows — any
  common scale removes the norm term. Distinct buckets after fix:
  S1 1→3, V0 1→6 (!), V1 1→4 (trained: 5).

## 3. Numbers that stand (fresh restarts, frozen 59756 snapshot, exact truth,
   BUCKETS=5/BUDGET=8000/PRUNE=1, 30 queries)

| table | top-1/30 | overlap/5 | note |
|---|---|---|---|
| S5p trained | 7 | 1.03 | control, ×3 flat |
| V0n trap-P norm | 3 | 0.47 | ×3 flat; rescued from V0 0/0.10 |
| S4n gear norm | 2 | 0.53 | |
| S1n plain norm | 0 | 0.20 | fresh (hot clone retracted) |
| V1n trap-Q norm | 1 | 0.20 | fresh (hot clone retracted) |
| V0 trap-P unnorm | 0 | 0.10 | fresh; collapse baseline |

- Order-sensitivity (V0n vs V1n, same protocol, fresh): 3v1 — present but
  small, inside the noise band. NOT the 0-vs-6 artifact (that was stale).
- Convergence: flat on every table × every round (nothing learns — by design).
- Verify rate (floor 0.5): 0/30 everywhere (tops run high here).
- Miss cost (Heat 3, this box): miss16 16.6us / hit 0.58us / head+fp ~1.4us;
  break-even hit rate ~32%; 99% hits ≈ 15x vs active16. Per-pass DISK writes
  for head/fp unmeasured (could dominate — open item).
- S3 vs S1 verdict NOW: V0n (3) > S1n (0) fresh — but N=30 noise band.
  S3-production: NOT proven (needs N≫30). S3-direction: ALIVE (leads the
  derived tier + cheapest + least redo). Neither crown nor kill.

## 4. Why normalization helped only halfway (honest limit)

Normalization fixed the collapse (spread 1→6) but recall went 6→3 vs the
unnormalized accident (which always walked the biggest bucket 7 — where
truth concentrates). Query-dependent buckets under integer label-matching
against TRAINED-assigned entry labels is still a lottery: the walk matches
`entry.anchor == rb` across two different partitions. Real fix = re-label
entries under the new table (re-anchor the index: 59,756 × 8 × 384 L2 ops,
seconds in C) — designed, NOT built. That is the completion of "make the
table matter".

## 5. Commits this session (DWGLS repo, feat/geo-native-fs)

- `364fe4b` rung capture 64/16/4/1 + centre full cell (test 24/24)
- `00befc1` bond-link token/carriers/join (test 9/9)
- `35bec8b` passive trap (test 9/9)
- `cafadc9` peano-overlay + inversion experiment (7/7)
- `56026a2` anchor arena (shared columns)
- `ced96ee` heat tools (trap tables V0–V3, S1 seedmode)
- `9111e9f` spread fix (normalize + norm flags)
- tools repo `10716dc` pruned walk + fallback (default OFF, live untouched)

## 6. Next session (ordered)

1. **Re-label experiment**: assign every index entry's anchor under a V0n
   table (C, minutes) → rerun walk recall. If recall jumps toward trained
   tier, the label lottery is confirmed as THE wall and tables become first-
   class routers. If not, the partition itself is at fault.
2. **Bigger-N heat** (200+ queries): settle S3-vs-S1 and order-sensitivity
   outside the noise band. Fresh restart per table, no hot-swap, exact
   frozen truth (protocol in §3).
3. **Miss-write measurement**: per-pass head/fp disk cost (fsync reality)
   before any production talk about traps.
4. Merge-minors note #16 stays PARKED (owner: เอาไว้ก่อน).
