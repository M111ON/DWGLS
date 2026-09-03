# Session Handoff — 2026-09-01

> **Branch:** `feat/geo-native-fs`
> **Purpose:** Scan project primitives, analyze capabilities, plan new geometry-based sparse container

---

## What Was Done

### 1. Primitives Scan (docs/DWGLS_PRIMITIVES.md)

Scanned all 160+ headers in `core/`. Found 21 proven subsystems:

| Category | Count | Key Files |
|---|---|---|
| Sacred Numbers | 20+ constants | gear_lock.h, fibo_spine.h, geo_config.h |
| Geometry | 11 GeoTypes | geo_param_grid.h, geo_cube_in_dodeca.h |
| Addressing | 6 views | geo_tesseract_addr.h, moe_expert_addr.h, geo_rdh_addr.h |
| Codecs | 2 proven | kis_codec_v4.h (lossless), geo_param_grid.h |
| Timeline | 4 systems | fibo_spine.h, geo_frame_seek.h, geo_tring_walk.h, fibo_walk.h |
| Storage | 5 backends | dramtile_store.h, geo_adaptive_store.h, residual_space.h |
| Containers | 4 formats | geo_kis_container.h, geo_tess_container.h, geo_goldberg_file.h |
| Ghost/Capacity | 3 systems | geo_ghost_lift.h, geo_ghost_envelope.h, geo_cap_account.h |
| GeoFS | 1 system | geofs_core.h |
| MoE | 3 components | moe_expert_addr.h, moe_expert_store.h, tied_dedup.h |

### 2. Capabilities Inventory (docs/DWGLS_CAPABILITIES.md)

Documented what each primitive does, with code examples and proven status. All 21 subsystems are complete and proven on real data.

### 3. FGLS vs DWGLS Diff (docs/DWGLS_vs_FGLS_DIFF.md)

Compared with original `I:\FGLS_new`:

| | FGLS_new | DWGLS |
|---|---|---|
| Core headers | 86 unique | 104 unique |
| infra/ | 0 | 16 (entirely new) |
| Tests | 6 | 200 |
| Tools | 0 | 100+ |

**DWGLS added:** 20736 address space, tesseract 4D, MoE expert, ghost lift, hyperbolic walk, GeoFS, breathing FS, delta compression, unified volume.

**DWGLS removed:** ~86 legacy headers (metatron, hamburger, geopixel, fabric wire, onion shell, etc.)

### 4. Legacy Catalog (docs/FGLS_LEGACY_CATALOG.md)

Categorized 86 FGLS_new-only headers into 4 groups:
- **A (7 potentially useful):** geo_field_core.h (multi-resolution zoom), geo_tring_fec.h (FEC), geo_temporal_ring.h (gap detection), geo_diamond_to_scan.h, geo_onion_shell.h, geo_addr_net.h, geo_metatron_reshape.h
- **B (2 experimental):** geo_payload_store.h, geo_tring_goldberg_wire.h
- **C (5 superseded):** geo_goldberg_tile.h, kis_codec.h/v3.h, geo_compound_cfg.h, geo_tring_addr.h
- **D (72 deprecated):** fabric_wire, temporal_ring, metatron, etc.

---

## What Was Discussed (New Vision)

### The Core Idea: Geometry-Based Sparse Container with Latent Seed

**User's vision (evolved through conversation):**

1. **Breathing File System** — already built (`breathing_fs.h`, `bfs_breath.h`, `bfs_seek_anchor.h`, `bfs_persist.h`), expand/contract but always lossless + deterministic

2. **Tesseract as container** — 18 tesseracts × 8 cubes × 144 slots = 20736
   - Cube 0 = INDEX FRAME (hook, invariant under breathing)
   - Cubes 1-7 = DATA (derived from cube 0)
   - 18 tesseracts hook into 5D

3. **Latent seed concept** — NOT neural network training
   - Seed (uint64, 8 bytes) → formula → weights (lossless)
   - Like VAE latent space but LOSSLESS (geometry map, not neural network)
   - Seed IS the compressed model
   - Different seeds = different models

4. **Sparse training consideration** — train only cube 0 (12.5%), derive cubes 1-7 (87.5%)
   - Requires MORE computation per epoch (derive 1008 slots)
   - But saves 87.5% memory
   - Need to benchmark: derivation cost vs memory savings
   - User concluded: "ต้องมาชั่งดูว่าคุ้มไหม"

