---
luminaCreated: 2026-08-16T06:55:01.412Z
tags: []
luminaModified: 2026-08-16T06:55:01.412Z
luminaVersion: 1.3.11
---
# Geo Jump Explorer — Experiment Results (2026-08-09)

## 🔬 บทสรุปการทดลอง

สำรวจ `geo_jump` (FGLS_new/collection/geo_jump_module/include/geo_jump.h)
เพื่อพิสูจน์ว่า: **Hilbert/Peano = maze walls (structure stays still, data moves) ไม่ใช่ space-filling**

### พื้นฐานที่ยืนยัน (จาก codebase)

| Constant | Value | Source | Meaning |
|----------|-------|--------|---------|
| `TRIT_MOD` | 27 (3³) | frustum_trit.h | ternary address dimension |
| `L38_FED_SV` | 34 | pogls38_fed_bridge.h | step vector (FROZEN) |
| `CYL_SIDE_HALF` | 27 | geo_cylinder.h | 3 side × 9 face (visible half) |
| `CYL_SIDE_FULL` | 54 (6×9) | geo_cylinder.h | bridge sacred number |
| `FRAME_STRIDE` | 37 | geo_frame_seek.h | **1440-cycle เท่านั้น** (prime, gcd(37,1440)=1) |
| `GEO_FIBO_CLOCK` | 1440 | geo_jump.h | cycle |
| `GEO_FULL` | 20736 (144²) | geo_jump.h | full address space |

### ผลการทดลอง MOD strides

`MOD walk: node' = (node × stride) % 20736`

| stride | order (unique slots) | note |
|--------|-------|------|
| 37 (frame_seek) | **576** (2.8%) | ❌ โดเมนผิด — 37 ออกแบบสำหรับ 1440-cycle |
| 27 (trit) | 66 (0.3%) | ❌ gcd(27,20736)=27 ≠ 1 |
| 34 (L38 SV) | 35 (0.2%) | ❌ gcd(34,20736)=2 ≠ 1 |
| 17 (8+9) | 144 (0.7%) | ❌ ไม่ใช่ generator |
| 54 (bridge) | 9 (0.0%) | ❌ gcd(54,20736)=54 ≠ 1 |
| 162 (ico) | 9 (0.0%) | ❌ gcd |
| 16813 (inv37) | 576 (2.8%) | ❌ อยู่กลุ่มเดียวกับ 37 |
| **5** | **1728 (8.3%)** | ✅ **MAX order (lcm(64,54)=1728)** |

### ⭐ ค้นพบ: 1728 = max order = 1 pentagon

```
φ(20736)          = 6912  (number of units)
20736 = 2⁸ × 3⁴

max element order = lcm(2⁶, 54) = lcm(64, 54) = 1728 ✓
1728 = 144 × 12 = GEO_TOWER face_slots

**MOD stride คนเดียว cover ได้มากสุด 1728 = 1 pentagon (12 faces)**
12 orbits × 1728 = 20736 = FULL space
```

### 🖖 4-Tetra Compound → 1440 cycle

| Concept | Value | Meaning |
|---------|-------|---------|
| 5 tetra × 12 origins | 60 | compound per 1 compound |
| × 12 sets | 720 | island (GEO_FIBO engine) |
| × 2 invert | **1440** | GEO_FIBO_CLOCK ✓ |
| 20736 / 1440 | 14.4 | → 0.4 threshold (geometry-given) |

### ✅ PASS: Baseline 25/25 (TIER1 22 + TIER2 3) — no regression

---

## 📁 ผลการทดลอง → สรุปเป็นหลักการ

```
MAZE WALL model (ถูกต้อง)
  Hilbert/Peano = structure (กำแพง), data walks through
  data NOT fill space — data visits addresses along walls
  geo_jump(node) = position-in-maze (bijective map)
  ไม่ใช่ "walk to next cell"

STRIDE หลัก (โดเมน-specific)
  37  → 1440-cycle only (frame_seek)
  5   → 20736 MOD stride (1728 order, 12 orbits = full)
  27  → trit dimension (address space struct, NOT stride)
  34  → L38 step vector (journey step, NOT stride)
```

## 🔭 Key Insight ต่อยอด

- **12 pentagons × 1728 = 20736** — 20736 มี sector structure จริง
- **MOD(5) walk = 1 pentagon cycle** — เป็น "clock ภายใน pentagon"
- **Threshold 0.4 = 20736/1440 remainder** — ผูกกับ 12-sector geometry
- **5-tetra compound = 4D mapping** — 60→720→1440 climb ตรงตาม sacred chain:
  `144 (12²) → 720 (60) → 1440 (cycle) → 20736 (144²)`

---

## Files touched

- `tests/geo_jump_explore.c` — explorer: 6 jump types, MOD coverage, Pentagon, Invert, Capo, DNA, Climate
- `tests/geo_jump_real_test.c` — real test (MOD sweep 27/34/17/54/162/16813)
- `tests/mod_order_sweep.c` — brute-force stride order search (พบ 5 → 1728)

No core files changed (PoC only). Baseline make test: **25/25 PASS** ✓