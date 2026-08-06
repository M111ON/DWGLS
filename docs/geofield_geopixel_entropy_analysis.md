# Geofield / Geopixel Entropy Analysis
## How Geometric Encoding Handles (or Fails) High-Entropy Data

**Date:** 2026-08-06  
**Context:** User's observation: "geofield กับ geopixel ที่บีบข้อมูลได้ดีแต่ยังแพ้ entropy"

---

## 1. What Files Exist (and What They Map To)

| User Term | Actual File | Purpose |
|-----------|-------------|---------|
| **geofield** | `core/geo_diamond_field_v4.h` (65KB, 1563 LOC) | Diamond shell/slot geometry with 5-path adaptive encoding per 64-byte chunk |
| **geopixel** | No dedicated file. Concept maps to **KIS v6** per-slot encoding (`core/kis_codec_v6.h`) + `geo_kis_projection.h` | Per-weight position mapping onto 20736-slot KIS grid |

Both components live in `I:/DWGLS/core/`. The geofield handles chunk-level (64B) geometric placement + compression. The geopixel handles weight-level position encoding on the KIS timeline.

---

## 2. What Compression They Provide

### Geo Diamond Field v4 — Chunk-Level Adaptive Encoding

5 encoding paths, chosen per 64-byte chunk based on entropy classification:

| Path | Method | Best For | Worst Case |
|------|--------|----------|------------|
| 0 | **Sparse** (count + pos,val pairs) | ≤2 unique values → ~2-4B/chunk | High entropy → not selected |
| 1 | **Bitpack** | Structured, few bits active → ~8-17B | Random → all bits active → no savings |
| 2 | **LZ77** (internal dictionary) | Repeated byte patterns → ~20-50B | Random → no matches → 64B raw |
| 3 | **LZ77 + Hilbert reorder** | Low entropy with spatial structure → ~15-40B | Medium+ entropy → skipped |
| 4 | **Raw** (64 bytes) | High entropy fallback | Always 64B |

**Entropy classifier** (`_chunk_entropy_class`): 16-bin histogram + absolute deviation
- Score 0 (low): try all 5 paths
- Score 1 (medium): skip Hilbert reorder
- Score 2 (high): raw store immediately

**Shell classification** (`shell_classify_score`): popcount + unique count - variance normalization → determines which shell level (0-8) the chunk belongs to.

### KIS v6 Codec — Weight-Level Position Encoding

Two-layer approach for Q8_0 weights:
1. **Codebook** (~550B): histogram of which Q8 values exist + counts
2. **Residuals**: per-slot difference between original position and sorted-position expectation

**Two modes:**
- **Mode 0 (varint)**: delta-encoded slot positions + XOR values — good for small N
- **Mode 1 (bitmap)**: 2592B bitmap + XOR values per differing slot — good for large N, ratio ≤ 1.13x

**Key formula:** `slot[i] = (i × 37) % 20736` — coprime stride ensures bijective mapping.

---

## 3. Where They Lose to Entropy

### The Core Problem: Q8_0 Entropy Floor

From `geo_sid_loader_report.md`:
```
Block[0]: entropy 2.68, 73% zero → metadata
Block[1+]: entropy 7.2–7.4 → Q8_0 weight data
File size: 669,769,072B (638.7 MB), ratio GGUF/GEO = 0.9912
```

**Q8_0 weights have 7.2-7.4 bits of entropy per weight** (out of 8 bits maximum). This means:
- Only 7-10% of bits are redundant
- Shannon limit: minimum 0.90-0.925 bytes/weight
- The GEO format currently achieves ~1.0x (no compression)

### Specific Entropy Failure Points

#### A. Diamond Field v4: High-Entropy Chunks Go Raw

The entropy classifier (`_chunk_entropy_class`) short-circuits to raw storage when:
- `uniq > 10` (unique 16-bins) AND `abs_dev > 1800` (high deviation)

