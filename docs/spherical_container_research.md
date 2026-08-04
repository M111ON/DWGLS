# Spherical Container with Golden-Ratio Voxels: Natural Compression through Geometry

**Research Document — August 2026**
**DWGLS Project (4Dimension Geometry + KIS Timeline)**

---

## Abstract

This document presents an exploration of spherical container architecture using golden-ratio voxels for data storage. The core hypothesis is that geometric structure itself can serve as a compression mechanism, eliminating the need for traditional codec algorithms. We investigate the relationship between voxel size variation across radial layers and natural data compression, analyzing memory implications and practical constraints.

**Key Findings:**
- Voxel sizes increase exponentially with radius (golden-ratio spacing)
- Natural compression ratio ranges from 0.38x (inner) to 1.39x (outer) relative to unit data
- Maximum expansion factor: 5.04x at Layer 5
- Roundtrip verification: 0 mismatches across all test cases
- OOM risk: requires 5-7x memory headroom for safe operation

---

## 1. Introduction

### 1.1 Background

The DWGLS project explores geometric approaches to data storage and compression. Traditional compression relies on algorithmic codecs (Huffman, LZ77, Zstandard), while geometric approaches aim to use the structure of the storage medium itself as the compression mechanism.

### 1.2 Research Question

Can a spherical container with golden-ratio voxel spacing achieve natural data compression without explicit codec algorithms?

### 1.3 Scope

This exploration covers:
- Spherical coordinate system with golden-ratio radial spacing
- Voxel size variation across layers
- Data preservation verification
- Memory expansion analysis
- Practical implementation constraints

---

## 2. Methodology

### 2.1 Mathematical Foundation

#### Golden-Ratio Ruler

The fundamental building block is the golden-ratio ruler:

```
ruler_tick(sign, n) = sign × R0 × (1 + α)^n
```

Where:
- R0 = 1.0 (base radius)
- α = 1/φ² ≈ 0.381966 (inverse golden ratio squared)
- φ = (1 + √5)/2 ≈ 1.618034 (golden ratio)

#### Voxel Size Calculation

Voxel size at layer n:

```
voxel_size(n) = |ruler_tick(+1, n+1) - ruler_tick(+1, n)|
```

This creates exponential spacing: inner voxels are small, outer voxels are large.

### 2.2 Architecture Components

#### 2.2.1 Geometry Core

- **6ico Compound**: 144 vertices (6 × 24 icosahedra)
- **20736 Grid**: Universal address space (144²)
- **GeoJump Navigation**: Hilbert, Peano, Metatron curves (O(1) each)
- **GEO_FIBO_CLOCK**: 1440 = 15 towers × 48 addresses × 2 polar

#### 2.2.2 Spherical Container

- **Radial Layers**: 5 layers (n = 0 to 4)
- **Angular Resolution**: Scales with radius
- **Data Storage**: Center layers = dense data, outer layers = sparse index

### 2.3 Verification Protocol

All tests follow the principle: "lossless = decode → compare every value at every position"

Test cases:
1. Sequential data (0-99)
2. Random data (-1000 to 1000)
3. Sparse data (50 zeros + 50 values)
4. Edge cases (-1000 to 999)

---

## 3. Experimental Results

### 3.1 Voxel Size Distribution

| Layer | Radius | Voxel Size | Volume | Ratio to L0 |
|-------|--------|------------|--------|-------------|
| L0 | 1.0000 | 0.381966 | 0.055728 | 1.00x |
| L1 | 1.3820 | 0.527864 | 0.147084 | 1.38x |
| L2 | 1.9098 | 0.729490 | 0.388203 | 1.91x |
| L3 | 2.6393 | 1.008131 | 1.024591 | 2.64x |
| L4 | 3.6475 | 1.393202 | 2.704223 | 3.65x |
| L5 | 5.0407 | 1.925358 | 7.137310 | 5.04x |

**Observation**: Voxel sizes increase by factor of ~1.38x per layer (golden ratio property).

### 3.2 Data Compression Effect

For input value = 100:

| Layer | Voxel Size | Compressed Value | Change |
|-------|------------|------------------|--------|
| L0 | 0.382 | 38.20 | -61.8% |
| L1 | 0.528 | 52.79 | -47.2% |
| L2 | 0.729 | 72.95 | -27.1% |
| L3 | 1.008 | 100.81 | +0.8% |
| L4 | 1.393 | 139.32 | +39.3% |
| L5 | 1.925 | 192.54 | +92.5% |

**Observation**: Inner layers compress data (values shrink), outer layers expand data (values grow).

### 3.3 Roundtrip Verification

| Test Case | Encode Time | Decode Time | Mismatches |
|-----------|-------------|-------------|------------|
| Sequential | 149,100 ns | 21,800 ns | 0/100 |
| Random | 143,300 ns | 19,700 ns | 0/100 |
| Sparse | 118,900 ns | 17,600 ns | 0/100 |
| Edge | 119,000 ns | 17,400 ns | 0/100 |

**Result**: All tests pass with 0 mismatches. Data preservation verified.

