# Octagram + Beam Addressing — Parked Research Track
**Date:** 2026-09-05
**Status:** PARKED — proven viable, not priority
**Blocked by:** contour gauge roundtrip not proven lossless, current pipeline has higher-priority fixes

---

## History

Beam addressing was the ORIGINAL approach for DWGLS weight addressing:
- weight → (zone, position) → 3D cell via O(1) formula
- 200M+ ops/sec in C
- Contour gauge: data enters field → bounces off boundaries → leaves fingerprint (delta)
- Reconstruct from fingerprint using clock (tick/step/frame_seek)

It was killed (collection/beam_addressing/ = 128 files) because:
1. **Permutation cost > sort savings** (NET -11 bits per block)
2. **Roundtrip lossy** (0/100 PASS, ratio 0.59×)
3. **Ghost cells** (projection overlap in 3D cube)

## Octagram Revival

### What Octagram fixes
The Octagram (D4 dihedral group on 3×3 grid) constrains permutation to 8 forms:
- 32-value block → 3 groups of 9 + 5 remainder
- Each group: log₂(8) = 3 bits form index (vs log₂(9!) = 20.4 bits full perm)
- Total perm cost: 3×3 + 5×8 = 49 bits (vs 117 bits full perm)
- NET: +57 bits (PROFIT vs old -11 LOSS)

### What's proven (all tests PASS)
1. D4 group closure: 8×8 = 64 compositions, all in group
2. D4 forms are bijections on 3×3 grid
3. Algebraic transforms preserve magic sum (shift/scale/affine)
4. Center flexibility: center≠5 still 6-7/8 lines balanced
5. Roundtrip encode→decode → sorted values: 8/8 PASS
6. Gap creation: D4-sorted = 6.75× more gaps than plain sort

### Octagram connection to existing DWGLS
- Voronoi mask (24 cells × 864) = compound of 3 octahedra (3×8 faces)
- Octagram 8 forms = 1 octahedron (8 faces)
- Beam wave (beam_wave.c) = XOR-based encoding on dual square
- FiboSpine 12-tick = clock for on-the-fly computation
- Frame seek (stride-37) = contour path through address space

## Why parked
1. Current pipeline (tesspack bake→graft→inference) works, 11/11 PASS
2. Pack order ≠ layer order (1.83 vs 2.45 tok/s) is immediate fix
3. DLL no_alloc bug blocks inference
4. Contour gauge roundtrip = theory only, not proven lossless
5. Ghost cell fix = theory only (D4 gaps help but not proven)

## Resume checklist
- [ ] Prove contour gauge roundtrip lossless (small 32-value test)
- [ ] Prove D4 gaps reduce ghost cells in beam projection
- [ ] Compare Octagram contour vs tess_stride_scatter on real weights
- [ ] If all pass: integrate as alternative scatter in tesspack pipeline

## Key files
- collection/beam_addressing/ — full beam system (128 files)
- tools/beam_cost_probe.c — permutation cost on real Q8_0
- core/geo_voronoi_mask.h — 24-cell Voronoi (compound of 3 octahedra)
- core/iso_rot90.h — D4 operations (rot90/mirror)
- beam_wave.c — XOR wave encoding on dual square 360×360
