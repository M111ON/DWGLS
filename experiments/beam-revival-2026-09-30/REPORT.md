# Beam Addressing Revival — Full Campaign Report (2026-09-30)

**Status: KILL as compression. RICH as infrastructure.**
One-line verdict: every complete end-to-end accounting exceeds raw
(2.256x / ~3.3x / 8.00 b/val). But the campaign leaves proven keepers,
a recovered design doc, two upgraded tools, and five concrete open threads.

Model under test: `Qwen2.5-0.5B-Instruct-Q8_0` (630M Q8 values, real bytes).
All probes: `experiments/beam-revival-2026-09-30/*.c` (14 files, out of build path).
Keepers in tree: `core/mm_wang6.h`, `core/geo_lblock6.h`,
`tests/test_mm_wang6.c`, `tests/test_lblock6.c` (GEO_FAST, green).
Memories: #7037 (twin/carry + sector-0 corollary), #7038 (end-to-end verdict).

---

## 1. Starting point

Revived the dropped Beam Addressing plan against prior art in
`collection/beam_addressing/` (HANDOFF 2026-07-23: beam_value C impl,
beam_timer, 148–217x benchmark; Next steps never done: real GGUF weights,
6-direction verification). Old tests re-ran green in minutes
(test_beam_value PASS, 7bit 6/6, geometric ALL PASS) — but on synthetic data.
First honest finding: on real GGUF, `beam_field_test` maps 30/30 yet the
"field" encoding is 18 B/weight vs ~1 B for Q8_0. The night's rule was set:
**gross numbers without restore cost are deceptive.**

## 2. Phase 1 — Sort + Sector (PASS on clustering, wall identified)

`beam_sector_probe`: 630M values sorted low→high, split by leading digit
(sectors 1–9 + sector 0). Lossless multiset OK; sorted stream collapses to
~200 runs (int8 has 256 levels); sector 0 = 0.76% (4.77M values, free).
`beam_cost_probe` (pre-existing) quantified the wall the plan had hit years ago:
sort saves ~105.8 b/block, permutation costs ~117.1 b/block →
**net −11.3 b/block on all 3 models**. Without the cost: 244.8→139 b/block
(~43% saving) — that 43% is the prize Phase 2 was supposed to unlock.

**Sector-1 anomaly (79 vs 22 runs), resolved**: not data structure — int8
range geometry. Only leading digit 1 reaches the hundreds in [−128,127]:
sector 1 = ±{1,10–19,100–127} + (−128) = 79; sectors 2–9 = ±{d,d0–d9} = 22.
A Benford-of-range artifact. Pure-2-9 blocks: **48 of 12M (0.0%)** — values
0/1 interleave every block, so sector separation at block granularity is
impossible; only global streams qualify (where the permutation wall waits).

## 3. Cube sequence + tick + wall-XOR (BUILT, lossless)

