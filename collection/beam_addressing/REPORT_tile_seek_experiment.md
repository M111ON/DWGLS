# Beam Tile Seek + Hilbert Maze — Experiment Report

**Date:** 2026-07-27
**Session:** Tessellation → Frame Seek → Hilbert Maze → Evolve

---

## 1. Objective

พัฒนาระบบ codec ที่ใช้ tessellation structure (20736 nodes) และ frame seek (1440 tiles) เพื่อเก็บค่าน้ำหนักแบบ deterministic โดยไม่ compression

**หลักการ:** "Coordinate IS data" — น้ำหนักไม่ถูกเก็บเป็นตัวเลข แต่ถูกกู้คืนจากพิกัดเชิงเรขาคณิต

---

## 2. Architecture

### 2.1 Tessellation Structure (from geo_jump.h)
```
GEO_FULL = 20736 = 12 pentagons × 1728
1728 = 12 shells × 144 (GEO_TOWER)
144 = 3 blocks × 48 (GEO_BLOCK)
48 = 3 floors × 16 cells (GEO_METATRON_CELLS)
16 = 4×4 metatron grid
```

**Decompose:**
- face = node / 1728 (0..11)
- shell = (node / 144) % 12 (0..11)
- local = node % 144 (0..143)

**Tower decomposition:**
- tower = node / 144 (0..143)
- block = (node % 144) / 48 (0..2) — 3 vertices ของ triangle
- floor = ((node % 144) % 48) / 16 (0..2)
- cell = (node % 144) % 16 (0..15)

### 2.2 Frame Seek (from geo_frame_seek.h)
```
FRAME_CYCLE = 1440
FRAME_STRIDE = 37
frame_enc(t) = (t × 37) % 1440
```

### 2.3 Hilbert Curve (from GEOMATRIX V5)
```
3D Hilbert: d2xyz(n, d) maps distance d → [x,y,z] in n×n×n cube
n=4 → 4×4×4 = 64 positions
Inverse: LUT-based (64 entries)
```

### 2.4 Icosahedron (Frequency-4)
```
162 vertices on sphere
Combined with Hilbert: (64 × 2) × 162 = 20736
= geo_jump structure
```

### 2.5 Evolve (from fusion_visualizer.html)
```
Cellular automaton on Hilbert curve
Structure stays still, data moves
Ternary states: 0, 1, 2
Evolution: neighbor average → state change
```

---

## 3. Experiments

### 3.1 beam_position_codec.c — Weight → Frame Seek Position

**Approach:** weight → frame_enc(w + 128) → position on 1440 grid

**Results:**
- Basic roundtrip: 256/256 PASS ✓
- Block: 18/32 PASS
- Real model: 69/3200 exact (NOT lossless)
- Size: 45 bytes/block = 1.32× Q8_0

**Issue:** Bit packing truncation at 11 bits

### 3.2 beam_tess_codec.c — Tessellation Position

**Approach:** weight → tessellation node_id (face, shell, local)

**Results:**
- Decompose/Recompose: 20736/20736 PASS ✓
- Collision: 0 — 256/256 unique nodes ✓
- Roundtrip: 256/256 PASS ✓
- Block: 31/32 PASS
- Real model: 3174/3200 exact (NOT lossless)
- Size: 65 bytes/block = 1.91× Q8_0

**Issue:** 16 bits per weight (face=4, shell=4, local=8) — BIGGER than Q8_0

### 3.3 beam_tile_seek.c — Tessellation → Frame Seek

**Approach:** weight → tessellation node → clock_tick → frame_seek tile

**Results:**
- Collision: 0 — 256/256 unique tiles ✓
- Roundtrip: 256/256 PASS ✓
- Block: 32/32 PASS ✓
- Real model: 3161/3200 exact (NOT lossless)
- Size: 45 bytes/block = 1.32× Q8_0

**Insight:** Tessellation → clock_tick → frame_seek ทำงานได้ดี แต่ size ใหญ่กว่า Q8_0

