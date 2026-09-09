# DWGLS Project Status — 2026-09-10

> **MAP not COMPRESS** — Geometry IS the address space. Coordinate = data.
> No hash, no lookup, no zero — everything deterministic + replayable.

---

## Executive Summary

DWGLS (4Dimension Geometry + KIS Timeline) is a geometric data compression pipeline for LLM inference. The system maps tensor weights into a 20736-slot address space using dodecahedral geometry, achieving lossless compression with zero-copy GPU access.

**Key achievement:** Full pipeline proven end-to-end — GGUF → tesspack → assembled GGUF → llama.cpp inference → identical tokens. GPU scatter bench achieves **47 GB/s** zero-copy and **63.56 GB/s** with compressed descriptors on NVIDIA T4.

---

## Pipeline Status

### ✅ Fully Proven (Lossless)

| Stage | Tool | Status | Evidence |
|-------|------|--------|----------|
| **Bake** (GGUF → .tess) | `tess-bake` | ✅ Lossless | 291/291 tensors, 1181 .tess files |
| **Pack** (GGUF → .tesspack) | `tess-gguf-pack` | ✅ Lossless | Single-file container, 43,596 capos |
| **Assemble** (.tesspack → GGUF) | `tesspack-assemble` | ✅ Lossless | Patches non-sequential offsets, byte-identical roundtrip |
| **Graft** (.tesspack → GGUF) | `tess-graft` | ✅ Lossless | mmap output, layer-order sort, 2.1× speedup |
| **Stream** (per-capo reader) | `tess-stream` | ✅ Lossless | Streaming decode, O(1) per capo |
| **Verify** (roundtrip check) | `tesspack-verify` | ✅ Lossless | CRC-64 + stride-37 scatter verify |
| **MoE pipeline** | `moe-bake/graft/stream/route` | ✅ Lossless | 108/108 tensors, expert routing |
| **Multi-format** | All 4 tools | ✅ Lossless | Kokoro-82M, Bonsai-4B Q1_0, Qwen3-VL-2B, LFM-8B |

### ✅ GPU Benchmarks (Colab T4)

| Method | 144B | 512B | 2048B | Notes |
|--------|------|------|-------|-------|
| Brute force (H2D + HBM) | 0.81 GB/s | 1.21 GB/s | 0.63 GB/s | Old way, H2D copy overhead |
| **DRamTile zero-copy** | 4.21 GB/s | 4.54 GB/s | **47.19 GB/s** | No H2D copy, pinned host memory |
| DRamTile + sig32 verify | slow | slow | slow | Small tesspack, PCIe random access |
| **Compressed 8B descriptors** | 3.13 GB/s | 15.47 GB/s | **63.56 GB/s** | sig32 XOR-fold halves PCIe bandwidth |

### ✅ CPU Zero-Copy

| Component | Status | File |
|-----------|--------|------|
| mmap → callback → t->data | ✅ Proven | `gguf_lazy_serve.c` |
| no_alloc + set_tensor_data | ✅ Proven | `llama-model.cpp` patches |
| E2E inference (assembled GGUF) | ✅ Identical tokens | Qwen2.5-0.5B Q8_0 |

---

## Architecture

### Core Geometry

| Constant | Value | Significance |
|----------|-------|--------------|
| **20736** | 12⁴ = 144² | Full address space (18 tesseracts × 8 cubes × 144 slots) |
| **144** | 16·9 | Slot count per cube (4²·3²) |
| **1728** | 12³ | Pipes × ticks in FiboSpine |
| **12** | 4×3 | Dodecahedron base, tetrahedron edges |
| **37** | Coprime with 144 | Stride for scatter (zero collisions) |

### Pipeline Chain

```
GGUF → tess-bake → .tess files → tess-gguf-pack → .tesspack
                                                        ↓
                                              tesspack-assemble → assembled GGUF
                                                        ↓
                                              llama.cpp inference → identical tokens
```

### GPU Chain

```
.tesspack → mmap → cudaHostRegister → pinned memory
                                           ↓
                                    GPU kernel reads directly (zero-copy)
                                           ↓
                                    DRamTile addressing (Hilbert 8×8)
                                           ↓
                                    sig32 XOR-fold verification
```

### Key Components

| Component | File | Purpose |
|-----------|------|---------|
| **DRamTile** | `core/infra/geo_dram_tile.h` | Anchor × 128 + Hilbert 8×8 addressing |
| **GearLock** | `core/infra/gear_lock.h` | CPU/GPU world counters (128 × 162 = 20736) |
| **FiboSpine** | `core/infra/fibo_spine.h` | 1728 pipes × 12 ticks + JetBridge |
| **Rail Hub** | `core/geo_rail_hub.h` | Tensor pull layer (131M pulls/s proven) |
| **GGUF Reader** | `core/gguf_reader.h` | mmap-based GGUF parser |