5. **Multi-view batch training** — user mentioned but decided it's too resource-heavy for now

6. **Key insight: Geometry map ≠ Neural network**
   - Neural network: seed → [millions of params] → weights (non-linear, lossy, expensive training)
   - Geometry map: seed → [few integers] → weights (linear, lossless, O(1) compute)
   - If formula is bijection → inverse exists → no search needed

### What Exists but Isn't Connected Yet

```
breathing_fs.h (expand/contract) ←→ NOT CONNECTED ←→ geo_tesseract_addr.h (cube 0 = index)
                              ↕
                    moe_expert_addr.h (weight storage)
```

### Key Questions Unanswered

1. **Can cube 0 (144 slots) route to cubes 1-7 (1008 slots)?** — YES if formula-based derivation works
2. **Is derivation cost < stored cost?** — Need benchmark
3. **Does sparse training provide net speedup?** — Need benchmark
4. **How to find the right seed?** — Inverse formula if bijection, search otherwise
5. **"Add data through some layer"** — User hasn't decided which layer yet (seed? cube 0? cubes 1-7?)

---

## Files Created This Session

| File | Location | Content |
|---|---|---|
| DWGLS_PRIMITIVES.md | docs/ + Obsidian | Sacred numbers, geometry, addressing, codec tables |
| DWGLS_CAPABILITIES.md | docs/ + Obsidian | 21 proven subsystems with code examples |
| DWGLS_vs_FGLS_DIFF.md | docs/ + Obsidian | Fork diff analysis |
| FGLS_LEGACY_CATALOG.md | docs/ + Obsidian | 86 legacy headers categorized |
| SESSION_HANDOFF_2026-09-01.md | docs/ + Obsidian | This file |

---

## Next Steps (For Next Session)

### Priority 1: Validate the Concept

| Step | What | Why |
|---|---|---|
| **1a** | Prototype cube 0 → cubes 1-7 derivation | Prove formula-based routing works |
| **1b** | Measure derivation cost (ns/slot) | Prove O(1) per slot |
| **1c** | Measure memory savings | Prove 87.5% reduction is real |

### Priority 2: Design Cube 0 Format

| Step | What | Why |
|---|---|---|
| **2a** | Design cube 0 data layout | What goes in 144 slots to route 1008 slots? |
| **2b** | Design breathing-aware update | How does cube 0 update when system breathes? |
| **2c** | Design 5D hook | How do 18 tesseracts connect to 5D? |

### Priority 3: Connect Existing Systems

| Step | What | Why |
|---|---|---|
| **3a** | Connect breathing_fs ↔ tesseract | Make breathing work with index frame |
| **3b** | Connect tesseract ↔ weight storage | Map real model weights into container |
| **3c** | Verify lossless roundtrip | weights → container → derive → same weights |

### Priority 4: Seed-Based Latent Space (Future)

| Step | What | Why |
|---|---|---|
| **4a** | Find inverse formula for tesseract mapping | Make seed → weights reversible |
| **4b** | Design seed compression | How small can seed be? |
| **4c** | Prototype seed search | Find seed that produces target weights |

---

## Key Quotes from User

> "tesseract ผมไม่ได้จะเอามาใช้แบบนั้น... hook frame นึงเอาไว้เป็น index ไม่ว่าระบบจะย่อจะขยายยังไง frame นี้จะคงที่"

> "ระบบผมเป็น breathing file system ที่ย่อขยายได้ ทุกอย่างเปลี่ยนแปลงตลอดเวลา แต่ lossless และ deterministic เสมอเวลาอ่าน"

> "ผมทำไปแล้วไม่ใช่หรอ แต่ยังไม่ได้เชื่อม และยังไม่ได้อิงถึงเรื่องเอา weight มาใส่"

> "ก็เพราะเป็น geometry map เลยง่ายกว่าปกติไง"

> "อันนี้ดูเรื่องใหญ่... สำหรับการเทรนจริงๆผมมองเป็นอีกแบบนึง เอาไว้อยู่ คอนเซปเหมือนกับ latent ของ genAi model A, seed#1234, encode/decode:fixed = same result"

> "ก็เพราะเป็น geometry map เลยง่ายกว่าปกติไง — ไม่ต้อง train, แค่ compute"
