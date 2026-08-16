---
luminaCreated: 2026-08-16T06:55:01.320Z
tags: []
luminaModified: 2026-08-16T06:55:01.320Z
luminaVersion: 1.3.11
---
# Cube-in-Dodecahedron Implementation Summary

## Files Created

### `core/geo_cube_in_dodeca.h`
Single source of truth for cube-in-dodecahedron mapping.

**Key Features:**
- 20 dodecahedron vertices derived from standard construction
- 8 cube vertices (indices 0-7) embedded in dodeca structure
- 3 axes × 2 signs = 6 half-axes = DiamondBlock 6 faces
- 8 cell types from parity (2³)
- φ connection verified: cube_edge = φ × pentagon_edge

**Constants:**
- `PHI` = 1.6180339887498948482
- `INV_PHI` = 1/φ ≈ 0.618
- `INV_PHI2` = 1/φ² ≈ 0.382

**Functions:**
- `dodeca_vertex(idx)` — get dodeca vertex by index
- `cube_vertex(idx)` — get cube vertex by index (0-7)
- `is_cube_vertex(idx)` — check if index is a cube vertex
- `cell_type_from_parity(nx, ny, nz)` — get cell type from generation parity
- `phi_ratio()` — verify φ connection
- `cube_address_to_xyz(n, k)` — map (n, k) to 3D position
- `half_axis_center(axis, sign, n)` — get center of half-axis at generation n

### `tests/test_cube_in_dodeca.c`
Comprehensive test suite (8 tests):
1. Cube vertices are subset of dodeca vertices ✓
2. φ ratio = 1.618034 (error 2.22e-16) ✓
3. 6 half-axes = 6 faces ✓
4. Cell type parity mapping ✓
5. Address mapping n=0 gives unit cube ✓
6. Generation scaling by φ ✓
7. Half-axis center X+ at n=0 = (1,0,0) ✓
8. Pentagon edge = 1.236068 ✓

## Geometry Facts Verified

| Property | Value | Notes |
|----------|-------|-------|
| Dodecahedron vertices | 20 | Standard construction |
| Cube vertices | 8 | Subset of dodeca (indices 0-7) |
| Pentagon edge | 1.236068 | Distance between adjacent face vertices |
| Cube edge | 2.0 | Distance between adjacent cube vertices |
| φ ratio | 1.618034 | cube_edge / pentagon_edge |
| Half-axes | 6 | 3 axes × 2 signs = DiamondBlock faces |
| Cell types | 8 | 2³ from parity (nx%2, ny%2, nz%2) |

## Addressing System

**Structure:** (n, k) where:
- n = generation (layer number)
- k = face/vertex ID (0-19 for dodeca, 0-7 for cube)

**Scaling:** Generation n scales by φⁿ

**Cell Types (from parity):**
```
(0,0,0) → (i,i,i)  — all icosa
(0,0,1) → (i,i,d)  — 2 icosa, 1 dodeca
(0,1,0) → (i,d,i)
(0,1,1) → (i,d,d)
(1,0,0) → (d,i,i)
(1,0,1) → (d,i,d)
(1,1,0) → (d,d,i)
(1,1,1) → (d,d,d)  — all dodeca
```

## Next Steps

1. **Connect to DiamondBlock**
   - 6 half-axes = 6 faces
   - Store n (generation) in 64-bit per face

2. **Test with GGUF**
   - Place weights on cube-in-dodeca structure
   - Measure performance

3. **Extend to 6ico compound (GEO_COMPOUND_144)**
   - Use cube-in-dodeca as building block
   - 144 vertices = 6 × 24 = 6 × (8 cube + 12 golden rect + 4?)