For Q8_0 weight blocks, most chunks hit this threshold. The 64-byte chunks in NN weight tensors are typically high-entropy — they're quantized residuals, not structured data.

**Impact:** The most common case (high entropy) gets zero compression.

#### B. KIS v6: Bitmap Overhead Dominates

Measured ratios from `kis_codec_v6_standalone_test`:

| Data | Raw | Codec | Ratio | Verdict |
|------|-----|-------|-------|---------|
| All same (42) | 1000B | 2639B | **2.64x** | Overhead kills small data |
| Alternating ±1 | 1000B | 3141B | **3.14x** | Overhead kills small data |
| Random Q8 (10K) | 10000B | 12843B | **1.28x** | 28% EXPANSION |
| One grid (20736) | 20736B | 23544B | **1.14x** | 14% EXPANSION |
| **Random Q8 (1M)** | 1000000B | 1123690B | **1.12x** | **12% EXPANSION** |

**The KIS v6 codec EXPANDS random data by 12%.** The fixed overhead is:
- Codebook: ~550B
- Bitmap: 2592B per chunk (V6_BM_BYTES = 324 × 8)
- For random data, almost every slot differs → bitmap nearly full + every XOR value stored

#### C. GEO File Format: Near-Zero Benefit

```
GGUF: 638.7 MB (Qwen2.5-0.5B-Q8_0)
GEO:  638.7 MB (ratio = 0.9912)
```

The GEO format transforms data geometrically but doesn't compress it. The `FrustumBlock` structure (2B scale + 54 DiamondBlocks × 64B + 1440B metadata) is roughly the same size as the input.

#### D. Diamond Field: Adaptive Routing Can't Beat Entropy

The diamond field tries 5 methods per chunk, but for Q8_0 weight data:
- Sparse: rarely selected (chunks have many unique values)
- Bitpack: rarely beats raw (most bits are meaningful)
- LZ77: internal dictionary is too small for random-ish data
- Hilbert+LZ77: spatial reorder helps structured data, not random
- **Raw: the default for most weight chunks**

The diamond field is excellent for **structured data** (metadata, sparse tensors, repeated patterns) but the entropy classifier correctly identifies weight chunks as high-entropy → raw.

---

## 4. How KIS v4/v5/v6 Could Improve

### KIS v4 (Codebook + Permutation)

**Current approach:** Sort weights → codebook (histogram) → delta-encoded permutation of sorted indices.

**Improvement opportunity:** The permutation encoding is O(N log N) for sort + O(N) for delta encoding. For high-entropy data, the permutation is nearly random → deltas are large → varint expansion.

**Fix:** For high-entropy data, skip permutation entirely. Just store codebook + raw sorted values. The codebook itself provides the only compression (value elimination).

### KIS v5 (Angular Wavelet + Sparse Residual)

**Current approach:** Map each Q8 code to an angular position (θ, φ) on a 144×144 grid. Store only residuals where original grid ≠ expected grid.

**Improvement opportunity:** The angular map `v5_angular(code, &th, &ph)` is deterministic based on code value. For high-entropy data, many codes map to the same grid cell → collisions → residuals are large.

**Fix:** Use a learnable/per-model angular map instead of the fixed formula. Fit the angular map to minimize collisions for the specific model's weight distribution.

### KIS v6 (Index-Based Mapping)

**Current approach:** `slot[i] = (i × 37) % 20736` — each weight gets a unique slot regardless of value.

**Improvement opportunity:** The bitmap overhead (2592B per 20736-slot chunk) is the dominant cost. For random data, nearly every slot differs.

**Three concrete fixes:**

1. **Variable bitmap density:** Instead of full 2592B bitmap, use run-length encoded bitmap. For random data, the bitmap is ~50% set → RLE could compress it. For structured data, bitmap is sparse → RLE compresses well.

2. **Entropy-adaptive mode selection:** When entropy > 7.0 bits/weight, skip the codebook entirely and use a simpler format: just codebook + raw values. The permutation/residual encoding doesn't help when the data is genuinely random.

