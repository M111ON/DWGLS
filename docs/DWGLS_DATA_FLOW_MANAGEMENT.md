# DWGLS — Data Flow Management System
## Comprehensive Technical Documentation

**Date:** 2026-08-28
**Status:** Phase 3 COMPLETE — All core proofs validated
**Model:** Qwen2.5-0.5B-Instruct-Q8_0.gguf (verified on llama.cpp b9733)

---

## Table of Contents

1. [Executive Summary](#1-executive-summary)
2. [Architecture Overview](#2-architecture-overview)
3. [Proven Technical Properties](#3-proven-technical-properties)
4. [Geometric Addressing Layer](#4-geometric-addressing-layer)
5. [Data Flow Patterns](#5-data-flow-patterns)
6. [KV Buffer Structure](#6-kv-buffer-structure)
7. [Container Format](#7-container-format)
8. [API Reference](#8-api-reference)
9. [Test Results](#9-test-results)
10. [Design Constraints](#10-design-constraints)
11. [Known Limitations](#11-known-limitations)
12. [Future Work](#12-future-work)

---

## 1. Executive Summary

DWGLS (4Dimension Geometry + KIS Timeline) is a **data flow management system** that uses the KV cache buffer of transformer models as a persistent, zero-overhead storage layer.

### What DWGLS Is

- A system for managing how data flows into, through, and out of a model's KV cache
- Uses "dead" (unused) positions in the KV buffer as safe storage zones
- Provides lossless roundtrip: write data → read back → byte-identical
- Zero impact on model inference (KL divergence = 0.0)

### What DWGLS Is NOT

- NOT a geometry system (geometry is a language for describing data flow rules)
- NOT a compression system (data is stored losslessly, not compressed)
- NOT a model modification (works with unmodified llama.cpp)

### Key Numbers

| Property | Value |
|----------|-------|
| Storage capacity | ~24 MB (Qwen2.5-0.5B, 2048 context) |
| Inference impact | KL = 0.0, max logit diff = 0.0 |
| Roundtrip fidelity | 0 bytes differ (lossless) |
| Geometric slots | 144 (6ico compound) |
| Scatter stride | 37 (coprime to 144) |
| Layers supported | All 24 layers |
| Context size | 2048 tokens |

---

## 2. Architecture Overview

### 2.1 Core Principle

> **MAP not COMPRESS** — Geometry IS the address space.
> Coordinate = data. No hash, no collision, no lookup table.

### 2.2 System Layers

```
Layer 4: Data Flow Management
  ├── Session State Persistence
  ├── Multi-Prompt Persistence
  └── Mid-Generation Checkpoint

Layer 3: Container Format (DWGL v1)
  ├── 21-byte header (magic, version, metadata)
  ├── FNV-1a checksum
  └── Payload data

Layer 2: Geometric Addressing
  ├── Stride-37 scatter (144 → 144 unique positions)
  ├── Bidirectional mapping (geo ↔ KV position)
  └── Multi-layer encoding (all 24 layers)

Layer 1: KV Buffer Access
  ├── Direct pointer access to KV cache tensors
  ├── Dead slot identification ([n_pos+1, n_ctx))
  └── Lossless read/write

Layer 0: llama.cpp Integration
  ├── C API (llama.h)
  ├── KV cache memory access
  └── Batch processing
```

### 2.3 Data Flow Map

```
External Data
     ↓ (write to dead slots)
KV Buffer [per-context]
     ↓ (extract to buffer)
External Buffer [persistent]
     ↓ (reinject into new context)
New KV Buffer [new context]
     ↓ (read back)
External Code
```

---

## 3. Proven Technical Properties

### 3.1 Dead Slot Safety

**Property:** Writing to dead KV positions does not affect model inference.

**Evidence:**
- KL divergence: 0.00000000
- Top-5 token overlap: 5/5
- Maximum logit difference: 0.0000000000

**Test:** `kv_impact_test.c` — Inject random data into 100-2000 dead positions, compare logits with baseline.

**Mechanism:** Model attention only reads positions [0, n_pos). Dead positions [n_pos+1, n_ctx) are never attended to.

### 3.2 Lossless Roundtrip

**Property:** Data written to KV buffer can be read back byte-identical.

**Evidence:**
- 144/144 geometric slots: 0 bytes differ
- 240/240 multi-layer tests: 0 bytes differ
- Container format: checksum verification passes

**Test:** `kv_geo_addr_test.c`, `kv_container_test.c`

### 3.3 Geometric Addressing

**Property:** Stride-37 scatter provides unique, collision-free, invertible mapping.

**Evidence:**
- 144/144 inverse mapping correct
- 0 collisions across 144 positions
- Bidirectional: geo → KV → geo = identity

**Test:** `kv_geo_addr_test.c`

### 3.4 Multi-Layer Storage

**Property:** Data can be stored in ALL layers simultaneously.

**Evidence:**
- 24 layers × 10 geo slots = 240/240 lossless
- Each layer stores independently

**Test:** `kv_geo_addr_test.c` (test 5)

### 3.5 Session Persistence

**Property:** Data survives context destruction via extract/reinject pattern.

**Evidence:**
- Write to context A → extract → destroy A → create B → reinject → readback match

**Test:** `kv_flow_demo.c` (flow 1)

### 3.6 Multi-Prompt Persistence

**Property:** Data flows across different inference contexts.

**Evidence:**
- Write in prompt A → extract → create prompt B → reinject → readback match

**Test:** `kv_flow_demo.c` (flow 2)

### 3.7 Checkpoint Integrity

**Property:** Mid-generation checkpoint produces identical output to one-shot generation.

**Evidence:**
- Path A: 10 tokens one-shot
- Path B: 5 tokens → checkpoint → 5 tokens
- Generation output: identical

**Test:** `kv_flow_demo.c` (flow 3)

---

## 4. Geometric Addressing Layer

### 4.1 Stride-37 Scatter

The universal scatter stride for DWGLS addressing.

```c
// Forward: geo slot → KV position
int geo_to_kv(int geo_slot, int dead_start, int dead_count) {
    return dead_start + (geo_slot * 37) % dead_count;
}

// Inverse: KV position → geo slot
int kv_to_geo(int kv_pos, int dead_start, int dead_count) {
    int rel = kv_pos - dead_start;
    int inv = 1;
    for (int i = 1; i < dead_count; i++) {
        if ((37 * i) % dead_count == 1) { inv = i; break; }
    }
    return (rel * inv) % dead_count;
}
```

### 4.2 Why Stride-37?

| Property | Value |
|----------|-------|
| gcd(37, 144) | 1 (coprime) |
| Coverage | All 144 positions |
| Collisions | 0 |
| Invertible | Yes (37⁻¹ exists mod 144) |
| Uniform distribution | Yes |

### 4.3 Geometric Constants

| Constant | Value | Meaning |
|----------|-------|---------|
| GEO_SLOTS | 144 | 6ico compound vertices |
| STRIDE_37 | 37 | Universal scatter stride |
| KV_STRIDE | 256 | Bytes per (layer, position) |
| MAX_LAYERS | 32 | Maximum layers supported |

### 4.4 Mapping Table (144 slots)

```
Geo Slot 0  → KV Position 0
Geo Slot 1  → KV Position 37
Geo Slot 2  → KV Position 74
Geo Slot 3  → KV Position 111
Geo Slot 4  → KV Position 148
...
Geo Slot 143 → KV Position 5331 (mod dead_count)
```

---

## 5. Data Flow Patterns

### 5.1 Session State Persistence

**Use Case:** Store conversation state across sessions.

```c
// Session A: Write state
write_dead(&ctx_a, dead_start, dead_start + slots, state_data, state_len);
uint8_t *saved = extract_dead(&ctx_a, dead_start, slots);
destroy(ctx_a);

// Session B: Restore state
ctx_b = create_context(prompt_b);
write_dead(&ctx_b, dead_start, dead_start + slots, saved, slots * k_stride);
free(saved);
```

### 5.2 Multi-Prompt Persistence

**Use Case:** Share data between different prompts.

```c
// Prompt A: Compute and store answer
run_t ra = make_run("What is the meaning of life?");
write_dead(&ra, dead_a, ..., answer, answer_len);
uint8_t *saved = extract_dead(&ra, dead_a, slots);
destroy(ra);

// Prompt B: Use A's answer
run_t rb = make_run("Explain quantum physics");
write_dead(&rb, dead_b, ..., saved, slots * k_stride);
// Now B has access to A's answer
```

### 5.3 Mid-Generation Checkpoint

**Use Case:** Save/restore generation state.

```c
// Generate 5 tokens
for (int i = 0; i < 5; i++) do_decode(&r);

// Checkpoint: save active KV to dead slots
checkpoint_kv(&r, dead_cp, cp_slots);

// Continue generating
for (int i = 0; i < 5; i++) do_decode(&r);

// Restore from checkpoint (if needed)
restore_kv(&r, dead_cp, cp_slots);
```

---

## 6. KV Buffer Structure

### 6.1 Memory Layout

```
KV Buffer (per layer, per type K/V):
┌─────────────────────────────────────────────────────────┐
│ Position 0  │ Position 1  │ ... │ Position 2047        │
│ [256 bytes] │ [256 bytes] │     │ [256 bytes]          │
└─────────────────────────────────────────────────────────┘
     ↑                ↑                    ↑
     Active zone      Active zone         Dead zone
     [0, n_pos)       [0, n_pos)          [n_pos+1, n_ctx)
```

### 6.2 Tensor Dimensions (Qwen2.5-0.5B)

| Property | K Tensor | V Tensor |
|----------|----------|----------|
| Shape | [128, 2048, 1, 1] | [128, 2048, 1, 1] |
| Type | f16 | f16 |
| Stride per position | 256 bytes | 256 bytes |
| Total size | 524,288 bytes | 524,288 bytes |
| Layers | 24 | 24 |

### 6.3 Dead Zone Calculation

```c
dead_start = n_pos + 1;           // After last active position
dead_count = n_ctx - dead_start;  // Available positions
// Qwen2.5-0.5B: dead_count = 2048 - (prompt_tokens + 1)
// After 1 token: dead_count = 2046
// After 10 tokens: dead_count = 2037
```

### 6.4 Storage Capacity

```c
total_bytes = dead_count × layers × 2 (K+V) × stride
// Qwen2.5-0.5B after 1 token:
// = 2046 × 24 × 2 × 256 = 25,153,536 bytes ≈ 24 MB
```

---

## 7. Container Format

### 7.1 DWGL v1 Header

```
Offset  Size   Field           Description
──────  ─────  ──────────────  ──────────────────────────────
0       4      Magic           "DWGL" (0x44 0x57 0x47 0x4C)
4       1      Version         1
5       2      n_pos_w         Position at write time (uint16 LE)
7       2      n_ctx           Context size (uint16 LE)
9       2      n_layers        Number of layers (uint16 LE)
11      2      k_stride        Bytes per K position (uint16 LE)
13      4      payload_size    Payload bytes (uint32 LE)
17      4      checksum        FNV-1a of payload (uint32 LE)
──────  ─────  ──────────────  ──────────────────────────────
Total: 21 bytes
```

### 7.2 Container Layout

```
┌─────────────────────────────────────────────┐
│ Header (21 bytes)                           │
├─────────────────────────────────────────────┤
│ Payload (N bytes)                           │
├─────────────────────────────────────────────┤
│ Padding (zeros to fill slot)                │
└─────────────────────────────────────────────┘
```

### 7.3 Checksum Algorithm (FNV-1a)

```c
uint32_t fnv1a(const void *data, size_t n) {
    const uint8_t *p = (const uint8_t *)data;
    uint32_t h = 2166136261u;
    for (size_t i = 0; i < n; i++) {
        h ^= p[i];
        h *= 16777619u;
    }
    return h;
}
```

### 7.4 Storage Rules

1. Container MUST be written AFTER all inference (decode writes at n_pos)
2. Container stored in dead slots [n_pos+1, n_ctx) of ALL layers
3. Each layer gets identical copy (redundancy)
4. Checksum verified on read

---

## 8. API Reference

### 8.1 KV Buffer Access

```c
// Get memory handle
llama_memory_t mem = llama_get_memory(ctx);

// Get tensor for specific layer
struct ggml_tensor *k = llama_memory_kv_cache_get_layer_k(mem, layer);
struct ggml_tensor *v = llama_memory_kv_cache_get_layer_v(mem, layer);

// Access raw data
uint8_t *k_data = (uint8_t *)k->data;
size_t stride = k->nb[1];  // Bytes per position

// Read/write at position
size_t offset = position * stride;
k_data[offset + byte_index] = value;
```

### 8.2 Write Function

```c
void write_dead(run_t *r, int slot_start, int slot_end,
                const uint8_t *data, size_t data_len);
```

- Writes data to dead slots of ALL layers
- Handles partial slots (data_len < stride)
- Writes K tensor with data, V tensor with XOR'd data

### 8.3 Read Function

```c
void read_dead_k(run_t *r, int slot_start, int slot_end,
                 uint8_t *out, size_t out_len, int layer);
```

- Reads from K tensor of specified layer
- Returns raw bytes from dead slots

### 8.4 Geometric Mapping

```c
int geo_to_kv(int geo_slot, int dead_start, int dead_count);
int kv_to_geo(int kv_pos, int dead_start, int dead_count);
```

- Bidirectional mapping between geometric slots and KV positions
- Stride-37 scatter ensures uniform coverage

---

## 9. Test Results

### 9.1 kv_impact_test.c — Dead Slot Safety

| Test | Description | Result |
|------|-------------|--------|
| 1 | Inject 100 dead slots | KL=0.00000000, top5=5/5 ✅ |
| 2 | Inject 200 dead slots | KL=0.00000000, top5=5/5 ✅ |
| 3 | Inject 500 dead slots | KL=0.00000000, top5=5/5 ✅ |
| 4 | Inject 1000 dead slots | KL=0.00000000, top5=5/5 ✅ |
| 5 | Inject 1500 dead slots | KL=0.00000000, top5=5/5 ✅ |
| 6 | Inject 2000 dead slots | KL=0.00000000, top5=5/5 ✅ |
| 7 | Corrupt active 1% | top5=0/5 ❌ (expected) |
| 8 | Corrupt active 5% | top5=0/5 ❌ (expected) |

### 9.2 kv_container_test.c — Container Format

| Test | Description | Result |
|------|-------------|--------|
| 1 | Roundtrip after decode | PASS ✅ |
| 2 | Encode after 8 tokens | PASS ✅ |
| 3 | Large payload (1000 bytes) | PASS ✅ |
| 4 | Two containers | FAIL ❌ (known constraint) |
| 5 | Cross-prompt-length | PASS ✅ |

**Note:** Test 4 fails because subsequent decodes overwrite earlier containers (expected behavior).

### 9.3 kv_geo_addr_test.c — Geometric Addressing

| Test | Description | Result |
|------|-------------|--------|
| 1 | Inverse mapping | 144/144 OK ✅ |
| 2 | No collisions | 0 collisions ✅ |
| 3 | Geo roundtrip | 144/144 lossless ✅ |
| 4 | Inference unchanged | maxdiff=0.0 ✅ |
| 5 | Multi-layer encode | 240/240 lossless ✅ |

### 9.4 kv_flow_demo.c — Data Flow Patterns

| Test | Description | Result |
|------|-------------|--------|
| 1 | Session State Persistence | PASS ✅ |
| 2 | Multi-Prompt Persistence | PASS ✅ |
| 3 | Mid-Generation Checkpoint | PASS ✅ |

---

## 10. Design Constraints

### 10.1 Critical Rules

1. **Encode AFTER inference:** Container must be written after all decode steps. Each decode writes KV at position n_pos, overwriting [n_pos).

2. **KV buffer is per-context:** Data written to context A is lost when A is destroyed. Must extract to external buffer first.

3. **Dead zone starts at n_pos+1:** Position n_pos is written by the next decode. Data at [n_pos+1, n_ctx) is safe.

4. **Active zone is sacred:** Positions [0, n_pos) must never be modified. Even 1% corruption breaks inference.

### 10.2 Capacity Limits

| Constraint | Value | Notes |
|------------|-------|-------|
| Max context | 2048 | Qwen2.5-0.5B |
| Dead positions | 2046 | After 1-token prompt |
| Bytes per position | 256 | f16, 128 dims |
| Total capacity | ~24 MB | 2046 × 24 × 2 × 256 |

### 10.3 Performance

| Operation | Time | Notes |
|-----------|------|-------|
| Write 144 slots | <1 ms | Direct pointer access |
| Read 144 slots | <1 ms | Direct pointer access |
| Inference impact | 0 | No overhead |

---

## 11. Known Limitations

### 11.1 Context-Dependent Storage

- Data lives in KV buffer memory, not on disk
- Destroying context destroys data (must extract first)
- Each context has its own KV buffer allocation

### 11.2 Position Overwrite

- Each decode writes at position n_pos
- Data at [n_pos) is overwritten by next decode
- Container must be written after all inference

### 11.3 Model-Specific

- Tested on Qwen2.5-0.5B only
- Qwen3-0.6B crashes with llama.cpp b9733 (STATUS_STACK_BUFFER_OVERRUN)
- Qwen3.5-2B (GDN) has no extractable KV cache

### 11.4 No Direct Model Access

- Data in dead slots is NOT attended to by the model
- Must extract → re-inject as tokens for model to use
- Model cannot directly "see" stored data

---

## 12. Future Work

### 12.1 Immediate

- [ ] Cross-model validation (when llama.cpp build supports it)
- [ ] Disk persistence (serialize KV dead slots to file)
- [ ] Multi-sequence support (multiple sequences sharing dead slots)

### 12.2 Medium-Term

- [ ] LoRA adapter hot-swap via dead slots
- [ ] Context window extension (use dead slots as context overflow)
- [ ] Model state checkpoint/restore

### 12.3 Long-Term

- [ ] Integration with llama.cpp as official feature
- [ ] GPU support (CUDA/HIP KV cache access)
- [ ] Distributed inference (share dead slots across nodes)

---

## Appendix A: Build Instructions

### Prerequisites

- MSYS2 MinGW-w64 (gcc 8.1.0+)
- llama.cpp b9733 prebuilt binaries
- Qwen2.5-0.5B-Instruct-Q8_0.gguf

### Build Commands

```bash
# Set environment
set PATH=C:\msys64\mingw64\bin;I:\llama\llama-b9733-bin-win-vulkan-x64;%PATH%

# Build geometric addressing test
gcc -O2 -std=c11 -Wno-unused-parameter -Wno-sign-compare -Wno-format \
  -Icore -II:/llama/include -o build/kv_geo_addr_test.exe \
  tools/kv_geo_addr_test.c \
  I:/llama/llama-b9733-bin-win-vulkan-x64/llama.dll \
  I:/llama/llama-b9733-bin-win-vulkan-x64/ggml.dll \
  I:/llama/llama-b9733-bin-win-vulkan-x64/ggml-base.dll \
  I:/llama/llama-b9733-bin-win-vulkan-x64/ggml-cpu-x64.dll -lzstd -lm

# Build data flow demo
gcc -O2 -std=c11 -Wno-unused-parameter -Wno-sign-compare -Wno-format \
  -Icore -II:/llama/include -o build/kv_flow_demo.exe \
  tools/kv_flow_demo.c \
  I:/llama/llama-b9733-bin-win-vulkan-x64/llama.dll \
  I:/llama/llama-b9733-bin-win-vulkan-x64/ggml.dll \
  I:/llama/llama-b9733-bin-win-vulkan-x64/ggml-base.dll \
  I:/llama/llama-b9733-bin-win-vulkan-x64/ggml-cpu-x64.dll -lzstd -lm
```

### Run Commands

```bash
# Geometric addressing test
.\build\kv_geo_addr_test.exe I:\model\Qwen2.5-0.5B-Instruct-Q8_0.gguf "Hello"

# Data flow demo
.\build\kv_flow_demo.exe I:\model\Qwen2.5-0.5B-Instruct-Q8_0.gguf
```

---

## Appendix B: File Reference

| File | Purpose | Lines |
|------|---------|-------|
| `tools/kv_geo_addr_test.c` | Geometric addressing validation | ~350 |
| `tools/kv_flow_demo.c` | Data flow pattern demo | ~300 |
| `tools/kv_container_test.c` | Container format validation | ~445 |
| `tools/kv_impact_test.c` | Dead slot safety analysis | ~400 |
| `tools/kv_raw_hook.c` | Raw K/V access + delta encoding | ~450 |

---

## Appendix C: Glossary

| Term | Definition |
|------|------------|
| Dead slots | KV positions [n_pos+1, n_ctx) not used by model |
| Active slots | KV positions [0, n_pos) used by model attention |
| Stride-37 | Universal scatter stride (coprime to 144) |
| GEO_SLOTS | 144 geometric slots (6ico compound) |
| KV_STRIDE | 256 bytes per (layer, position) |
| DWGL v1 | Container format (21-byte header + payload) |
| FNV-1a | Non-cryptographic hash for checksum |
| Lossless | Byte-identical roundtrip (0 bytes differ) |

---

*Document generated by DWGLS Data Flow Management System*
*Last updated: 2026-08-28*
