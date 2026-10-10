# HANDOFF 2026-10-10 — full session (morning → night)

**DWGLS repo:** `I:\DWGLS-native-fs` · `feat/geo-native-fs` · HEAD **`b6bddbc`** (clean)
**Tools repo:** `I:\tools` · my commits `10716dc` (walk) + `5db346e`
(inject builder); live binaries untouched (built to `build/ab/`, removed).
**Earlier handoffs today:** `docs/HANDOFF-2026-10-10-rdh-anchor.md` (morning),
`docs/HANDOFF-2026-10-10-spread.md` + addendum (evening investigation).
This doc = the whole arc + owner's rulings + what's next.

---

## 1. How the session tangled (read first)

The session opened with "is our anchor done?" — which dragged ann
internals (routing/recall/table heats) into what should have been KV work.
Owner's ruling, end of day: **ann and KV are separate matters.**
ann = index over the main memory warehouse (59,756 entries of stuff +
histories, always has data; training accepted since creation, never in
question). KV = the only undone part: starts EMPTY, never trains, temporal
storage like RAM — receives, DEPOSITS to the warehouse, holds only the
address (ticket book, not store). ann receives from KV as a background
cold path; live waits for neither. Merging training-free tables onto
trained-labeled entries was impossible by construction (integer matching =
home game for trained) — accepting one-time training was correct.

## 2. What got built (DWGLS, all tested, all committed)

- Anchor geometry per owner's 3×3 drawing: 9 cells via ±offset overlay,
  centre letter with pair-fold (Aa same pair) + own [##], capture at rungs
  64/16/4/1, letter = bond↔tombstone link token, passive trap (register
  empty ~15ns, first catch paints, later verify ~0.5–1μs, bond-heads +
  fingerprints, no pool.py change needed). Tests 24/24, 18/18, 9/9, 9/9.
- Experiments: Peano-9 chain overlay (order changes aggregate 64/64,
  hamming ~32, letter flips 63/64; sum control blind), inversion
  (fold16(bitrev) == nibble_rev exactly; weights hold, letters flip 53/64).
- Arena S1–S5 on shared columns; S3 heat tools (trap tables V0–V3).
- Spread fix: `ac_normalize_table` (unit rows).
- KV side: index-section builder (the #7527 seam), round-boundary anchors
  (turn-1 % 144, KVT_ROUND split from KVT_WINDOW).

## 3. Findings that stand (each measured, each reproducible)

- **Training = manifold alignment** (official): trained rows cos(data-mean)
  0.97–0.99 inside the cap; derived rows orthogonal (~0). Spread is its
  symptom. Letters innocent (52-hist uniform).
- **Min-norm collapse** (bug, fixed): RAW L2 + rows norm ~11 vs data ~0.9
  → every query takes the min-norm row (predicted S1→7, V0→5, V1→7 every
  time). Fix = normalize. Distinct buckets after fix: S1 1→3, V0 1→6.
- **Spread ≠ recall**: Jpc spreads 8 buckets, recalls 1/30. Recall needs
  LABEL correspondence (entries carry trained integers) — the re-label
  experiment is THE next step with predicted payoff.
- **Injection**: rows = normalize(mean + 0.17·axis) doubles trap recall
  (big-N 210q: Jpc 25/199 vs V0n 13/199) but trails trained 96/199 (6σ+).
- **Miss costs** (this box): miss16 16.6μs / hit 0.58μs / head+fp ~1.4μs;
  break-even hit rate ~32%. Per-pass DISK writes unmeasured (could dominate).
- **Retracted, do not cite**: seed-sweep unanimity, all hot-swap runs,
  7v6 ("noise"), S3==S1 equality (degenerate collapse).
- **Permanent rules**: restart per table (hot-swap intermittent 2/5 stale);
  exact frozen truth; N≥200 before believing recall deltas.

## 4. Holes in normal ann (owner-reviewed, memory #7563)

- **H1 silent unroute on growth** (OBSERVED 2×): antr≠nent → brute scan,
  recall intact, latency climbs, nothing alerts. Now DOWNGRADED by the
  closed-cluster model (memory #7565): regrow moves major root only,
  vectors untouchable, failure box-by-box → worst case is stale window.
  Fix = cold-path watcher (auto rebuild+retrain+swap + truthful alert).
- **H2 table/label version pair coupled only by ntr** (K unchecked;
  re-anchor w/o retrain = silently wrong subsets). Code-verified.
- **H3 model-swap scale fragility** (no norm check on route path). Latent.
- Non-holes: trained-path recall never wrong; arena numbers implicate
  nothing live.

## 5. Owner's architecture (end of day, memories #7560–#7565)

- Major = vertical (descent + chain, red axis); minor = horizontal only
  (bond walks, outbound, no retreat). Expansion only through passed layers.
- One frustum shared by a family (pair), separate tombstone pillars each.
- Closed clusters: append-new + search-old only; failure box-by-box;
  capo moves don't touch vectors; tombstones pin at front doors only.
- Trap/bond-head design already consistent with all of the above.

## 6. Live state at handoff

ann :8096 ok / 59756 / routed:1, embed ok (verified). OOM incident ~evening:
6 stray llama-servers + 3 dead ann_search (none listening) killed; pool,
gateway, MCP servers untouched (not ours); restored via keepalive --once.
Scratch :8100/:8101 removed; `build/ab/` empty; temp files removed.
11 unpushed commits exist from earlier sessions (owner knows).

## 7. Next (ordered by owner)

1. **Cold-path watcher** (H1 fix): mtime/count → rebuild+retrain+atomic
   swap + alert. Only live-touching job on the list.
2. **Re-label experiment**: entries' anchors under Jpc/V0n table → walk
   recall (predicted jump; decides whether tables become first-class).
3. **Big-N after re-label** (200+/3 corpora, restart per table).
4. **A–F arena revisit** after routing parity.
5. PARKED (do not open): merge minors (#16: shared frustum, separate
   pillars constraint), minor tag system (#17), miss-write disk cost.
