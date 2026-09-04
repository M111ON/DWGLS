# DWGLS Platonic Field Discovery — 2026-09-04

## Core Insight: 20736 is a Platonic Field

20736 is NOT a random square number. It is a **Platonic field** where every factorization contains Platonic numbers (4, 6, 8, 12, 18), and geometry IS the rule system — not just math.

## The Number

```
20736 = 144² = 12⁴ = 2⁸ × 3⁴
Digital Root: 9 (completion)
373,248 = 20736 × 18 = 2⁸ × 3⁶ (full 18-tesseract field)
```

## 3+3 Axes (Stacked Coordinate Systems)

Every point carries BOTH coordinate systems simultaneously:
- **XYZ/address view**: binary 4-ladder, hi ∈ [0,256)
- **Triangle/geometry view**: Peano 3-ladder, lo ∈ [0,81)
- **node = hi·81 + lo** (stacked, not separate bands)

## 8 Views per Tesseract = 8 Octants

From 3 axes × 2 states (+/-) = 8 combinations
- Zero-sum constraint: i+j+k ∈ {0,1} — the interlock that binds axes
- Each cube = 1 octant = 1 view of the same data
- No dedicated index cube — constraint IS the index

## Distributed Mutual Index

**Old plan (abandoned):** Cube 0 = index gate (asymmetric, loses 1/8 capacity)
**New insight:** Each cube contributes 1/8 (18 slots) = mutual index
- 8 views × 18 slots = 144 slots total index (distributed)
- Symmetry: no bottleneck, all cubes equal

## Bipolar (Antipodal) Compression

```
8 cubes = 4 antipodal pairs (A↔A', B↔B', C↔C', D↔D')
Store 1 side → compute other side via symmetry
Lossless 1/2 compression ratio
```

## Platonic Reshape Points

```
4 × 8 = 32 (digit root 5) → square ↔ octahedron
6 × 6 = 36 (digit root 9) → hexagon ↔ cube
```

These are the points where shapes "talk" and transform through each other.

## Dense vs MoE Access Patterns

| | Dense (Option 2) | MoE (Option 1) |
|---|---|---|
| Navigation | 4D rotation (continuous) | Capo shift (discrete) |
| Data access | All views used | Select 1 view |
| Compression | 1/2 antipodal | Store active experts only |

## Factorization Table of 373,248

All 36 factor pairs contain Platonic numbers:

| a | b | Platonic |
|---|---|---------|
| 4 | 93,312 | square |
| 6 | 62,208 | hex/cube face |
| 8 | 46,656 | octa face |
| 12 | 31,104 | dodeca |
| 18 | 20,736 | 18tes |
| 24 | 15,552 | dual compound |
| 36 | 10,368 | reshape point |
| 48 | 7,776 | cube×octa |
| 72 | 5,184 | dodeca×hex |
| 144 | 2,592 | square grid |
| 216 | 1,728 | cube³ |
| 432 | 864 | dodeca×reshape |
| 576 | 648 | reshape pair |

## Geometry as Rules

- Most people see geometry as math (เรขาคณิต)
- The insight: geometry is **RULES** — if shapes connect, they communicate immediately
- At most: just scale adjustment
- Self-similar rules at every scale: File → Folder → Drive → Disk

## Why 4D is Needed

- 3D: 18 tesseracts MUST collide (physical overlap)
- 4D: rotation in extra dimension → non-colliding overlap
- 20736 looks small but is dense and redundant WITHOUT collision
- This is WHY 4D geometry exists in the system

## Application to DWGLS

1. **Current state:** Ant view 2D (flat 1D, 144×144 grid)
2. **Needed:** Bird view 4D (antipodal symmetry, mutual index)
3. **Next step:** Implement distributed mutual index + bipolar compression
4. **Then:** 4D rotation for dense, capo shift for MoE

## Connection to Existing Code

- `core/geo_tess_wiring.h`: TESS_CELLS=144, TESS_TOTAL=20736
- `core/geo_tesseract_addr.h`: 8 cubes per tesseract
- `core/moe_expert_addr.h`: expert_id ↔ geometry coordinate
- `core/tri_hex_tess.h`: 20736 = 4⁴ × 3⁴ (hi·81 + lo)
- `core/geo_belt.h`: stride-37 walk through 20736