3. **Chunked entropy encoding:** Instead of one global codebook, use per-chunk (64-weight) codebooks. NN weight tensors have local structure (adjacent weights in a matrix row are correlated). Per-chunk codebooks exploit this local structure.

---

## 5. What "Daily Use Filesystem Compression" Needs to Achieve

### Target Benchmarks

| Metric | zstd level 3 (speed) | zstd level 19 (ratio) | Target for geo codec |
|--------|---------------------|----------------------|---------------------|
| **Compression ratio** | ~0.88-0.92x | ~0.85-0.90x | **≤ 0.88x** (match zstd-3) |
| **Encode speed** | ~500 MB/s | ~50 MB/s | **≥ 200 MB/s** |
| **Decode speed** | ~1500 MB/s | ~500 MB/s | **≥ 500 MB/s** |
| **Memory** | ~几MB | ~几十MB | **≤ 8 MB** |

### The Q8_0 Entropy Constraint

- Q8_0 entropy: 7.2-7.4 bits/weight → Shannon limit: 0.90-0.925 bytes/weight
- zstd level 3 achieves ~0.88-0.92x on real Q8_0 data (exploits inter-byte correlations)
- **Geometric codec must beat zstd-3 to justify its complexity**

### What's Missing in Current Geofield/Geopixel

1. **No inter-chunk compression:** Each 64-byte chunk is encoded independently. Real weight tensors have correlations between adjacent chunks (same matrix row, similar scale factors). A codec that exploits inter-chunk redundancy could achieve better ratios.

2. **No scale factor compression:** Q8_0 stores 2-byte scale + 32 bytes per block. The scale factors across blocks are highly correlated (smooth variation within a layer). Delta-encoding scales alone could save 20-30% on the metadata.

3. **No cross-tensor compression:** Different tensors in the same layer share statistical properties. A codec that uses one tensor's codebook to predict another's could save significant header space.

4. **Fixed overhead too high:** The KIS v6 bitmap (2592B per chunk) and codebook (~550B) create a fixed floor. For a 7B model at Q8_0 (~7GB), the per-chunk overhead adds ~10-15% to the output size, negating any compression benefit.

### Practical Recommendation

For a "daily use filesystem compression" targeting Q8_0 weight data:

**Hybrid approach:**
1. **Low entropy (score < 64):** Use geometric encoding (diamond field + KIS v6) — this is where geometry shines
2. **Medium entropy (64-192):** Use KIS v6 with entropy-adaptive mode (skip bitmap, use varint residuals only)
3. **High entropy (>192):** Use **zstd level 3** directly — the geometric overhead exceeds the compression benefit

This "geometric + zstd fallback" approach would achieve:
- Best of both worlds: geometric for structured data, zstd for random data
- Overall ratio: ~0.85-0.90x (matching zstd-19 on high-entropy, beating it on structured)
- Speed: ~200-500 MB/s (geometric paths are O(1), zstd-3 is fast)
- Memory: ~4-8 MB (small working set per chunk)

---

## 6. Summary: The Entropy Wall

```
                    Entropy Spectrum
    ◄── Structured ──────────────── Random ──►
    
    0 bits                          8 bits/weight
    │                                  │
    ├─ Geofield shines ◄─────────► Geofield raw (0% compression)
    ├─ KIS v6 effective ◄───────► KIS v6 expands (+12%)
    ├─ zstd-3 effective ◄───────► zstd-3 still compresses (~0.92x)
    │                                  │
    └──────────── THE WALL ────────────┘
         (~7 bits/weight = Shannon limit)
```

**Bottom line:** The geometric approach (geofield + geopixel/KIS v6) is excellent for structured data but hits a wall at ~7 bits/weight entropy. For daily-use filesystem compression, the system needs a **hybrid codec** that uses geometry where it helps and falls back to zstd where geometry can't compete. The target: ≤0.88x ratio at ≥200 MB/s encode speed, matching zstd-3's practical performance on Q8_0 data.
