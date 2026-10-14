# SHAPE: 8-fold Islamic Star (square + octagon scale ladder)

**Date:** 2026-10-14 · **Branch:** `feat/geo-native-fs` · **Language #10** (`octfold`)
**Source image:** regolo54 2018 — 8-fold Islamic star (square + octagon, khatam center)
**Experiment folder:** `experiments/octfold-8fold-2026-10-14/` (README, REPORT, 4 probes, evidence/)
**Status:** G1–G6 + step D ALL GREEN · `make test` 154/154 + 4/4 · `make octfold` exit 0

---

## 1. Shape identity

8-fold star built from **square + regular octagon** on the **4.8.8 (truncated-square) tiling**.
Center motif = square + 8 kites (khatam). Layers alternate square/octagon outward,
each level scaled by **√2 EXACT**, centroid invariant.

Measured from pixels (1200px image, black-mask centroid cx=597 cy=839.5):
axis rings 173, 246, 345, 482, 694 → geometric-mean ratio **1.4152** (√2 = 1.4142, err 0.07%).
Diagonal (22.5°) rings 224, 317, 448. Angular spectrum: inner r120–300 k=8, mid r300–480 k=24.
Incidence on 4.8.8 tiling: interior edge → 2 cells, interior vertex → 3 (2 squares + 1 octagon) — confirmed 2/2/1 constant.
The drawing is IMPERFECT (layer gaps, hand-drawn edges) — that imperfection is what step D quantifies.

## 2. Address law

```
addr = (n, s)      n = ring/depth (Z), s = sector 0..7
r(n) = R0 · 2^(n/2)          r(n+1)/r(n) = √2 EXACT, R0 = 173 px
type(n) = square if n even, octagon if n odd
center(n,s) = (r·cos(s·45°), r·sin(s·45°))
int form: a = (n+32)·8 + s   (depth bits | 3-bit sector), O(1) decode
fold O(1): parent(n,s) = (n−1,s), children = (n+1,s), parent(child) = id
```

Bijection cell→addr→cell 112/112 err=0, addr→cell→addr 112/112. Predicts image rings err ≤ 7px.

## 3. Ring-24 gear embed (A2)

8-fold = **STRIDE-3 decimation of ring-24**: sector s → ring tooth k = 3s (s = 3k mod 8, 3·3 = 9 ≡ 1 mod 8, unit).
Matches gear census law (count == stride): octagon stride-3. Square (4-fold) → k = 6s (stride 6).
Gear steps preserving the lattice (g ≡ 0 mod 3): {3, 6}. Radius: n→n+1 = ×√2, n→n+2 = ×2 = gear-step 2.
**2 geometric levels = 1 gear octave = 1 quadtree level.**

## 4. Quadtree comparison (B) — difference is HIERARCHY, not tiling

b₁ = E−V+1 = F for BOTH (Euler on a disk) — tiling is not the differentiator.

| metric | quadtree | this mesh |
|---|---|---|
| refinement steps | 1 (×2 only) | 2 (×√2, ×2) |
| path multiplicity to depth | 1 (forced) | Fibonacci 1,2,3,5,8,… |
| fold cost | 4 siblings coupled (34% blocked if sibling absent) | O(1) local, uncoupled |
| prune cost at depth m | exponential 4^(m−d) | linear (the ladder ray) |
| decision per step | 4 identical (0 info) | binary, real choice |
| address bits / octave | 2 | 6 (8 sectors × √2 × √2) |

## 5. Carrier grammar (the 10th language)

60 RID slots unfold sector-major with depth (w/8) ascending — the 8-fold "fan out":

```c
sector_of(w) = (3 * (w % 8)) % 8;   /* pure int, ring-24 stride-3 embed */
view = [0,8,16,24,32 … 37,45,53]    /* head [0,8,16,24,32], v[8]==3 */
count law per sector = {8,8,7,8,7,7,8,7}
```

Gates G1–G5 (`octfold_view_probe.c`, 9/9 green): bijection 60/60, inverse fwd∘inv = id = inv∘fwd,
int-only domain/range [0,60), hand head/tail oracles, stride-3 unit mod 8, sector-then-depth sort,
mutation (raw mod-8 relabel) turns suite red.

G6 real-GGUF roundtrip (`octfold_roundtrip.c`, Qwen2.5-0.5B Q8_0 675.7 MB, 5156 parts, 86 layers, 128 KB parts, ~6.5 s):
bake bad=0, readback bad=0, rebuild 644 MB byte-identical, flip@part-2578 localized + restored bad=0,
twin 676.3 MB survives destroy, corrupted-view mutation bad=172 (red ok).

