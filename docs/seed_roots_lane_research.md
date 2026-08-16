---
luminaCreated: 2026-08-16T06:55:02.145Z
tags: []
luminaModified: 2026-08-16T06:55:02.145Z
luminaVersion: 1.3.11
---
# Seed Roots Lane — Access Bigger Space Without Exploding Memory

**Research Document — August 2026**  
**DWGLS Project (4Dimension Geometry + KIS Timeline)**

---

## Abstract

This document presents the Seed Roots Lane architecture — a memory-efficient data access pattern that follows spike paths (roots) from seed points, combined with Cardioid direction control, Threshold magnitude control, Voronoi spatial subdivision, and AdaptiveLane channel subdivision (from Qwen's contribution).

**Key Findings:**
- Memory reduction: 99.67% (69 / 20,736 grid positions)
- Root path: follows geometric spike pattern from seed
- Cardioid: prevents spike clustering in single direction
- Threshold: limits data magnitude per spike
- Voronoi: adaptive spatial subdivision (9 phi-based seeds)
- AdaptiveLane: 144 base channels (6ico compound), subdivide by depth (2^depth)

---

## 1. Introduction

### 1.1 Background

Traditional data access loads entire datasets into memory. For large grids (20,736 positions), this causes OOM on limited hardware. Seed Roots Lane solves this by:

1. Starting from a seed point (ico or dodeca vertex)
2. Following spike paths outward (root growth)
3. Loading only accessed chunks on demand (lazy loading)
4. Controlling direction/magnitude via geometry

### 1.2 Core Principle

```
Structure IS the access pattern
Geometry bok forns where to go next
No separate index needed
```

### 1.3 Inspiration

- **Infinity Castle** (Demon Slayer): rooms shift together, not independently
- **Capo Shift** (guitar): shift all notes by changing capo position, relationships preserved
- **Root Growth**: seeds → roots → branches, organic expansion

---

## 2. Architecture

### 2.1 Components

```
SeedRootsLaneV3
├── Cardioid (direction control)     → spike กระจายไม่กระจุก
├── Threshold (magnitude control)    → จำกัดขนาด spike
├── Voronoi (spatial subdivision)    → แบ่งพื้นที่อัตโนมัติ
├── AdaptiveLane (channel subdivision) → 144 channels, subdivide by depth
└── Seed Roots (lazy loading)        → ไม่ load ทั้งหมด
```

### 2.2 Cardioid — Direction Control

```python
def cardioid(theta, a=1.0):
    """Cardioid: r = a(1 + cos(θ))"""
    return a * (1 + np.cos(theta))
```

**Purpose:** ไม่ให้ spike กระจุกในทิศทางเดียว
- θ = 0 → r = 2a (longest)
- θ = π → r = 0 (shortest)
- ผลลัพธ์: spike กระจายเป็นรูปหัวใจ

### 2.3 Threshold — Magnitude Control

```python
def apply_threshold(value, threshold, mode='clip'):
    """Threshold: จำกัดขนาดข้อมูล"""
    if mode == 'clip':
        return np.clip(value, -threshold, threshold)
    elif mode == 'gate':
        return value if abs(value) <= threshold else 0
```

**Modes:**
- `clip`: ตัดค่าที่เกิน threshold
- `gate`: ผ่านเฉพาะค่าที่น้อยกว่า threshold
- `scale`: ลดขนาดลง proportionally

### 2.4 Voronoi — Spatial Subdivision

```python
class VoronoiSeeds:
    def __init__(self, n_seeds=9):
        self.seeds = self._phi_seeds(n_seeds)  # golden ratio distribution
```

**Key:**
- 9 seeds (phi-based) = กระจายสม่ำเสมอที่สุด
- Subdivide when region too dense (threshold=10)
- New seed = centroid of dense region

### 2.5 AdaptiveLane — Channel Subdivision (from Qwen)

```python
class AdaptiveLane:
    def __init__(self, base_channels=144):
        self.base_channels = base_channels  # 6ico compound = 144 vertices
    
    def activate(self, channel_id, depth=0):
        """depth=0: 1ch, depth=1: 2ch, depth=2: 4ch, depth=3: 8ch"""
```

**Key:**
- 144 base channels = 6ico compound (144 vertices)
- Only activate channels that are needed
- Subdivide by depth: 2^depth sub-channels

### 2.6 Seed Roots — Lazy Loading

```python
class SeedRootsLaneV3:
    def grow_root(self, depth, threshold=None):
        """Grow root by one spike with threshold control"""
        direction = self.spike_direction(depth)
        new_pos = self.current_pos + direction
        # ... load chunk on demand
```

**Key:**
- Start from seed (origin)
- Follow spike paths outward
- Load chunk only when accessed
- Memory = O(accessed), NOT O(total)

---

## 3. Experimental Results

### 3.1 Memory Efficiency

| Component | Memory | Reduction |
|-----------|--------|-----------|
| Full grid | 20,736 | 0% |
| Loaded chunks | 20 | 99.90% |
| Active channels | 7 | 99.97% |
| Sub-channels | 49 | 99.76% |
| **Total integrated** | **69** | **99.67%** |

### 3.2 Root Path Growth

```
Spike  0: region= 1  sub_channels= 1  pos=(0.00, 0.00, 0.76)
Spike  5: region= 5  sub_channels= 8  pos=(3.09, 4.72, 1.76)
Spike 10: region= 7  sub_channels= 8  pos=(1.05, 7.82, 0.62)
Spike 15: region= 6  sub_channels= 8  pos=(-10.00, -10.00, 10.00)
Spike 19: region= 8  sub_channels= 8  pos=(10.00, -10.00, -10.00)
```

### 3.3 Voronoi Distribution

| Seeds | Final Seeds | Path | Memory |
|-------|-------------|------|--------|
| 9 | 40 | 3 steps | 3 |
| 18 | 47 | 4 steps | 4 |
| 36 | 60 | 2 steps | 2 |
| 72 | 89 | 2 steps | 2 |

### 3.4 Comparison: Full Grid vs Integrated

```
Full grid:       20,736
Integrated:      69
Reduction:       99.67%
Ratio:           0.003328x
```

---

## 4. Analysis

### 4.1 Why Voronoi Subdivision Helps

```
More seeds = more regions
More regions = more path options
More options = shorter paths
Shorter paths = less memory
```

### 4.2 Infinity Castle Analogy

```
Castle = structure ที่เปลี่ยนรูปได้
Capo = shift ทุกอย่างพร้อมกัน
Music = relationships ที่ไม่เปลี่ยน

เปลี่ยนจุดเดียว = เปลี่ยนทั้งหมด
ไม่ต้องคิด = geometry คิดให้
```

### 4.3 Root Growth Pattern

```
Seed = ico or dodeca vertex
Spike = connection ระหว่าง duals
Root path = วิ่งตาม spike ออกไปข้างนอก

ICO (20 faces)
    ↓ spike
DODECA (12 faces)
    ↓ spike
ICO (เพิ่ม resolution)
    ↓ spike
DODECA (เพิ่ม resolution)
    ...
```

---

## 5. Implementation

### 5.1 Files

```
dropbag/
├── seed_roots_lane.py           # V1: basic seed roots
├── seed_roots_lane_v2.py        # V2: + cardioid + threshold + voronoi
├── seed_roots_lane_v3.py        # V3: + AdaptiveLane (integrated)
├── voronoi_root_efficiency.py   # Voronoi path optimizer
└── voranoi_seed_roots.py        # Qwen's original (bugs fixed)
```

### 5.2 Usage

```python
from seed_roots_lane_v3 import SeedRootsLaneV3

# Create lane
lane = SeedRootsLaneV3()

# Grow roots
for depth in range(20):
    pos, chunk_id, region, sub_count = lane.grow_root(depth, threshold=10.0)
    lane.load_chunk(chunk_id, data)

# Check memory
summary = lane.get_summary()
print(f"Memory reduction: {summary['reduction']:.2f}%")
```

---

## 6. Conclusion

### 6.1 Summary

1. **Seed Roots Lane**: memory-efficient access via spike paths from seed
2. **Cardioid**: prevents spike clustering
3. **Threshold**: limits data magnitude
4. **Voronoi**: adaptive spatial subdivision
5. **AdaptiveLane**: 144 channels, subdivide by depth
6. **Result**: 99.67% memory reduction (69 / 20,736)

### 6.2 Key Insight

```
Structure IS the access pattern
Geometry bok forns where to go next
No separate index needed

เหมือน:
- รากงอกออกจากเมล็ด
- เส้นใยเติบโตจากจุดกำเนิด
- Structure บังคับ path เอง
```

---

**Document Version**: 1.0  
**Last Updated**: August 5, 2026  
**Status**: Research exploration — integrated with Qwen's AdaptiveLane
