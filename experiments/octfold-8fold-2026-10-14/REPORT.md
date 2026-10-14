# DWGLS — 8-fold Ladder: Address Law + Quadtree Comparison
date: 2026-10-14
source image: regolo54 2018 (8-fold Islamic star, square+octagon)

## STEP 1-3 (base) — PASSED
- fold ladder: square <-> diamond(octagon), ratio per level = sqrt(2) EXACT
- centroid invariant across all levels
- image rings measured: axis 173,246,345,482,694 -> gmean ratio 1.4152 (sqrt2=1.4142, 0.07%)
- incidence 2/2/1 confirmed on 4.8.8 tiling: interior edge->2 cells, interior vertex->3 (2 squares + 1 octagon)

## A) ADDRESS LAW — PASSED
addr = (n, s):  n=depth(Z), s=sector(0..7)
  r(n) = R0 * 2^(n/2)          r(n+1)/r(n) = sqrt(2)
  type(n) = square if n even, octagon if n odd
  center(n,s) = (r cos(s*45), r sin(s*45))
- bijection: cell->addr->cell 112/112 err=0 ; addr->cell->addr 112/112
- predicts image rings (err <= 7px on 1200px)
- fold O(1): parent(n,s)=(n-1,s); children=(n+1,s); parent(child)=id
- closed-form int: a=(n+32)*8+s  (n bits | 3 bits sector), O(1) decode

## A2) EMBED INTO RING-24 GEAR — PASSED
- 8-fold = STRIDE-3 decimation of ring-24 :  sector s -> ring k=3s
- matches gear census law (count == stride): octagon stride-3 == our 8-fold octagon
- square(4-fold) -> ring k=6s (stride 6)
- gear steps preserving 8-fold lattice (g = 0 mod 3): {3, 6}
- radius ladder: n->n+1 = x sqrt2 ; n->n+2 = x2 = gear-step 2
- => 2 geometric levels = 1 gear octave = 1 quadtree level

## B) QUADTREE vs MESH — the difference is in the HIERARCHY not the tiling
NOTE (correction): cycle-rank b1 = E-V+1 = F for BOTH (Euler on a disk). The tiling is NOT
the differentiator; the refinement hierarchy is.

| metric                    | quadtree        | this mesh              |
|---------------------------|-----------------|------------------------|
| refinement steps          | 1 (x2 only)     | 2 (x sqrt2, x2)        |
| path multiplicity to depth| 1 (forced)      | Fibonacci 1,2,3,5,8,.. |
| fold cost                 | 4 siblings, coupled (34% blocked if sibling absent) | O(1) local, uncoupled |
| prune cost at depth m     | exponential 4^(m-d) | linear (the ladder ray) |
| decision per step         | 4 identical (0 info) | binary, real choice  |
| address bits / octave     | 2               | 6  (8 sectors x sqrt2 x sqrt2) |

## C) G6 — REAL-GGUF ROUNDTRIP — PASSED
`tools/octfold_view_probe.c` (G1-G5) + `tools/octfold_roundtrip.c` (G6):
- model: Qwen2.5-0.5B-Instruct-Q8_0.gguf · 675.7 MB · 5156 parts · 86 layers
- G1 view bijection 60/60 = YES
- R1 BAKE+READ  5156 parts · bake bad=0 · readback bad=0 (lossless)
- R2 REBUILD   644 MB rebuilt · byte-identical = YES
- R3 DAMAGE    flip@part 2578 -> localized=YES -> restored bad=0 (lossless again)
- R4 PERSIST   twin 676.3 MB survives destroy
- MUT corrupted view -> readback bad=172 (red ok)
- wall time ~6.5 s

## C2) MULTI-VIEW CARRIER (bake once, serve all) — PASSED
`tools/octfold_bridge_probe.c`: bake ONCE via octfold, cross-read by pure
address translation sigma = vwA o vwB^-1 (no re-bake, no copy):
- cross-read via hosoya  bad=0 · XOR-consensus=YES
- cross-read via limacon bad=0 · XOR-consensus=YES
- cross-read via reverse bad=0 · XOR-consensus=YES
- damage through cross-view ordering -> localized same part -> restore once
=> octfold is a first-class 10th view on the 60-slot carrier

## D) ERROR TOLERANCE — PASSED
`octfold_tolerance_probe.c` — the source drawing is IMPERFECT (layers have gaps,
hand-drawn edges), so the real question is how much noise the ADDRESS LAW absorbs.

D1 GEOMETRIC MARGIN (decode basin) — two legitimate decoders, each matching theory:
- arithmetic nearest |r-r(n)|: up +20.71% (=(sqrt2-1)/2) · down -14.65% (=(1-1/sqrt2)/2)
- geometric  nearest |log r-log r(n)|: up +18.92% (=2^(1/4)-1) · down -15.91% (=1-2^(-1/4))
- angular margin: +-22.50 deg (45/2), exact
=> a cell can drift ~+-16% radially / +-22.5 deg angularly and still decode right.

D2 RADIAL ERASURE (why the ladder is self-healing)
- log2 r(n) = log2 R0 + n/2 is AFFINE in n (slope exactly 1/2)
- single interior ring erased -> geometric mean of neighbours recovers it (err 1.2e-16)
- whole block erased -> affine fit on the 2 surviving boundary rings recovers all
  (max rel err 6.6e-16)
=> TWO surviving rings determine the entire ladder losslessly. The drawing's "gaps"
   are exactly the structure this law sits on.

D3 MULTI-VIEW MAJORITY (error tolerance by view redundancy)
- 3 independent bijections (octfold/hosoya/limacon) over the same 60-slot carrier
- smash 5 bytes in ONE view -> 2-of-3 majority recovers 60/60 lossless, 0 unrecoverable
=> redundancy is free (address translation only, no copy)

MUT: wrong ratio (1.40 instead of sqrt2) -> affine slope test goes RED (b=0.485 != 0.5)

Bug found & fixed while wiring: the limacon map in the probe had 59 entries
(1 + 29*2), leaving one slot uninitialized -> nondeterministic pass. The mirror-pair
view `[30,31,29,...,1,0]` ends with slot 0; adding it makes 60 and the suite is
deterministic (verified 3/3 runs).

## STATUS vs GATE "gear = language"  (all six)
| gate | evidence | status |
|---|---|---|
| G1 bijection | probe bitset 60/60 | DONE |
| G2 inverse   | fwd o inv = id = inv o fwd | DONE |
| G3 int-only  | sector_of = (3*(w%8))%8, no float | DONE |
| G4 oracle    | hand head/tail + count law {8,8,7,8,7,7,8,7} + 3 unit mod 8 | DONE |
| G5 mutation  | raw mod-8 relabel -> red | DONE |
| G6 GGUF      | 5156 parts lossless, byte-identical rebuild | DONE |

## HONEST STATUS
- octfold is a **drawing-derived constructed** view (same category as pascal/
  hexagram, languages 7-8), NOT an independently-discovered one like the snub
  enantiomorph pair. Its value: it is the first view whose grammar is the
  8-fold **scale ladder** (sqrt2 per level) rather than a stride/order on 60.
- what is genuinely proven: it is a valid permutation of the 60-slot carrier
  that passes all six LANGUAGES.md gates on a real 675 MB GGUF.
- what is NOT claimed: that the 8-fold star is *forced* by the carrier (that
  would need a parity/closure argument like snub's); here it is a chosen map.
