# Birthday paradox POC → where it can plug into DWGLS

Date: 2026-11-25 · experiment dir: `experiments/birthday_paradox-2026-11-24/`
· run: `experiment_two_level_field.py` → `experiment_two_level_field.json`, `run_two_level_field.log`

## 0. What the experiment actually is

Two synthetic ideas, both about **bucketing under a budget**:

1. **Birthday scale.** A state space of size M starts colliding at ~√M items (single-sided:
   `1.1774·√M`). So if you bucket N items into K buckets, the efficient K is
   `K* ≈ √(b·N)` (route cost `c_r·K` vs scan cost `c_s·b·N/K`, with `c_s≈c_r`).
   The meet-in-the-middle half (`2k ≈ 1.665·√S`) does **not** transfer — retrieval has no
   reverse walk toward an answer it doesn't know.

2. **Light cone.** Split a candidate set into three: **timelike** (inside the budget,
   deterministic, cheap), **overflow** (inside the radius but too costly), **spacelike**
   (outside the radius, prunable in O(1)). Measure marginal gain per unit cost and stop at
   the knee. The POC's "saved X% compute" is **not evidence** — `required_cycles` was drawn
   from `np.random.exponential`, so the saving is a property of that generator, not of any system.

## 1. Where it does NOT apply (state this first)

- **Tesspack storage (`core/tess_resolver.h`, 20736 cube, stride-37).** This is a bijection:
  `cell = (slot × 37) % 20736`, `gcd(37,20736)=1`. Collision-free by construction
  (test_tess_index_frame 7/7, test_cube_wire IDENTICAL). Birthday is about *collisions in a
  bucketed map*; a bijection has none. Do not paste birthday numbers onto the 20736 cube.
- **ANN (`I:\tools\ann`).** Birthday *was* measured there on the live 54,527-vector index
  (K=64 → 13,176 rows for recall .95; K=256 → 7,491, −43%). That is a separate system with
  its own routing layer (anchor doors, topb=2 roster). It is prior art for the *method*, not the
  answer to "where in DWGLS".

## 2. Where it DOES apply in DWGLS (the routing/addressing layer)

DWGLS's routing is not the 20736 cube. It is the layered address space above it:
Goldberg tiles (per level) + Tring fibers (dim) + Diamond shells (per chunk). These *are*
bucketed maps, so birthday and the light cone apply.

### 2.1 Goldberg tile space — capacity ceilings

`gp_face_count(n) = 10n² + 2`, n = 1..8 (GP_MAX_LEVEL 8, dim 0..7):

| level n | tiles | birthday capacity √tiles |
|---|---|---|
| 1 | 12 | 3.5 |
| 2 | 42 | 6.5 |
| 3 | 92 | 9.6 |
| 4 | 162 | 12.7 |
| 5 | 252 | 15.9 |
| 6 | 362 | 19.0 |
| 7 | 492 | 22.2 |
| 8 | 642 | 25.3 |

Total tile-instances across 8 levels = 2,056 → collision-free capacity ≈ √2056 ≈ 45 items.

So a Goldberg level can coordinate at most ~25 distinct items before collision; beyond that you
must go up a level (more tiles) or use the Tring fiber. **This is the plug-in point:** the
current design fixes `gp_level` per file ("Fixed gp_level per file — uniform sphere"), so a
large file saturates its tile space silently. Birthday gives a concrete capacity gate:
`if items_in_level ≥ 0.5·√(10n²+2) → step n up (or spill to dim+1)`.

### 2.2 Tring dim — the collision multiplier (connects to #7530)

`tick = (tile_id << 8) | dim`, dim 0..7. Tring is the perpendicular fiber: same tile_id,
different dim = different dimension. Birthday: 8 dims multiply tile space by 8, capacity by √8
≈ 2.83. So adding dimensions is not just "more depth" — it is the mechanism that keeps the
tile space collision-free as item count grows. This is the same arithmetic as the POC's
"3 planes" (ceil(N/20736)) but at the Goldberg layer, and it is *why* the tesseract dimension
exists (#7530: "collision is guaranteed, so hide the payload in another dimension").

### 2.3 Diamond shell — capacity gate on the classifier

`shell_slots(n) = (2n+1)³`, n = 0..8, total 13,041:

| level n | slots | birthday capacity √slots |
|---|---|---|
| 0 | 1 | 1.0 |
| 1 | 27 | 5.2 |
| 2 | 125 | 11.2 |
| 3 | 343 | 18.5 |
| 4 | 729 | 27.0 |
| 5 | 1331 | 36.5 |
| 6 | 2197 | 46.9 |
| 7 | 3375 | 58.1 |
| 8 | 4913 | 70.1 |

Current classifier (`shell_classify_level` in `geo_diamond_field_v4.h`) routes a 64-byte chunk
to a level by `SCORE_THRESH` (entropy-ish score only). It never checks how many chunks already
occupy that level. **Plug-in:** add a capacity gate — if `occupancy[level] ≥ 0.5·√slots[level]`,
route to `level+1`. This prevents a level from becoming a collision hotspot and is a ~5-line
change to `shell_classify_level` plus an occupancy counter in `Shell`.

### 2.4 Batch grouping — the light cone maps directly

`BATCH_L1=8, L2=64, L3=512`, `BATCH_MAX_DIFF=12` (max differing bytes from base).

Birthday collision count per group: √8=2.8, √64=8, √512=22.6. The diff cap (12) is the
**light-cone budget**: a chunk within ≤12 diffs of the base is timelike (absorb into base,
cheap); beyond 12 is spacelike (new base). The current 12 is a fixed guess.

**Plug-in:** run the POC's marginal-gain measurement on real chunk batches — plot recall of
"base still represents the chunk" vs diff cost, find the knee. The method is already proven on
the ANN index (§1 above); porting it to `test_field_focus.c`-style batches would replace the
fixed 12 with a measured knee. This is the most actionable of the four.

## 3. Proposed next steps (none implemented)

1. Add occupancy counter + capacity gate to `Shell`/`shell_classify_level` (2.3) — smallest,
   testable in `test_field_focus.c`.
2. Measure the batch diff-cap knee on real chunks (2.4) — replaces a magic 12 with a number.
3. Make `gp_level` per-file choice birthday-aware (2.1) — currently fixed; capacity gate prevents
   silent saturation.
4. Leave the 20736 cube and ANN alone — they are correct as-is and out of scope.

## 4. Not verified

- No measurement on real DWGLS data: all numbers above are structural capacity arguments
  (slots, tiles, batch sizes), not measured collision rates.
- The diamond field and Goldberg sphere are used in tests, not the production tesspack serve path;
  check `test_field_focus.c` and `test_geo_diamond_map.c` before assuming the hot path.
- Batch diff-cap knee requires a real chunk corpus and a representativeness metric; not defined yet.
