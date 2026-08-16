---
luminaCreated: 2026-08-16T06:55:01.654Z
tags: []
luminaModified: 2026-08-16T06:55:01.654Z
luminaVersion: 1.3.11
---
# KIS Dimension — Information Gathering
## Status: Collecting, not building yet

---

## 1. What We Know (Confirmed)

### 4D Geometry Structure
```
X, Y, Z = spatial position (ตำแหน่งในพื้นที่)
W = scale (ตำแหน่งในเวลา) — temporal position indicator

NOT:
W = time (time วัดไม่ได้)
W = topology
```

### KIS Timeline Properties
```
20736 = snap point (fixed size)
- ไม่ infinite
- ไม่ predict อนาคต
- วนลูป (dodeca ↔ icosa cycle)

"Size = บอกตำแหน่งใน timeline"
- เหมือนโลกรู้ว่าปี 2026
- วัด time ไม่ได้ แต่วัด size ได้
- Size บอกว่าอยู่ตรงไหนของ timeline
```

### Architecture
```
KIS field = address space (map)
FGLS container = warehouse (data)
Pointer = lookup only (ไม่คำนวณ)

Access:
- O(1) lookup per axis
- Bird's eye view from multiple axes
- Simultaneous access
```

### Geometry Values (6ico = protagonist)
```
GEO_COMPOUND_144 (6ico):
- V=144, E=576, F=576, C=144
- "18tes" — 18-triangle tessellation
- Working field for KIS-timeline
```

---

## 2. Existing Code (in DWGLS)

### geo_param_grid.h
- GeoType enum (12 types, dodeca root family)
- GeoProps (verts, edges, faces, cells)
- GeoCodec (codebook + idx-stream)
- Roundtrip verified

### kis_codec_v6.h
- Index-based mapping: slot[i] = (i*37) % 20736
- 20736 slots (144×144)
- Stride-37 (coprime with 20736)
- Lossless roundtrip

### geo_frame_seek.h
- 1440 timeline (stride-37 walk)
- frame_enc(t) = (t*37) % 1440
- DualFrame (face, slot, phase)
- Frame-by-frame access

### geo_dual_place.h
- 162 icosahedron vertices → 64 grid
- Hilbert (border 28 cells) + Peano (inner 36 cells)
- O(1) LUT placement

### cube_on_kis.c
- f(time) → (face, x, y, z) → weight
- 6/12 face modes
- frame_seek or geo_seed routing
- z = t/1440 % 10 (depth layer)

---

## 3. Key Numbers

| Number | Meaning |
|--------|---------|
| 20736 | snap point, universal grid |
| 1440 | timeline cycle (stride-37) |
| 162 | icosahedron vertices (L2) |
| 81 | Peano grid (3⁴ ternary) |
| 64 | DiamondBlock (8×8) |
| 37 | stride (coprime with 20736, 1440, 64) |
| 144 | 6ico vertices, V5 grid |
| 576 | 6ico edges/faces |

---

## 4. Questions to Answer

1. **How does 4D (X,Y,Z,W) map to 20736?**
   - 3D = spatial (X,Y,Z)
   - W = scale (temporal position)
   - How to combine?

2. **How does 6ico (144 vertices) relate to 20736?**
   - 144 × 144 = 20736?
   - Or 144 × something else?

3. **How does loop work?**
   - dodeca ↔ icosa transition
   - Where does W (scale) change?
   - How does expand/contract happen?

4. **Container (FGLS) placement?**
   - How does FGLS sit on KIS field?
   - Where does time enter?

---

## 5. Approaches to Try

### Approach A: 3D + W as scale factor
```
X, Y, Z = spatial (0..N)
W = scale factor (0..20736)

Total slots = X × Y × Z × W
```

### Approach B: 4D hypercube on 20736
```
20736 = 12⁴ (4 dimensions of 12)
X, Y, Z, W = each 0..11

12 × 12 × 12 × 12 = 20736
```

### Approach C: 6ico × scale
```
6ico = 144 vertices
Scale = 144 (W dimension)
144 × 144 = 20736
```

### Approach D: Timeline as W
```
20736 timeline positions
W = position in timeline (0..20735)
X, Y, Z = spatial within each position
```

---

## 6. Next Steps

1. Try each approach with simple test
2. See which one fits the "no start, no end, loop" concept
3. See which one allows "bird's eye view from multiple axes"
4. See which one matches "size = temporal position"
