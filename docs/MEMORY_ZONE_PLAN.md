---
luminaCreated: 2026-08-16T06:55:01.897Z
tags: []
luminaModified: 2026-08-16T06:55:01.897Z
luminaVersion: 1.3.11
---
# Memory Zone Planning — 7B on 4GB GPU
## Date: 2026-08-06 | Status: Phase 2.5

---

## Goal
Prove 7B model can fit in 4GB GPU memory via geometric addressing + lazy loading.

---

## Memory Budget: 7B Q8_0 model

| Category | Size | Notes |
|----------|------|-------|
| Raw weights | 7.0 GB | 7B × 1B (Q8_0 = 8 bit per weight) |
| GGUF overhead | ~0.1 GB | metadata, tokenizer, padding |
| **Total disk** | **~7.1 GB** | |
| Available GPU VRAM | **4.0 GB** | Target |
| **Required reduction** | **~43%** | Must offload or compress |

## Zone Architecture

```
┌─────────────────────────────────────────────┐
│ GPU VRAM (4 GB)                             │
│ ┌─────────┐ ┌────────┐ ┌─────────────────┐ │
│ │ HOT     │ │ WARM   │ │ SHARED ADDR     │ │
│ │ Active  │ │ Async  │ │ SPACE           │ │
│ │ Layers  │ │ Pref-  │ │                 │ │
│ │         │ │ etch   │ │                 │ │
│ │ 800 MB  │ │1.2 GB  │ │ 200 MB          │ │
│ └─────────┘ └────────┘ └─────────────────┘ │
│                           ┌─────────────┐  │
│                           │ INDEX       │  │
│                           │ (fast LR)   │  │
│                           │ 200 MB      │  │
│                           └─────────────┘  │
├─────────────────────────────────────────────┤
│ SYSTEM RAM (16-32 GB)                       │
│ ┌──────────────────────────────────────────┐│
│ │ COLD CACHE (mmap'd from disk)            ││
│ │ All 7B weights, not actively accessed    ││
│ │ 7.1 GB                                   ││
│ └──────────────────────────────────────────┘│
└─────────────────────────────────────────────┘
```

### Zone Budget

| Zone | Size | Memory | Data | Rationale |
|------|------|--------|------|-----------|
| **HOT** | 800 MB | GPU | Current layer + residual | Only the active execution layer |
| **WARM** | 1.2 GB | GPU | Next N layers prefetched | Async load during compute |
| **INDEX** | 200 MB | GPU | GeoType energy LUTs | Address translation tables |
| **SHARED** | 200 MB | GPU | Norm layers, embeddings | Shared across all layers |
| **COLD** | 7.1 GB | RAM | Remaining weights | OS page cache, transparent |
| | | | | |
| **GPU total** | **2.4 GB** | | + 1.6 GB headroom for inference | |
| **System RAM** | **7.1 GB** | | Virtual memory (faults to disk) | |

## Geometric Compression Analysis

### Cell-Type Pruning (Phase 2.3 data)
```
DII: 3,315 (16.0%) — MAIN — always kept
DID: 3,303 (15.9%) — MAIN — always kept
III: 2,055 ( 9.9%) — PROBE — prunable
IDI: 2,055 ( 9.9%) — PROBE — prunable
DDI: 2,973 (14.3%) — MIRROR — depends
DDD: 2,961 (14.3%) — MIRROR — depends
IID: 2,037 ( 9.8%) — PROBE — prunable
IDD: 2,037 ( 9.8%) — PROBE — prunable
```

- **MAIN (DII+DID): 30.9%** → keep lossless
- **MIRROR (DDI+DDD): 28.6%** → partial keep
- **PROBE (III+IDI+IID+IDD): 39.5%** → prune at 10% threshold

**At 15% threshold: 68.1% prunable** — but needs inference validation

## Access Patterns

### Layer-by-layer inference
1. Token processed through layer 0
2. Layer 0 weights HOT (800MB)
3. Layers 1-3 pre-fetched async (WARM, 1.2GB)
4. Layer 0 finished → layer 1 becomes HOT → layers 2-4 pre-fetched
5. Continue through 32 layers

### Memory flow
```
HOT (current) ← WARM (next) ← COLD (RAM)
     ↓ compute
HOT → COLD (evicted)
WARM → HOT (promoted)
RAM → WARM (prefetched from COLD)
```

## Address Space Mapping

- `geo_frame_seek.h`: seek to any tensor's DiamondBlocks in O(1)
- `geo_param_grid.h`: GeoType selection (144 verts for Q8_0)
- `geo_cube_container.h`: DiamondBlock (64B atomic unit)

### Direct I/O Budget
```
Read one tensor = 32 M words × 1 byte = 32 MB
Disk read (SSD): 500 MB/s → 32 MB = 64 ms
PCIe bandwidth: 16 GB/s → 32 MB = 2 ms
GPU to load: ~2 ms + 64 ms disk = 66 ms per layer
```

## Risk

| Risk | Severity | Mitigation |
|------|----------|------------|
| Vulkan ErrorDeviceLost (b9733) | **CRITICAL** | Must fix or switch backend |
| 7B has 32 layers → 32 × 66ms = 2.1s delay | Medium | Pre-fetch hides most |
| KV cache for 2048 context | 1.6 GB GPU | Uses remaining GPU headroom |
| cell-type pruning unverified | Medium | Need inference test |
| 39.5% pruning may not hold on all models | Medium | Need 3+ model validation |

## Path to 7B on 4GB

```
1. ✅ Cell-type classification (Phase 1.4) — 8 types identified
2. ✅ Pruning engine (Phase 2.3) — 39.5% @ 10%, 68.1% @ 15%
3. ✅ GCube container (Phase 2.2) — 64B atomic block
4. ✅ Diamond field mapping (Phase 2.4) — 144 vert bijection
5. ✅ Batch converter (Phase 2.1) — GGUF → GCube
6. ⏸ Inference test (Phase 3) — blocked by Vulkan b9733
7. ⏸ Memory zone integration — after inference proof
```

## Immediate block

**Vulkan ErrorDeviceLost (ggml#9733)** — prevents inference testing. Resolution:
- Use CUDA build (llama.cpp with CUDA)
- Run with `-ngl 0` (pure CPU)
- Wait for ggml fix

## Conclusion

**7B on 4GB = mathematically possible** (2.4 GB weight budget with 68.1% pruning). 
**Engineering barrier**: inference engine proof (no Vulkan GPU available for test).

**Unblocking priority:** 
1. CPU-only inference proof (ignore GPU)
2. Verify honesty: tensor weights ≠ zero after geo routing
3. Measure perplexity impact of 15% threshold pruning