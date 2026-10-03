# GLS-HEXPENT record — 2026-10-03 (HexPent flower lineage, root `gls_`)

Owner-built GeoGebra construction: recursive tri→pent→tri growth, 4 triangle
generations + pentagon rings + arc families + inter-gen joints. Status:
**design exploration (parked)** — spec + acceptance tests ready, no code.
Naming memory: #7172 (`gls_` family root).

## Sources (inputs, never edited)

- `C:\Users\Administrator.AVENTADOR\Downloads\tri_semi_pent.html` — protocol,
  base side 27.71 (2-decimal display)
- `...\tri_semi_pent.ggb` — same construction, full doubles (90 elements)
- `...\geogebra-export.ggb` — redrawn clean, base side exactly 20,
  medial derived from arc intersections (103 elements)
- `...\HexPent_flower2.ggb` — gen-3 pentagons + gen-4 triangle + mixed-gen
  intersections + polygon-overlap flower points + echo triangles (205 elements)

## Construction rule (the deliverable — rules, not numbers)

```text
gls gen(n): medial(n) (= arc intersections = midpoints, Thales theorem)
  -> regular pentagons outward on each medial edge (CCW winding)
  -> semicircles on triangle sides (CCW) + CircleArcs (center,from,to CCW)
  -> 2 outer pentagon vertices pin on semicircles (residual 0), 1 free apex
  -> 3 free apexes = triangle gen(n+1)
gen(n+1) = R(-60 deg) x r x gen(n) about fixed center O (concentric, in place)
```

Generations (side): 20 -> 21.6535 -> 23.4437 -> 25.3819860998 (predicted
digit-for-digit BEFORE drawing, confirmed after). Next: s5 ≈ 27.4805,
spokes {30,150,270}.

## gls_ invariants (accepted, measured exact)

| # | Name | Content | Receipt |
|---|------|---------|---------|
| 1 | gls_2plus1 | per pentagon: 2 pinned + 1 free; frees form next triangle | 18/18 hits ≤1.2e-13 over 3 gens |
| 2 | gls_r_exact | growth ratio r = (√(15+6√5)−1)/4 = 1.082676064001459 | 15/15 digits vs coords; derived, not fitted |
| 3 | gls_inplace | shared center O fixed, scale only, zero translation | centroid equal 12 decimals ×3 gens |
| 4 | gls_chiral_ccw | medial + pentagons + arcs all CCW | signed areas +, arc calibration 32.686/37.26 |
| 5 | gls_alias_joint | outer apex coincides with a free vertex (S=K, J_1=H_1, I_2=D_2, K_1=B_1, triple H_1=W_1=J_1) | ≤2.4e-13 each, 3/3 gens |
| 6 | gls_arcratio | arc long/short = 1.140308 (angle-pure, all gens); shared R/gen, R ladder = r | 12 arcs, R 12.052725 -> 13.049197 |
| 7 | gls_spokes | 18 exact spokes: 6 main (30+60k°) + 12 sidebands (±9.3088474785°) | 38/38 on-spoke, offsets equal 12 decimals |
| 8 | gls_pimeter | semicircle length = s×π/2 (s=2 reads π, s=20 reads 10π) | 4/4 gens incl. 34.013271688845, 36.8253551158909 |
| 9 | gls_intergen | mixed circle-intersections form ~70.4°/109.6° rhombi joining free apexes across gens (K-O_1-P_1-H_1 etc., side 15.931725) | 3 rhombi; angles/side measured, forms open |
| 10 | gls_dualread | curves-only (21 conicparts) and polys-only (19 polygons) each carry full 6-fold symmetry | census + screenshots |

Rhombi census: 12 total — 9 lattice diamonds (60°/120°, sides 10 / 10.8268 /
11.7219 = medial edges) + 3 inter-gen (70.400838°/109.599162°).
Centroid O = (0, 2.226497308104). Triangle spokes alternate {30,150,270} /
{90,210,330} per gen (orientation flips apex-down/up).

## Rejected (do not reopen without new evidence)

