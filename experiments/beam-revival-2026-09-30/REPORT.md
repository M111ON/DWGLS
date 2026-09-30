# Beam Revival Campaign — 2026-09-30 (REPORT)

Question: can Beam Addressing (sort → sectors → cubes → angular address) compress
Q8_0 weights losslessly below raw? Model: Qwen2.5-0.5B-Instruct-Q8_0 (real data).

## Verdict

**Beam-as-compression: KILL.** Every complete accounting exceeds raw.
MAP-not-COMPRESS confirmed with receipts (memory #7038).

| Test | Result |
|---|---|
| Sort saves / perm costs (beam_cost_probe) | +105.8 b/blk vs −117.1 → **net −11.3 b/blk** (3 models) |
| True end-to-end w/ pairing map | **2.256x (expands)** — positions 76% of cost |
| Nine-lane (values+interleave, w/o within-lane perm) | 0.376x gross → **~3.3x true** |
| Occurrence probe (beam6 distinct / ascending playback) | 255 identities / **0.60% offsets match → PERM REAL** |
| D4 orbit collapse (500 stream cubes) | **500/500 orbits → Phase 2 as-specified dead** |
| Shrink 20736→144 (mean buckets) | exact 0.76%, mean_err 43.9 → lossy |
| Carry across field boundary (12→8) | local: 9% orphan + 97% wrong-home; global: 100% survive |

## Positive findings (keep)

- Weights ARE x:−x symmetric: **96.53% foldable** (−128 orphan 39K). Hourglass premise holds.
- Sector-1 anomaly (79 vs 22 runs) = int8 range artifact, not data structure.
- Pure-2-9 blocks: 0.0% (sectors interleave everywhere). Local pairs ≤15.8%.
- Fold chains ÷12/÷4/÷3: residuals shrink per round (7.75→2.19), tops →0.
- Hyperbolic push compounds: local cancellation grows per round (/12: 5%→50%).
- 256×81 meeting (÷3-side →256, ÷4-side →81) verified; ÷12 chain 20736→1728→144→12→1.
- New keepers: `core/mm_wang6.h` (6-dir Wang, 2700/2700 faces open), `core/geo_lblock6.h`
  (3D Hilbert via Skilling decode, 512/512 + 511/511 unit steps), Hilbert-path gate
  walk 511/511 open. Tests wired in GEO_FAST.
- EHCP design doc recovered at `I:/FGLS_new/collection/docs/EHCP_Topo_Design_v1.txt`
  (WARP complement ≡ twin fold; ghost ≡ sector-0 no-delete).
- Hilbert tool (`F:/Artifact/space_filling_curves_engine.html`) upgraded: click A/B
  pins, Δidx + Euclid + Locality readout.

## Theorem (binding constraint)

Restoring exact order of N items costs log2(N!) bits in ANY representation
(Lehmer, pairs+positions, lanes, bitmaps). 8-bit values < ~13.6-bit positions
(N=32000) → reorder-then-restore can never go below raw. Open doors (unproven):
canonical-order consumer, field-generated structural zeros, lossy.

## Files

- Probes (archived, out of build path): `experiments/beam-revival-2026-09-30/*.c` (14 files)
- Keepers: `core/mm_wang6.h`, `core/geo_lblock6.h`, `tests/test_mm_wang6.c`,
  `tests/test_lblock6.c` (+Makefile GEO_FAST entries)
- Prior art used: `collection/beam_addressing/` (HANDOFF 2026-07-23, island codec
  LOSSLESS 21M/0 FAIL but 1.03x — sign-split misread, not zeros)
