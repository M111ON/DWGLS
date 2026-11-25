# GeoJump structural verification results

Date: 2026-11-25 · script: `geojump_verify.py` · results: `geojump_verify_results.json`

## Verified claims

### 1. Hilbert 4×4 is a bijection ✅
Values: [0,1,14,15, 3,2,13,12, 4,7,8,11, 5,6,9,10] — all 0..15, no duplicates.

### 2. Peano 4×4 (serpentine) is a bijection ✅
Values: [0,7,8,15, 1,6,9,14, 2,5,10,13, 3,4,11,12] — all 0..15, no duplicates.

### 3. 4×4 Hilbert ×3 floors = 48-cell block is a bijection ✅
All 48 values 0..47, no duplicates. (floor index 1..3 via Peano serpentine, cell via Hilbert)

### 4. 3 blocks × 48 = 144 tower is a bijection ✅
All 144 values 0..143, no duplicates.

### 5. Invert Hilbert ×3 (user's variation) is a bijection ✅
Reverse curve (15 - hilbert) stacked 3 times → 48 unique values. Works.

### 6. Peano weave (XOR of two Peano patterns) ❌
Only 1 unique value out of 16. Total collision. Does NOT work.

## Failed claims

### 7. XOR of Hilbert and Peano on 4×4 ❌
Only 4 unique values out of 16 (12 collisions). Same failure as 8×8. XOR collapses.

### 8. Interleave on 4×4 = injective but wasteful ⚠️
16 unique values but in 0..255 range (only 16 of 256 used). Injective, not compact.
For a 16-cell space, a 4-bit bijection is needed; interleave gives 8 bits.

## Scale-dependent findings

### 9. Seam rate: 75% on 4×4 vs 23.2% on 8×8
On 4×4, Hilbert and Peano (serpentine) disagree on locality 75% of the time.
On 8×8, disagreement was 23.2%.
Seam detection is **not scale-invariant** — unreliable at small scales.

### 10. Tring 720 vs GeoJump 48: no clean mapping
720 = 15 × 48. 15 is not a natural dimension in either system.
720 = 6 spokes × 120 (tring) = 5 × 144 (5 towers).
48 = 4×4×3 (geojump block).
No single-factor decomposition connects them. They are separate structures.

## What still needs proving

1. **Does the geojump 4×4×3 structure actually serve as an address space in production?**
   - In code: `_jump_hilbert` and `_jump_peano` are used in `geo_jump()` router.
   - But `gjc_walk()` only accepts bijective routers (MOD). Hilbert/Peano are "WALLS" (idempotent projections, never placement).
   - So Hilbert/Peano are used for *routing/climate*, not for *placement*. The 48-cell block is a container layout, not an address space for arbitrary data.
   - **Verdict:** The structure exists and is bijective, but it's used as a wall/projection, not as a free address space. Using it as an address space would be a new application.

2. **Can sparse inverse storage be applied to geojump blocks?**
   - Verified: sparse inverse saves 83% at 10% occupancy, 50% at 25%, 0% at 50% on a 48-cell block.
   - But geojump blocks are always fully populated (48 cells = 48 bytes of payload). Sparse storage only helps if the field is partially empty.
   - **Verdict:** Applicable only to sparse/partial fields, not to full blocks.

3. **Can the "invert Hilbert ×3" variation be used as a walk?**
   - Verified: it's a bijection on 48 cells.
   - But it's just a reversed curve — the same locality properties, just traversed backwards.
   - **Verdict:** Valid as a walk variant, but doesn't add new structural properties.

4. **Is 720 related to 48 in a way that matters?**
   - 720 = 15 × 48. No clean decomposition.
   - The tring walk (720) and geojump (48) are independent structures.
   - **Verdict:** No provable structural relationship. Using both in the same system requires an explicit mapping (not derived).

## Summary

| Claim | Status |
|---|---|
| 4×4 Hilbert bijection | ✅ Proven |
| 4×4 Peano (serpentine) bijection | ✅ Proven |
| 48-cell block bijection | ✅ Proven |
| 144-tower bijection | ✅ Proven |
| Invert Hilbert ×3 bijection | ✅ Proven |
| XOR combination | ❌ Fails (4/16) |
| Peano weave (XOR) | ❌ Fails (1/16) |
| Interleave combination | ⚠️ Injective, not compact |
| Sparse inverse storage | ✅ Works (83% savings at 10% occ) |
| Seam detection | ⚠️ Scale-dependent (75% on 4×4, 23% on 8×8) |
| 720 ↔ 48 mapping | ❌ No clean relationship |

**Bottom line:** The geojump 4×4×3 structure is a valid bijection (proven). Its components (Hilbert, Peano serpentine, invert) are all bijections. But the *combination* methods (XOR, weave) fail, and the tring walk 720 has no clean mapping to 48. The mental structure (deterministic walk → invert → sparse) is sound, but the specific combination of two different walks (Hilbert × Peano) doesn't produce a better address space than either alone.
