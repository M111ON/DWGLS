# AGENTS.md — DWGLS (4Dimension Geometry + KIS Timeline)

## 🎯 Core Architecture

### Parameterized Geometry Layer (`core/geo_param_grid.h`)

**One family: Dodeca Root** → all shapes derive from the same parent.

| GeoType | verts | edges | faces | cells | Notes |
|---------|-------|-------|-------|-------|-------|
| GEO_DODEC_BASE | 20 | 30 | 12 | 1 | dodecahedron (root) |
| GEO_ICO_BASE | 20 | 30 | 20 | 1 | icosahedron (dual) |
| GEO_COMPOUND_24 | 24 | 48 | 24 | 6 | inverted dodeca compound |
| GEO_DODEC_EDGES | 30 | 60 | 32 | 1 | edge-based |
| GEO_COMPOUND_60 | 60 | 90 | 32 | 1 | pentakis dodeca |
| GEO_PENTAKIS_72 | 72 | 90 | 32 | 1 | 12 base + 60 pyramids |
| GEO_GOLDBERG_92 | 92 | 270 | 92 | 1 | goldberg dual |
| GEO_COMP_SPIKE_120 | 120 | 180 | 62 | 1 | spike compound |
| GEO_GOLDBERG_132 | 132 | 270 | 92 | 1 | goldberg level 2 |
| **GEO_COMPOUND_144** | **144** | **576** | **576** | **144** | **★ 6ico = 18tes (protagonist)** |
| GEO_GOLDBERG_192 | 192 | 270 | 92 | 1 | goldberg level 3 |

**★ 6ico Compound (GEO_COMPOUND_144) — The Protagonist**
- V=144 · E=576 · F=576 · C=144
- "18tes" — 18-triangle tessellation field
- This is the WORKING field for KIS-timeline

**Mechanism:**
- Parameters before entry — you choose GeoType → selects shape
- `sort → distinct count → codebook size`
- `mask = how many distinct values fit in geometry vertices`
- **No hash, no lookup** — coordinate = address

### KIS-Timeline (`core/kis_codec_v4/v5/v6.h`)

**KIS = FIELD, not pipeline.**

```
∞ ← contraction ← 0 ← expansion → ∞
                    ↑
              enter anywhere
```

- **No start, no end, no zero entry point** — enter ANYWHERE
- **Forward** = expansion (spike → more vertices)
- **Backward** = contraction (seal → fewer vertices)
- **Like a balance scale** — place data anywhere on 0-20736
- **6 values same position** = 6 data points from different topology
- **Direction = value** = path data came from

**Loop transition:** dodeca ↔ icosahedron through spike vertex
```
Dodeca (12 pent) → spike → Ico-like (60 faces)
Ico (20 tri) → spike → Dodec-like (60 faces)
```

**Infinite alternation: Ico ↔ Dodec through spiking**
- Spike = operation that transforms duals (face ↔ vertex)
- h-depth: spike = h→0 (infinite resolution), sealed = h→R (finite)

### Core Principle

> **MAP not COMPRESS** — Geometry IS the address space.
> Coordinate = data. No hash, no collision, no lookup table.

## 📁 Structure

```
DWGLS/
├── core/           (16 headers)
│   ├── 4D Geometry (9)
│   │   ├── geo_param_grid.h      ← PARAMETERIZED GEOMETRY (start here)
│   │   ├── geo_dual_place.h      ← Hilbert+Peano 162→64 mapping
│   │   ├── geo_goldberg_sphere.h ← Goldberg polyhedra
│   │   ├── geo_goldberg_lut.h    ← Goldberg lookup tables
│   │   ├── geo_diamond_field_v4.h ← Diamond geometry
│   │   ├── frustum_gcfs.h        ← Frustum geometry
│   │   ├── frustum_layout_v2.h   ← Frustum layout
│   │   ├── geo_hex_layer.h       ← Hexagonal geometry
│   │   └── geo_tring_walk.h      ← Tring walk patterns
│   │
│   └── KIS Timeline (7)
│       ├── kis_codec_v4.h        ← LOSSLESS proven on real GGUF
│       ├── kis_codec_v5.h        ← v5 codecs
│       ├── kis_codec_v6.h        ← v6 codecs
│       ├── geo_adaptive_store.h  ← Adaptive storage engine
│       ├── geo_kis_container.h   ← Container format (CRC-64)
│       ├── beam_entropy_container.h ← Beam code v2
│       └── entropy_container.h   ← Entropy container
│
└── tests/          (7 files)
    ├── kis_codec_v4/v5/v6_test.c
    ├── kis_adaptive_deploy.c
    ├── kis_real_gguf_test.c
    ├── kis_map_roundtrip.c
    └── section4_seal_residual.c
```

## 🧭 Working Rules

### Geometry Constants (Sacred)
- **12**: dodecahedron base (12 faces)
- **20**: icosahedron base (20 faces)
- **24**: compound dodeca (inverted)
- **30**: edge count (both base)
- **60**: pentakis / compound-60
- **72**: pentakis-72
- **92**: goldberg-92
- **120**: spike compound
- **132/192**: goldberg levels
- **144**: 6ico compound (★ protagonist, 18tes)
- **576**: edges+faces of 6ico compound

### Coordinate = Address
- Geometry provides: mask bit per vertex (which slots used) + addressing
- No hash functions allowed for weight mapping
- No lookup tables for address resolution (LUT only for static geometry)

### Verification
- Lossless = decode → compare every value at every position
- `geo_codec_verify()` = binary truth
- Ratio < 1.0 must prove via decode (never trust encode-only)

## 📋 Session Start

1. Check `core/geo_param_grid.h` — understand current GeoType
2. Check `core/kis_codec_v4.h` — baseline codec state
3. Run `make test` (if Makefile exists) or compile tests manually

## 🔧 Build (manual)

```bash
# Tests
gcc -O2 -Wall -o tests/kis_codec_v4_test tests/kis_codec_v4_test.c -lm
./tests/kis_codec_v4_test
```