Bridge (`octfold_bridge_probe.c`): bake ONCE via octfold, cross-read hosoya (stride 13) / limacon (mirror pairs) / reverse (59−p) all bad=0, XOR-consensus YES, cross-view damage localizes same part. σ_L = vwA ∘ vwB⁻¹, no re-bake, no copy.

## 6. Error tolerance (D)

Source drawing is imperfect → question is how much noise the ADDRESS LAW absorbs.

- **D1 decode basin** (two legitimate decoders, each matching theory):
  arithmetic nearest |r−r(n)|: up +20.71% ((√2−1)/2), down −14.65% ((1−1/√2)/2);
  geometric nearest |log r − log r(n)|: up +18.92% (2^(1/4)−1), down −15.91% (1−2^(−1/4));
  angular ±22.50° (45/2) exact. Cell drifts ~±16% radial / ±22.5° angular, still decodes right.
- **D2 radial erasure** (ladder is self-healing): log₂ r(n) = log₂ R0 + n/2 AFFINE (slope exactly 1/2).
  One ring erased → geometric mean of neighbours (err 1.2e-16). Whole block erased → affine fit on 2 surviving boundary rings recovers all (max err 6.6e-16).
  **Two surviving rings determine the entire ladder losslessly** — the drawing's gaps sit on this structure.
- **D3 multi-view majority**: 3 independent bijections (octfold/hosoya/limacon), smash 5 bytes in ONE view → 2-of-3 majority recovers 60/60, 0 unrecoverable. Redundancy free (address translation only).
- **MUT**: wrong ratio 1.40 → slope b=0.485 ≠ 0.5, test goes RED (proves T4a not vacuous).

Bug found while wiring: limacon map had 59 entries (1+29·2), one slot uninitialized → nondeterministic pass. Mirror-pair view `[30,31,29,…,1,0]` ends with slot 0; adding it makes 60, deterministic 3/3 runs.

## 6b. Hybrid C — quadtree coarse + octfold fine (E, `octfold_hybrid_bench.c`)

Proposal asked: does the octave-hybrid C win at depth ≤ 2 (shallow coincidence) or ≥ 8 (asymptotic → candidate #11)? Measured at equal resolution (Q depth L, O 2L half-steps, C = c coarse octaves + 2(L−c) half-steps):

- node-count crossover 4² = 8·2¹ = 16 at **depth 2 EXACT** (proposal confirmed) — but that is where raw counts meet, not where cost flips
- MEASURED leaf count = 4^L for L=0..10 (model validated, not assumed); smart region query is **sub-exponential** (L=8: 1232 visits vs 4⁸=65536) — 4^L is full-subtree worst case
- C beats the **partition (quadtree)** from L=1 (free split) / L=2 (coarse); C/Q and O/Q prune ratios → 0 as L grows → **asymptotic** (exp vs linear)
- C beats **octfold** at **no depth**: c=0 makes C ≡ O exactly (CHECK `h_prune(L,0)==o_prune(L)` for L=0..40); c≥1 only adds cost

**Verdict:** C's edge over quadtree is asymptotic (the "shallow" reading is false), but C never beats octfold — it *is* octfold plus a canonical quadtree coarse address. C is a **storage convention**, not a new language; **candidate #11 declined**. (Also caught: 32-bit `long` overflow on MinGW, 4¹⁶ wraps — all counters are `int64_t`.)

## 7. Honest scope

`octfold` is **drawing-derived constructed** (same category as pascal/hexagram #7–8), NOT independently-discovered like snubL/snubR. First view whose grammar is the **8-fold scale ladder** (√2 per level) rather than a stride/order on 60. Proven: valid 60-slot permutation passing all six gates on real 675 MB GGUF. NOT claimed: 8-fold forced by carrier (needs parity/closure argument like snub's); here it is a chosen map.

## 8. Reproduce

```sh
make octfold            # all four (probe + roundtrip + bridge + tolerance)
/c/msys64/usr/bin/make.exe octfold   # if make not in PATH
make octfold-probe      # G1–G5, no model needed
make octfold-roundtrip  # needs LLAMA_GGUF (Qwen2.5-0.5B-Instruct-Q8_0.gguf)
make octfold-bridge     # needs LLAMA_GGUF
make octfold-tolerance  # D, no model needed
make octfold-hybrid     # E, no model needed
```

Probes: `experiments/octfold-8fold-2026-10-14/octfold_{view_probe,roundtrip,bridge_probe,tolerance_probe,hybrid_bench}.c`.
Full step log: `experiments/octfold-8fold-2026-10-14/REPORT.md`. Registry row: `docs/LANGUAGES.md` #10.
Evidence crops: `experiments/octfold-8fold-2026-10-14/evidence/` (gitignored, regenerable, 12 PNG 7.6 MB).
