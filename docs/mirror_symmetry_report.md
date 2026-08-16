---
luminaCreated: 2026-08-16T06:55:01.910Z
tags: []
luminaModified: 2026-08-16T06:55:01.910Z
luminaVersion: 1.3.11
---
# Q8_0 Mirror Symmetry Analysis — Results

## Executive Summary

**Mirror symmetry is NOT present in real Q8_0 neural network weights.**
The 8-octant dedup architecture (store 1 octant, mirror formula = 8 views)
produces ~1.0x compression — zero benefit from mirror operations.

## Test File
`tests/test_kis_mirror_symmetry.c` — builds and runs on any Q8_0 GGUF.

## Raw Results (3 models)

| Model | X-axis | Y-axis | Z-axis | Avg | Est. Ratio |
|-------|--------|--------|--------|-----|-----------|
| Qwen2.5-0.5B-Q8_0 | 0.00% | 0.03% | 0.00% | 0.01% | ~1.0x |
| SmolLM2-360M-Q8_0 | 0.09% | 0.20% | 0.00% | 0.10% | ~1.0x |
| Qwen3-0.6B-Q8_0 | 0.03% | 0.00% | 0.00% | 0.01% | ~1.0x |

## Why Mirror Symmetry Fails

### The Math
Mirror flip: `slot → (KIS_AXIS - slot) % KIS_AXIS` within one axis.
This maps weight[i] to weight[j] where i and j are unrelated indices.

### Root Cause
1. **Neural network weights are high-entropy** (7.2-7.4 bits/weight entropy)
2. **KIS 3-axis mapping** (data[i], data[(i+1728)%n], data[(i+3456)%n]) creates
   3 independent views, NOT geometric mirrors
3. **Mirror flip is an address operation, not a value operation** — it remaps
   which slot a weight occupies, but weights at mirrored addresses are
   independent values from different positions in the weight tensor

### Entropy Argument
- Q8_0 weights have ~7.2 bits entropy per weight
- Random pair coincidence rate: ~0.4% (1/256 values match by chance)
- Measured: 0.01-0.10% — matches random baseline
- Mirror symmetry requires structured redundancy that doesn't exist in NN weights

## What Actually Works for Compression

| Strategy | Ratio | Mechanism |
|----------|-------|-----------|
| Mirror 8-octant (this test) | 1.0x | ❌ No symmetry in weights |
| KIS scale compression | 1.0-10x | ✅ Scale compresses angular space |
| Sort + mask codebook (FGLS) | ~1.5-2x | ✅ Reorder + sparse mask |
| Diamond field v4 adaptive | 1.5-3x | ✅ 5-path entropy classifier |
| Standard quantization (Q4/Q8) | 2-4x | ✅ GGML built-in |

## Conclusion

The "8x dedup via mirror views" hypothesis from the tesseract container
architecture does NOT hold for real Q8_0 data. The geometric address space
(KIS 3-axis, 20736 slots) is valuable for *addressing* and *routing*, but
mirror operations on weights produce random-looking results with no redundancy.
