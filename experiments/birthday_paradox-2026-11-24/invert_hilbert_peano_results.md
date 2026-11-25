# Invert-Hilbert + Peano: experiment results

Date: 2026-11-25 · script: `invert_hilbert_peano.py` · results: `invert_hilbert_peano_results.json`

## What was tested

User's idea: Hilbert/Peano walks are deterministic (always the same path). Instead of storing the full walk
(position → index), store only the **inverse** (index → position) for **occupied cells**. Combine
horizontal invert-Hilbert with vertical Peano. Tested on empty/sparse grid fields.

## Results

### 1. Both curves are perfect bijections (verified)

| Curve | Grid | Cells | chi² vs uniform | Roundtrip |
|---|---|---|---|---|
| Hilbert | 8×8 | 64 | 0.0 (perfect) | OK |
| Peano | 9×9 | 81 | 0.0 (perfect) | OK |

### 2. Inverse lookup is 25× faster than forward computation

| Method | 64 cells | Speedup |
|---|---|---|
| Forward (compute Hilbert) | 49.8 µs | — |
| Inverse (array lookup) | 2.0 µs | **25×** |

This is the core of the "invert" idea: compute forward on the fly (deterministic), store inverse
(sparse) for hot-path O(1) lookup.

### 3. Sparse inverse storage saves 81–97% at low occupancy

| Occupancy | Sparse (8 B/cell) | Full (4 B/cell) | Savings |
|---|---|---|---|
| 1% | 8 B | 256 B | 96.9% |
| 5% | 24 B | 256 B | 90.6% |
| 10% | 48 B | 256 B | 81.2% |
| 50% | 256 B | 256 B | 0% (break-even) |

**Plug-in point:** for any sparse field (empty grid, few occupied cells), store sparse inverse
instead of full array. Break-even at ~50% occupancy.

### 4. XOR combination has high collision (63%) — not suitable as unique address

| Method | Unique (of 259 occupied) | Collisions |
|---|---|---|
| XOR (Hilbert 6-bit ^ Peano 7-bit) | 96 | 163 (63%) |
| **Interleave** (Hilbert 6 \| Peano 7<<6) | **259** | **0** |
| Hilbert-only | 64 | 195 |
| Peano-only | 78 | 181 |

**Conclusion:** XOR of two curve indices collapses (small result space). **Interleaving** gives
zero collisions and is the better combination method. XOR is still useful as a *collision detector*
(double-hashing: an item colliding under Hilbert likely won't under Peano's different pattern).

### 5. Cross-dimensional adjacency agreement: 76.8%

Of 56 Hilbert-adjacent cell pairs, 43 (76.8%) are also Peano-close (|index diff| ≤ 3).

The 23.2% disagreement marks **curve seams** — boundaries where the two curves disagree on
locality. These are detectable and could be handled specially (e.g., boundary-aware routing).

## Summary: what works and what doesn't

| Idea | Verdict | Numbers |
|---|---|---|
| Store inverse (sparse) for occupied cells | ✅ Works | 81–97% savings at ≤10% occ, 25× faster lookup |
| XOR of two curves as unique address | ❌ Fails | 63% collision |
| Interleave of two curves as unique address | ✅ Works | 0% collision |
| XOR as collision detector (double-hash) | ✅ Works (in principle) | Different collision patterns |
| Cross-dim seam detection | ✅ Works | 23.2% seams detectable |

## Proposed DWGLS integration

1. **Sparse fields (empty grids, low occupancy):** store sparse inverse of Hilbert/Peano index.
   Compute forward on the fly. 81–97% storage savings, 25× faster lookup.
2. **Cross-dimensional addressing:** use Hilbert for one dim, Peano for another. Interleave (not XOR)
   for unique addresses. The 23.2% seam rate gives a natural boundary detector.
3. **Double-hashing for collision resolution:** if an item collides under Hilbert, try Peano
   (different collision pattern → likely no collision). Standard cuckoo-style resolution.
4. **Do NOT use XOR as a single address** — it collapses the space.

## Not verified

- Tested on 8×8 and 9×9 grids only. Larger grids (72×72) tested on sparse subset.
- Peano implementation verified by roundtrip but not against a reference implementation.
- No test on actual DWGLS data (weights, chunks) — only synthetic grids.
- The "seam" concept (23.2% disagreement) is measured but its utility for routing is untested.