### 3.4 beam_gosper_seek.c — Gosper Curve

**Approach:** weight → tessellation node → Gosper L2 → frame_seek tile

**Results:**
- Collision: 207 — 49/256 unique tiles
- Roundtrip: 49/256 PASS
- Block: 4/32 PASS
- Real model: 129/3200 exact

**Issue:** Gosper L2 มีแค 49 cells — ไม่พอสำหรับ 256 weights

### 3.5 beam_hilbert_seek.c — Hilbert on 16×16 Grid

**Approach:** weight → tessellation node → Hilbert index on 16×16 grid → tile

**Results:**
- Hilbert 16×16: 256/256 PASS ✓
- Collision: 128 — 128/256 unique tiles
- Roundtrip: 128/256 PASS
- Block: 15/32 PASS
- Real model: 2504/3200 exact

**Issue:** weight_to_node mapping ทำให้ collisions — weights หลายค่า map ไป face/shell เดียวกัน

### 3.6 beam_tri_hilbert.c — Triangle Tower + Hilbert

**Approach:** weight → tessellation node → tower/block/floor → Hilbert on 4×4 grid

**Results:**
- Triangle Tower Decompose: 20736/20736 PASS ✓
- Hilbert 4×4: 16/16 PASS ✓
- Collision: 184 — 72/256 unique tiles
- Roundtrip: 72/256 PASS
- Block: 4/32 PASS
- Real model: 87/3200 exact

**Issue:** weight_to_node mapping กระจุกอยู่ไม่กี่ tower

### 3.7 beam_hilbert_maze.c — Hilbert as Maze Wall

**Approach:** Hilbert = FIXED maze, weight navigates through maze

**Results:**
- Hilbert path: 16/16 PASS ✓
- Navigation: 768/768 PASS ✓
- Collision: 248 — 8/256 unique tiles
- Roundtrip: 8/256 PASS
- Block: 1/32 PASS
- Real model: 122/3200 exact

**Issue:** Maze มีแค 16 cells — weight navigates ผ่าน 16 cells เท่านั้น

### 3.8 beam_hilbert_icosahedron.c — Hilbert × Icosahedron (FINAL)

**Approach:** Hilbert (64) × Icosahedron (162) × 2 = 20736
- 3D Hilbert maze + Icosahedron sphere = geo_jump
- Evolve: cellular automaton on Hilbert curve

**Results (Non-evolved):**
- 3D Hilbert: 64/64 PASS ✓
- Icosahedron: 162/162 PASS ✓
- Collision: 5 — 251/256 unique tiles
- Roundtrip: 251/256 PASS
- Block: 30/32 PASS
- Real model: 3100/3200 exact
- Size: 45 bytes/block = 1.32× Q8_0

**Results (Evolved):**
- 3D Hilbert: 64/64 PASS ✓
- Icosahedron: 162/162 PASS ✓
- **Collision: 0 — 256/256 unique tiles** ✓✓✓✓
- Roundtrip: 256/256 PASS ✓
- Block: 32/32 PASS ✓
- Real model: 3200/3200 exact ✓
- Size: 45 bytes/block = 1.32× Q8_0

**Evolve mechanism:**
1. Hilbert maze is FIXED (structure stays still)
2. Weight enters maze + sets neighboring cells
3. Weight EVOLVES through maze (cellular automaton, 5 steps)
4. Hash of evolved board → final position
5. **256/256 unique tiles — LOSSLESS!**

---

## 4. Key Insights

### 4.1 "Structure Stays Still, Data Moves"
- Hilbert ควรเป็น MAZE WALL — FIXED path ผ่าน tessellation
- น้ำหนักควร navigates ผ่าน maze — DATA MOVES
- ถ้าต่างคนต่างขยับ → จับกันไม่ได้