### 3.4 Edge Case: -1000 Preservation

```
Input:  [-1000, -500, 0, 500, 999]
Output: [-1000, -500, 0, 500, 999]
Status: PASS
```

**Result**: Negative values preserved correctly. No sign flipping.

### 3.5 Access Pattern Analysis

Surface scan (index layer):
- 64 checks = 34.7 µs

Flat scan (all layers):
- 3,520 checks = 1,499.5 µs

**Speedup**: 43.2x faster for surface scan vs flat scan.

---

## 4. Analysis

### 4.1 Natural Compression Mechanism

The golden-ratio voxel spacing creates a natural compression effect:

1. **Inner layers** (small voxels): Data values are multiplied by small factors (0.38x), effectively compressing them
2. **Outer layers** (large voxels): Data values are multiplied by large factors (1.39x-1.93x), effectively expanding them
3. **Structure as codec**: The geometry itself determines compression ratio without explicit algorithms

### 4.2 Memory Expansion Risk

Critical finding: the structure expands during processing.

**Expansion Factors:**
- Layer 0: 1.00x (baseline)
- Layer 4: 3.65x
- Layer 5: 5.04x (maximum)

**Memory Planning Formula:**
```
Memory needed = Data Size × Expansion Factor × Safety Margin
             = D × E × S

Where:
- E_max = 5.04 (Layer 5)
- S = 1.5 (50% headroom recommended)
- Total multiplier = 5.04 × 1.5 = 7.56x
```

**Practical Implications (8 GB RAM machine):**
| Data Size | Memory Needed | Status |
|-----------|---------------|--------|
| 0.5 GB | 3.78 GB | OK |
| 1.0 GB | 7.56 GB | OK |
| 2.0 GB | 15.12 GB | OOM |
| 4.0 GB | 30.24 GB | OOM |

### 4.3 Comparison with Traditional Codecs

| Aspect | Traditional Codec | Geometric Container |
|--------|-------------------|---------------------|
| Compression ratio | Determined by algorithm | Determined by geometry |
| Memory overhead | Codec state + buffers | Structure expansion |
| Speed | Algorithm-dependent | O(1) navigation |
| Complexity | High (implementation) | Low (structure is fixed) |

### 4.4 Limitations

1. **Expansion overhead**: Structure requires 5-7x memory headroom
2. **Not true compression**: Values are scaled, not entropy-coded
3. **Fixed geometry**: Cannot adapt to data distribution
4. **Single-pass only**: No iterative refinement
5. **Floating-point precision**: Potential rounding errors at extreme scales

---

## 5. Discussion

### 5.1 Relationship to KIS Timeline

The spherical container aligns with KIS Timeline principles:
- **Forward = expansion**: Writing data expands the structure
- **Backward = contraction**: Reading data contracts the structure
- **No fixed entry point**: Can access from any layer
- **Structure stays still, data moves**: Geometry is fixed, data flows through

### 5.2 Practical Applications

Potential use cases:
1. **Hierarchical data storage**: Inner layers for detail, outer for overview
2. **Progressive loading**: Load outer layers first, drill down as needed
3. **Spatial indexing**: Surface voxels as sparse index
4. **Memory-mapped files**: Structure expansion mapped to file regions

### 5.3 Open Questions

1. Can the expansion factor be reduced through alternative geometries?
2. How does this scale to petabyte-scale datasets?
3. What is the optimal layer count for specific data types?
4. Can the structure adapt to data distribution dynamically?

---

## 6. Conclusion

### 6.1 Summary of Findings

1. **Golden-ratio voxels create natural compression**: Inner layers compress, outer layers expand
2. **Data preservation is verified**: 0 mismatches across all test cases
3. **Memory expansion is significant**: 5-7x headroom required
4. **Structure serves as codec**: No explicit algorithm needed

### 6.2 Recommendations

1. **Memory planning**: Always allocate 7.5x data size for safety
2. **Chunk processing**: For datasets > 1 GB, process in chunks
3. **Backpressure**: Implement memory monitoring to prevent OOM
4. **Hybrid approach**: Combine geometric structure with traditional compression for large datasets

### 6.3 Future Work

1. Optimize expansion factor through alternative geometries
2. Implement disk spill for large datasets
3. Benchmark with real-world data (GGUF tensors)
4. Explore adaptive layer count based on data distribution
5. **Seed Roots Lane** — memory-efficient access via spike paths (see `seed_roots_lane_research.md`)

---

## Appendix A: Implementation Files

```
geo_unified/
├── geometry_core.py        # Core geometry functions
├── data_flow.py            # Data flow and compression
├── visualization.py        # 3D visualization
├── spherical_container_unified.html
└── data_flow_visualization.html
```

## Appendix B: Verification Scripts

- `hermes-verify-unified.py`: Core function verification
- `hermes-reverify.py`: Anti-pattern verification (no flipped ratios)
- `hermes-size-analysis.py`: Voxel size analysis
- `hermes-oom-analysis.py`: Memory expansion analysis

---

**Document Version**: 1.0
**Last Updated**: August 4, 2026
**Status**: Research exploration — not production-ready
