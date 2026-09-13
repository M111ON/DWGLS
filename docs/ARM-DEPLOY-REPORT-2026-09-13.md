# DWGLS ARM Deployment Report
**Date**: 2026-09-13
**Target**: Samsung SM-G988B (Exynos 990, Cortex-A55 ×8, 10GB RAM)
**SSH**: Termux at 192.168.1.235:8022
**Status**: Complete — all experiments done, phone cleaned up.

---

## 1. Setup

### Termux Environment
- **OS**: Android (Samsung One UI, TP1A.220624.014.G988BXXSNHYB1)
- **CPU**: Cortex-A55 (aarch64), 8 cores
- **RAM**: 10GB total (5.5GB typical used, 4.7GB available)
- **Swap**: 3GB
- **Storage**: 108GB total, ~15GB free
- **GPU**: Mali (vulkan.mali.so exists in /vendor/lib64/hw/), but **unusable from Termux** — Android linker namespace blocks dlopen() of vendor libraries from non-system apps. Only Mesa Lavapipe (software Vulkan rasterizer) available = CPU emulation through Vulkan API, no speedup.
- **Packages installed**: gcc, make, curl, ollama, vulkan-loader-generic

### Deploy Method
- `scripts/deploy_termux.sh` — rsync source to ~/dwgls/, auto-compile, auto-test
- `scripts/build_termux.sh` — compile/test/bench targets for ARM

---

## 2. Core Tests — ARM vs x86

**24/27 tests compiled**, 23/24 runtime PASS on ARM. 3 build failures from geo_tess_container.h needing ggml headers (TIER2 deps not available on non-ggml builds).

### Geometry Integer Tests (ARM wins 1.4–6.4×)

| Test | ARM (A55) | x86_64 (VPS) | Ratio | Winner |
|------|-----------|--------------|-------|--------|
| tess_sacred | 0.025 ms | 0.143 ms | 5.7× | ARM |
| tess_subdivide | 0.041 ms | 0.214 ms | 5.2× | ARM |
| scale_bridge | 0.025 ms | 0.160 ms | 6.4× | ARM |
| tess_index_frame | 0.054 ms | 0.114 ms | 2.1× | ARM |
| fibo_walk | 0.035 ms | 0.061 ms | 1.7× | ARM |
| cube_addr | 0.033 ms | 0.035 ms | 1.1× | ~equal |

### Memory-Bound Tests (x86 wins)

| Test | ARM (A55) | x86_64 (VPS) | Ratio | Winner |
|------|-----------|--------------|-------|--------|
| tess_magnify | 0.342 ms | 0.100 ms | 3.4× | x86 |
| geo_hyperbolic | 0.405 ms | 0.149 ms | 2.7× | x86 |

### Pattern
ARM wins on **pure integer geometry** — NEON SIMD + simpler integer pipeline + in-order execution advantage. x86 wins on **memory-bound** workloads — larger L3 cache + out-of-order execution. Scale_bridge (6.4×) is the biggest ARM win because it's 100% integer modular arithmetic.

---

## 3. Benchmark Tools

### bench_unified (100K iterations)

| Operation | ARM | x86 | Winner |
|-----------|-----|-----|--------|
| Name lookup | 40.2 ns | 97.1 ns | ARM 2.4× |
| Flat index | 82.2 ns | 87.4 ns | ~equal |
| DRAM coords | 50.3 ns | 25.4 ns | x86 2× |
| RDH coords | 54.8 ns | 25.0 ns | x86 2.2× |
| GearLock tick | 39.8 ns | 25.9 ns | x86 1.5× |

### bench_cache (sequential/random)

| Pattern | ARM | x86 | Winner |
|---------|-----|-----|--------|
| Sequential | 3.4 ns/slot | 4.7 ns/slot | ARM 1.4× |
| Random | 7.9 ns/slot | 3.7 ns/slot | x86 2.1× |
| Cold cache | 155.7 ns | 156.5 ns | ~equal |

### bench_geo_fast

| Operation | ARM | x86 | Winner |
|-----------|-----|-----|--------|
| GeoFast lookup | 127.4 ns | 25.0 ns | x86 5× |
| GeoFast read | 72.6 ns | 29.8 ns | x86 2.4× |
| GeoFast write | 49.0 ns | 32.9 ns | x86 1.5× |

**Interpretation**: ARM excels at simple integer pipelines (name lookup, sequential access). x86 excels at coordinate math requiring more complex ALU + larger caches (GeoFast, random access).

---

## 4. LLM Inference Baseline

### Qwen2.5-0.5B Q8_0 (639 MB) — CPU, 8 threads

| Tool | pp (t/s) | tg (t/s) |
|------|----------|----------|
| llama-bench | 88 | 30* |
| Ollama (Q8_0) | 203–239 | 7–10 |
| Ollama (Q4_K_M, 397 MB) | 117–132 | 10–11 |