---

## Models Tested

| Model | Format | Size | Capos | Overhead | Lossless |
|-------|--------|------|-------|----------|----------|
| Kokoro-82M ONNX | Q8F16 | 86 MB | 1,257 | 0% | ✅ |
| Bonsai-4B Q1_0 | 1-bit | 546 MB | 6,275 | 36.5% | ✅ |
| Qwen3-VL-2B Q4_K_M | Q4_K_M | 1.1 GB | 536 | 24.5% | ✅ |
| LFM2.5-8B Q4_K_M | Q4_K_M | 4.8 GB | 1,871 | 5.4% | ✅ |
| Qwen3-0.6B Q8_0 | Q8_0 | 675 MB | — | — | ✅ |
| Qwen2.5-0.5B Q8_0 | Q8_0 | 675 MB | 291 | — | ✅ |

---

## Test Suite

| Tier | Tests | Status | Last Verified |
|------|-------|--------|---------------|
| TIER1 | 121/121 | ✅ PASS | 2026-09-05 |
| TIER2 | 4/4 | ✅ PASS | 2026-09-05 |
| Real-pack verify | 44,319 capos | ✅ 0 fail | 2026-09-05 |
| Scale bridge | 35/35 | ✅ PASS | 2026-09-05 |
| Hyp fusion E2E | 10/10 | ✅ PASS | 2026-09-10 |

---

## Tools Reference

| Tool | Command | Purpose |
|------|---------|---------|
| tess-bake | `make tess-bake` | GGUF → .tess files |
| tess-load | `make tess-load` | .tess → raw weights |
| tess-stream | `make tess-stream` | Streaming per-capo reader |
| tess-assemble | `make tess-assemble` | .tess + metadata → GGUF |
| tess-packer | `make tess-packer` | dir → .tesspack / unpack / info |
| tess-gguf-pack | `make tess-gguf-pack` | GGUF → .tesspack directly |
| tesspack-assemble | `make tesspack-assemble` | .tesspack → standalone GGUF |
| tesspack-verify | `make tesspack-verify` | Lossless roundtrip verify |
| moe-bake | `make moe-bake` | GGUF → DtSlotRegion |
| moe-graft | `make moe-graft` | DtSlotRegion → GGUF |
| moe-stream | `make moe-stream` | Streaming top-K experts |
| moe-route | `make moe-route` | Combined bake+route+graft |
| tess-graft | `make tess-graft` | .tesspack → graft GGUF |
| tess-view | `make tess-view` | .tesspack → assemble + verify |
| tess-breathe | `make tess-breathe` | mmap RSS measurement |

---

## Recent Commits (2026-09-10)

| Commit | Description |
|--------|-------------|
| `af2d957` | GPU zero-copy test + Colab deploy scripts |
| `56b71c3` | GPU bench numbers in AGENTS.md |
| `2a718f1` | GPU scatter v2 results — DRamTile 47 GB/s, compressed 63.56 GB/s |
| `c827d63` | Graft OOM fix + iso_user_path auto-detect backend |
| `b854447` | AGENTS.md update — tesspack_assemble, graft OOM, Colab done |
| `f14fca6` | tesspack_assemble fix — non-sequential GGUF offsets |
| `3b6e6db` | Scatter bench header format fix |
| `28d20df` | GNN+Fan24 tile connectivity + tesspack_assemble |

---

## Pending / Next Steps

| Item | Priority | Status |
|------|----------|--------|
| **Standalone CUDA scatter decode kernel** | High | Write CUDA kernel for tesspack → raw tensor decode (no llama.cpp dependency) |
| **GPU zero-copy integration** | High | CPU zero-copy proven, GPU scatter proven, full integration pending |
| **Colab CUDA build optimization** | Medium | llama.cpp+CUDA build takes 1-2h on Colab, need faster approach |
| **Larger tesspack GPU test** | Medium | Test with full model (18MB tesspack too small for accurate GPU bench) |

---

## Known Limitations

1. **GPU scatter bench Method C (sig32 verify)** slow on small tesspacks — random PCIe access pattern, need larger data for accurate measurement
2. **llama.cpp+CUDA build** on Colab takes 1-2 hours — blocks rapid iteration
3. **tesspack format** stores only scatter-encoded data — ONION (f32/f16) stored as raw blobs
4. **Compression overhead** varies by model: 0% (Kokoro) to 36.5% (Bonsai Q1_0)

---

## References

- `docs/PIPELINE-MAP.md` — Full pipeline diagram with mermaid
- `docs/PRODUCTION-READINESS-PLAN.md` — 5-phase production plan
- `docs/BENCH-GPU-SCATTER-V2-2026-09-10.md` — GPU benchmark results
- `AGENTS.md` — Working rules, sacred constants, tool reference
- `docs/tess-format-spec.md` — .tess file format specification
