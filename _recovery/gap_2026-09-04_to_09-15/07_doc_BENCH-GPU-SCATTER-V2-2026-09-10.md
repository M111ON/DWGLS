# GPU Scatter Bench v2 — Colab T4 Results
# Date: 2026-09-10
# GPU: Tesla T4 (sm_75), 15.6 GB VRAM
# Tesspack: bonsai-4b-q1_0 (18 MB, 146 capos)

## Results

| Method | 144B | 512B | 2048B |
|--------|------|------|-------|
| A) Brute force (H2D + HBM) | 0.81 GB/s | 1.21 GB/s | 0.63 GB/s |
| B) DRamTile zero-copy (no H2D) | 4.21 GB/s | 4.54 GB/s | 47.19 GB/s |
| C) DRamTile + sig32 verify | 0.00 GB/s | 0.00 GB/s | 0.00 GB/s |
| D) Compressed 8B descriptors | 3.13 GB/s | 15.47 GB/s | 63.56 GB/s |

## Key Findings
- DRamTile zero-copy reaches 47 GB/s at 2048B chunks (no H2D copy needed)
- Compressed descriptors achieve 63.56 GB/s — sig32 XOR-fold halves PCIe bandwidth
- Method C (sig32 verify) extremely slow (214ms for 146 pulls) — likely atomic contention in error counter
- Brute force bottlenecked by H2D copy overhead

## Next Steps
- Fix Method C atomic contention bug
- Test with larger tesspack (full model) for realistic throughput numbers
- Integrate DRamTile zero-copy path into llama.cpp inference pipeline