- Spiral: centroids coincide → homothety, rotation 0. FAIL decisively.
- DeepSeek closed forms: 5√3/8 off 1.4e-4; φ/√(φ+2) = 0.8507 not 1.08276
  (arithmetic error); √(φ²/√5) off 6.3e-4; 57/50 off 3.1e-4 + "19 = edge
  count" false (30); Reuleaux wrong mechanism; 1.1404 was display rounding
  of exact 1.140308.
- φ/dodeca labels, "30" and π-meter as load-bearing: shimmers only.
- Angular address as a requirement: rejected by owner — spokes stay an
  observation, not an address proposal.

## Addendum — pair symmetry (2026-10-03, verified in HexPent_flower2.ggb)

- Chain point set (rule-following only): 120°-invariant 0 misses, centroid
  = O digit-for-digit → balance about O is exact at every level (each
  triangle/medial already centroids at O). Mechanism is 120° triplets,
  NOT 180° antipodal pairs (0/54 antipodal in full set).
- What the up+down PAIR adds: directional completeness 3 spokes -> 6
  (hexagram reading the eye recognizes); radii still alternate by r, so
  strictly D3, visually 6-fold. Name: pair-completeness (observation).
- Full set breaks exact symmetry at exactly 3 points: S_1 (mixed),
  R_2 + S_2 (poly∩poly flower) — exploration extras, not chain.
- Dead commands: 4× `Intersect(g_3,r_3,1)` (K_2,L_2,M_2,O_2) yield
  NaN (segment misses arc) — cleanup candidates, no geometry.

## Addendum — gls_regrow doctrine (2026-10-03, owner verdict)

- gls structures are HARD (exact, predicted-ahead ×3) but NOT long-stable:
  fp path-spread (~1e-12/gen), object-count lag (205 elements), exploration
  cruft breaks symmetry (3 points) — all grow with run length.
- Rule: NEVER persist grown state; REGENERATE from seed per use via closed
  form gen(n) = R(−60n°)·r^n·G0 (O(1), no iteration, no accumulation).
  Fits frequent-regrow points only; long release without renew is NOT OK.
- Natural consumer: ephemeral coordinate paths (#6365) — emit per query,
  discard after use. Adapter work stays parked until a non-size payoff
  is named.

- 9.3088474785° closed form; 15.931725 closed form; 70.400838° closed form;
  arc sweep angles θ (112.135°/127.865°) radical forms (likely none clean).
- Mixed/flower/echo points: positions recorded, no rules yet (owner to name).
- Quantization decision (angle→18 slots is integer-ready; radius→gen index
  integer-ready; Euclidean lengths stay out) — required before any code.

## Implement points (candidates, all parked)

- P1 test gate (required first by #7067): `tests/test_gls_hexpent.c`,
  integer-only (spoke microdegrees, ring indices, 2+1 membership, alias
  pairs, orientation signs as cross products, r as convergent bounds);
  wire into `GEO_FAST` (Makefile:213) mirroring `test_net_walk`
  (Makefile:43, `tests/test_net_walk.c:2` BUILD pattern).
- P2 `core/tri_hex_tess.h`: `th_pentagon` (L60, 0–11) + `th_shell` (L68)
  vs gls 18 spokes × rings — GAP: 12 pentagons ≠ 18 spokes, do not force.
- P3 `core/geo_net_walk.h`: `nw_build_fan` (L131) / `nw_walk` (L39) for
  gls_alias_joint + gls_intergen as glued entries (needs Euler check).
- P4 `core/scale_bridge.h`: `sbr_w_to_scale` (L62) / `sbr_scale_to_ring`
  (L95) for gls_inplace readout — GAP: gls r (1.082676) ≠ semitone
  2^(1/12) (1.059463), separate ladders, do not conflate.
- P5 `core/geo_param_grid.h`: GeoType TRI↔PENT as generator (today: selector).
- Scope guard: touch nothing in codec/container/serve/weights. No component
  swap implied by any drawing (#7111). No visualization artifacts (#7112).