`beam_cube_seq`: sector streams → 10³-cube sequence ((10³)N à la frame_seek)
with plain incrementing tick (no fibo) + wall-XOR layer
(H(i)=(i·37)%1000, P(i)=(i·91)%1000, key=H^P — fixed, invertible;
10 is neither 2ⁿ nor 3ⁿ so true Hilbert/Peano can't tile it, strides stand in).
Roundtrip OK on 630,095,872 values, 630,100 ticks.

## 4. Angular zigzag radius (BUILT, lossless)

`beam_zigzag_probe`: 2 spheres × 3 axes (A=positive, B=negative),
angle=(r·137+tick)%360 with snake-direction alternation, r carried in the
pack (never cut — the #882 lesson). Roundtrip OK on 2M real values, bad=0.
`beam_cube_place` (diagonal readout d,d,axis): lossless but collapses to
30/300 cells with chains to 7224 — placement without compression.
Rewritten as **Hilbert-field placement** (tick→cube, H=(tick·37)%1000):
200K values → 200 cubes, **0 collisions** — position-on-pattern kills chains.
`beam_angular360` (36 positions × 10 digit-units, the user's 6×6→360 frame):
lossless, 113/360 addresses used (bounded by 129 |v|-levels, sign folded).

## 5. Phase 2 — permutation via D4/orbits (KILL as specified)

`beam_orbit_probe`: octahedral group (24 rotations + inversion = 48, covering
the plan's non-standard 36). Sanity OK (48 orientations → 1 orbit).
Real stream cubes: **500/500 distinct orbits — zero collapse**.
Per the plan's own kill criterion (≥8x), Phase 2 as written is dead:
raw cubes carry no symmetry to harvest.

## 6. Occurrence index — the decisive experiment (PERM REAL)

Question: two 5s — same beam, different offsets. Can order derive from beam?
`beam_occurrence_probe`: beam6(v) (sign, axis, r, r2, r3, digit, all f(v)) has
**255 distinct values for 200K inputs** (pigeonhole: f(1 byte) ≤ 256 outcomes;
avg ~784 sharers per identity, ~1.7M at full scale). Ascending-order playback
with no stored order: **1207/200000 offsets match (0.60%)**.
Per agreement: **order must be stored**. Length-at-position IS the value
(renaming 8 bits doesn't shrink them); occupancy bitmaps restore lane-per-
position but not which-of-167M-members (the ~26 b/val within-lane perm).

## 7. Fold/hourglass side-quest (PREMISE CONFIRMED, compression still no)

- `beam_fold_zero`: weights ARE x:−x symmetric — **96.53% foldable**
  (214M pairs; −128 orphan 39K, orphan-by-design). 0.514x gross **excludes
  the pairing map** — honest total needs it (§8).
- `beam_fold12` chains on real data: ÷12 (7.75→5.90→4.02→2.19, top 0),
  ÷4→81 (7.62→…→4.69), ÷3→256 (7.54→…→4.99). Residuals never vanish
  (max 24/25 at meeting sizes) — **that remainder is the carry for the Boom**.
- 256×81 meeting verified (÷3-side →256, ÷4-side →81; ÷12 chain
  20736→1728→144→12→1, pipes appearing on their own).
- Hyperbolic push (−12 both sides) compounds: local cancellation grows per
  round (/12: 5.4%→16.6%→36.1%→**50%**). Refusing −12 (positive-only) kills
  the bonus by construction (localpairs=0, means converge to +40 not 0).
- One pyramid read three ways (÷3/÷4/÷12 → tops 256/81/1): Boom = readings
  agree = built-in triple-check; any round dropping its carry shows up.
- Shrink 20736→144 one-shot (mean buckets): exact 0.76%, mean_err 43.9 —
  maze-as-same-map by brute shrink is lossy; gradual folds only.
- `beam_carry_xfield`: carries crossing 12→8 field — local coords: 9% orphan
  + **97% wrong-home**; global coords: 100% survive. Value never lost; the
  LINK dies. Cross-boundary rule: same addressing rules or re-anchor at the
  wang gate with a rule tag (park, don't enter wrong home).

## 8. The binding theorem (why every door closed)

Restoring exact order of N items costs log2(N!) ≈ N·log2(N/e) bits in EVERY
representation tried (Lehmer/block, pairs+positions, lanes+within-lane perms,
bitmaps, run boundaries). With 8-bit values vs ~13.6-bit positions (N=32000),
reorder-then-restore can never go below raw — information-theoretic, not an
implementation gap. Local pairing can't dodge it either (adjacent pairs 0.53%,
within-32 15.8% — the symmetry is global, positions scattered).
**Folding yields organization + verifiability, never fewer bytes.**

## 9. Keepers and recoveries (the actual profit)

1. `core/mm_wang6.h` — 6-direction Wang for 3D maze cells (canonical face
   keys; 2700/2700 internal faces open by construction; colors spread
   685–742; borders shut). Old wang was 4-dir 2D only.
2. `core/geo_lblock6.h` — L-Block in 3D: real Skilling Hilbert d→xyz
   (512/512 coverage, 511/511 unit steps — first Butz attempt failed 175/511
   and was replaced), entry-direction → 6-orientations, wired to wang6:
   Hilbert-path walk 511/511 gates open, 0 orientation mismatches.
3. `core/geo_lblock.h` / bridge re-verified (17/17 + 10/10).
4. EHCP design doc recovered (`I:/FGLS_new/collection/docs/EHCP_Topo_Design_v1.txt`,
   POC 34/34): WARP complement ≡ twin fold, ghost ≡ sector-0 no-delete,
   circuit-switch + GeoFace Router semantics for the DB layer.
5. Island codec re-run on real model: LOSSLESS 21M blocks / 0 FAIL (principle
   holds — small fields CAN be forced), 1.03x size (its "51.7% zeros" comment
   was a misread of the 51.6% positive split; real zeros 0.76%).
   27648 = 4608×6 = 2¹⁰×3³ noted for the 6-direction reading.
6. Hilbert tool upgraded (`F:/Artifact/space_filling_curves_engine.html`):
   click A/B pins (rotation-proof path indices), Δidx + Euclid + Locality.
7. `docs`: 360 = 36 positions × 10 digit-units (6 faces × 6 vertices × Lo Shu+void)
   frame recorded and probed (113/360 used — bounded, not broken).

## 10. Open threads (concrete next steps)

- **B. `geo_maze_seek.h`** — consolidate frame_seek + wang6 + lblock6 into one
  core header. Safe, useful regardless of compression verdict. (User: will use.)
- **Tamper states for wang6** — TAMPER/BREAK/DROP don't exist yet; gates are
  binary. Gives fail-loud + localization + containment (currently silent pass
  on color-matching corruption).
- **EHCP/DB reconnect** — WARP-gate + ghost + circuit-switch flows as the
  foundation of several current subsystems; built for DB originally.
- **BFS→L2 wiring** — drive the existing 20736³ L2 address space from the BFS
  seeker (2 axes) instead of tesseract stacking. MAP infrastructure, not squeeze.
- **Unproven escapes** (only things that could still beat raw): canonical-order
  consumer, field-generated structural zeros, lossy.
- Awaiting user: the 36000→300 fold rule (which cells → which of 10×10×3) —
  needs the angular-zigzag-first derivation; and the readout decision.