### 4.2 Tessellation Structure is Correct
- 20736 nodes, 12 pentagons, 12 shells, 144 cells
- Decompose/Recompose 20736/20736 PASS ✓
- Structure ทำงานถูกต้อง 100%

### 4.3 Frame Seek Works
- frame_enc(t) = (t × 37) % 1440
- 256/256 unique positions ✓
- Tessellation → clock_tick → frame_seek tile ทำงานได้

### 4.4 Gosper/Hilbert Need Larger Grid
- Gosper L2: 49 cells — ไม่พอสำหรับ 256 weights
- Hilbert 4×4: 16 cells — ไม่พอสำหรับ 256 weights
- ต้องใช้ Gosper L3 (343 cells) หรือ Hilbert 更大 grid

### 4.5 Weight-to-Node Mapping is Critical
- modular mapping (w/22, rem/2) ทำให้ collisions
- ต้องใช้ mapping ที่กระจายตัวทั่ว 20736 nodes
- ไม่ใช่กระจุกอยู่ไม่กี่ tower

### 4.6 Hilbert × Icosahedron = geo_jump
- (64 × 2) × 162 = 20736
- Hilbert 4×4×4 = 64 positions
- Icosahedron frequency-4 = 162 vertices
- Combined = geo_jump structure

### 4.7 Evolve is the Key
- Cellular automaton on Hilbert curve
- Structure stays still, data moves
- Hash of evolved board → injective mapping
- **256/256 unique tiles — LOSSLESS!**

### 4.8 Size Comparison
```
Q8_0:              34 bytes/block (baseline)
beam_tile_seek:    45 bytes/block (1.32×)
beam_tess:         65 bytes/block (1.91×)
beam_hilbert_ico:  45 bytes/block (1.32×) ← FINAL
```

---

## 5. Files Created

| File | Description | Status |
|------|-------------|--------|
| beam_position_codec.c | Weight → frame_enc position | Standalone test |
| beam_tess_codec.c | Tessellation position encoding | Standalone test |
| beam_tile_seek.c | Tessellation → frame seek tile | Best non-evolved |
| beam_gosper_seek.c | Gosper L2 curve | 49/256 unique |
| beam_hilbert_seek.c | Hilbert 16×16 grid | 128/256 unique |
| beam_tri_hilbert.c | Triangle tower + Hilbert | 72/256 unique |
| beam_hilbert_maze.c | Hilbert as maze wall | 8/256 unique |
| **beam_hilbert_icosahedron.c** | **Hilbert × Icosahedron + Evolve** | **256/256 LOSSLESS** |

---

## 6. Architecture Summary

```
Weight (int8, -128..127)
    ↓
weight_to_node() → tessellation node (20736 grid)
    ↓
node_to_hilbert_ico() → Hilbert × Icosahedron index
    ↓
Evolve: cellular automaton on Hilbert curve (5 steps)
    ↓
Hash of evolved board → final Hilbert position
    ↓
hilbert_ico_to_tile() → frame_seek tile (1440)
    ↓
Storage: 11 bits per weight
```

**Decode:** tile → LUT → weight (256 entries, O(1) lookup)

---

## 7. Conclusion

**สิ่งที่สำเร็จ:**
- Tessellation structure 20736 nodes ทำงานถูกต้อง 100%
- Frame seek 1440 tiles ทำงานได้
- Hilbert × Icosahedron = geo_jump structure
- **Evolve: 256/256 unique tiles — LOSSLESS!**
- "Coordinate IS data" principle ได้รับการพิสูจน์

**Key Insight:** "Structure stays still, data moves" — cellular automaton on Hilbert curve distributes weights ได้ดีกว่า direct mapping

**Size:** 45 bytes/block = 1.32× Q8_0 (ใหญ่กว่าเล็กน้อย แต่ LOSSLESS)

**Next Steps:**
- ลดขนาดให้ใกล้เคียง Q8_0 (34 bytes)
- ใช้ delta encoding ระหว่าง weights
- ใช้ tessellation structure สำหรับ delta compression