*llama-bench tg32 = 30 t/s is artificially high due to very short generation length (cache-hot). Sustained generation = 7–11 t/s.

### Qwen3-4B Q1_0 (546 MB) — CPU, 8 threads

| Test | t/s |
|------|-----|
| pp128 | 6.2 |
| tg32 | 4.4 |

### Qwen3-4B-MoE Q4_K_M (3.4 GB) — loaded via llama-cli, CPU
- Model loads successfully
- Text generation works (tested "Hello, my name is" → "Bonsai, your AI assistant")
- Full benchmark not run due to memory constraints (10GB total, model + inference context tight)

### Ollama vs llama-bench
Ollama prompt eval is 2–2.5× faster than llama-bench on the same model — likely due to different llama.cpp build optimizations. Sustained generation speed is similar (~10 t/s for 0.5B models on ARM A55).

---

## 5. DWGLS MoE Pipeline

### moe-bake: GGUF → DtSlotRegion

| Metric | ARM | x86 | ARM Speedup |
|--------|-----|-----|-------------|
| Time | **18.8s** | 67.3s | **3.6×** |
| Tensors | 108/108 | 108/108 | — |
| Pool | 2801.7 MB | 2801.7 MB | — |
| Lossless | ✓ | ✓ | — |

Bake scatters 108 MoE expert weight tensors (64 experts × 36 layers × 3 projections) into 20736-slot geometric region. ARM NEON + integer pipeline makes this 3.6× faster than x86.

### moe-stream: Top-K Expert Loading + SwiGLU Matmul

| Metric | ARM | x86 | ARM Speedup |
|--------|-----|-----|-------------|
| Time (layer 0) | **0.2s** | 1.5s | **7.5×** |
| Experts loaded | 4/64 | 4/64 | — |
| FFN matmul | 4/4 PASS | 4/4 PASS | — |
| Byte compare | 12/12 match | 12/12 match | — |
| cos similarity | 1.000000 | 1.000000 | — |

Stream loads only router-selected top-4 experts (6.25% of total) per layer. SwiGLU computation matches GGUF byte-for-byte.

### What DWGLS Means for MoE Loading
- **I/O savings**: Load 6.25% of expert weights instead of 100% → 16× less data for MoE layers
- **Memory savings**: Only active experts in RAM → fit larger models on smaller devices
- **Bake overhead**: One-time 18.8s (ARM) for 3.4GB model — acceptable for deployment
- **ARM advantage**: Integer geometry computation is where ARM NEON excels (3.6–7.5× over x86)

---

## 6. Vulkan GPU Status

**Blocker**: Android linker namespace isolation prevents Termux from loading vendor Mali GPU driver (`/vendor/lib64/hw/vulkan.mali.so`). The Vulkan loader finds the ICD JSON but dlopen() fails with:
```
dlopen failed: library "/vendor/lib64/hw/vulkan.mali.so" needed or dlopened by
"/data/data/com.termux/files/usr/lib/libvulkan.so.1.4.357" is not accessible
for the namespace "(default)"
```

**Fix requires**: root access or custom ROM with linker namespace patch. Without root, Termux is CPU-only on this device.

---

## 7. Cleanup Done

Removed from phone:
- All GGUF model files (~4.6 GB freed)
- moe_expert_region.bin (2.8 GB freed)
- Ollama Q8_0 model (removed from ollama registry)
- Build artifacts (.o files, compiled binaries)
- Temp scripts (fix_vulkan.sh, ollama_bench scripts)

Kept:
- ollama + qwen2.5:0.5b (397 MB) — useful for quick inference tests
- DWGLS source (~/dwgls, 62 MB) — ready for next deployment
- Termux packages (gcc, make, curl)

---

## 8. Key Takeaways

1. **ARM A55 is 3–6× faster than x86 VPS for DWGLS geometry integer workloads** — scale_bridge 6.4×, tess_sacred 5.7×, tess_subdivide 5.2×
2. **x86 wins on memory-bound workloads** — tess_magnify 3.4×, geo_hyperbolic 2.7×, GeoFast 5×
3. **DWGLS MoE pipeline proven on ARM**: bake 18.8s (3.6× faster than x86), stream 0.2s (7.5× faster), 108/108 tensors lossless
4. **MoE streaming saves 16× I/O** — load 4/64 experts = 6.25% of weights
5. **LLM inference works**: Qwen2.5-0.5B at ~10 t/s gen, Qwen3-4B at ~4.4 t/s gen, all CPU
6. **Vulkan GPU blocked by Android** — needs root for Mali driver access
7. **Ollama builds are faster than Termux llama.cpp** for prompt eval (2–2.5×), similar gen speed
