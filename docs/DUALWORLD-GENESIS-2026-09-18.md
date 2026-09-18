# Dual-World Genesis — 2026-09-18 (part 2, geometry arc)

Companion to HYBRID-GATE-DOCTRINE (part 1, gate arc). Owner narrated the full
first-principles construction; every number pinned in tests/.

## 1. Genesis (the whole story in one line)

Square world (xyz, base-2) x triangle world (ijk, base-3)
-> tesseract 1152 -> pentagon angles -> 12 -> 144 -> field 20736.
Nothing asserted; everything derived on a calculator before implementing.

## 2. Dual-world chain (test_dualworld.c, 15/15)

- xyz: squares, 6 dirs = 3 pairs (+-), base-2: 16/32/2^5 (why base-2 works)
- ijk: triangles, 4 dirs unique (no parallels), base-3: 27
- 3x6=18 (18tes), 6x6x8x4=1152 (one tesseract from both worlds)
- 27x32=864, 864/8=108 (pentagon interior)
- Owner intent, closed loop: 180-72=108, 108-72=36, 108+36=144,
  144/2=72 (regenerates!), 108/2=54. Fixed point, not a line.
  (First version 360-108 was 252: wrong, corrected per owner.)
- {36,54,72,108} = pentagon angle family (golden vertex/base, half/full interior)
- 32-27=5 (pentagon number), 6+6=8+4=12 -> 12^2=144
- Structure: octa holds a square plane; 3-octa compound = XYZ axes;
  tetra dice rolls flush on triangle tessellation (face-to-face lock-in)

## 3. Scale = H of the triangle (test_tri_h.c, 10/10)

- Single knob: side s -> (2H)^2 = 3s^2 (integers): s=2 -> 12, s=6 -> 108,
  s=12 -> 3x144. Doubling side quadruples everything.
- HOME: side 144 = 12^2, H = 72sqrt3, area 20736U exact.
  W axis (scale_bridge) IS H moving (W=12 -> side 72, area 5184).
- H=12 resonance: (2H)^2 = 576 (6ico E+F), area 192U (goldberg-192).
  12 is the resonant operating point: geometry measures itself in system
  integers there, irrationals elsewhere.

## 4. Digit ladder (test_digit9.c, 17/17)

- Doubling chain 1152->2304->4608->9216 squares: dsums 18,27,27,45...
  all digital root 9. 1152 = 18x64 (back to 18).
- RAIL (+9 spacing, forced: 9Z-closed under x2) vs VISITS (chaotic order,
  pairs early, random-walk later). Verified to k=3000 squares.
- HOLE at 36 (and 9): never visited in 3000 doublings. Rungs have WINDOWS:
  36 only livable at few digits (window closed unvisited); 360 first hit
  at k=130 (85 digits, avg 4.24 ~ random expectation).
- 36 = 12x3 = the AXIS value (structural, not transient): the train never
  stops at its own rails. /3 ladder: 6,9,15,18,...,51 (+3 steady) with the
  SAME hole scaled (missing 12). Scale-invariant hole (#1002).
- 27 = 3^3 (base-3 signature on a base-2 chain).

## 5. Translator rule (principle)

Rung-hopping with holes suits TRANSLATOR/TRANSITION/CONVERTER mechanisms,
not the seeker (seeker keeps full coverage via stride-37 bijection).
Rungs translate at multiples-of-9 boundaries; axis values (12/36) are
translation-invariant FIXED POINTS (12 is 12 in both worlds).
Counterparts: scale_bridge.h (W-teeth, W=0 identity anchor),
tess_assemble/gguf_pack (format converters), moe_expert_addr.
(#1003)

## 6. Figures pinned (skill: geometric-pattern-integration)

- test_twin_figure.c 10/10: 2 views/1 field/1 axis (kis/hyper circles)
- test_gate_weave.c 8/8: E control point over A-E-B (open/close/isolate)
- test_tri_subdiv.c 8/8: 4^n / triangular numbers / 81 = Peano depth-2
- test_trap63.c 8/8: 3+3 alternation (opposites differ, 2nd-neighbors equal)
- test_hexring4.c 8/8: hexagram-core ring = 4U = 4-subdivision whole
- test_hexred.c 8/8: vertex-spoke ring 2/9 vs 3/9 U (ninths), triples 6n/9n
- test_tetstack.c 8/8: T+D+4O = n^3 (octet truss conservation)
- Full zone-map drawing (side/hex-radius/corner numbers): PENDING owner
  (test_zone_map stubbed on those 3 numbers — not guessed per #105)

## 7. Standing principles referenced

#993 watch for intrinsic-scope fixes en route. #994 cost ledger, depth by
stakes. #995 cage economics improve with size. #996 onion growth earned.
#997 crust relocation (model-free/model-bound). #998 sign-holder identity.
#999 numbers as handles (never renumber). #1000 calculator-verifiable core
vs measured behavior. #1001 theorems-with-our-numbers lookup first.
#1002 scale-invariant hole.
