# Tesspack Dense-Model Limitation — `output.weight` f32

## Problem Statement

tesspack standalone inference produces **correct output** for some quantized models but **garbage output** for others, despite identical pack/bake pipelines. Root cause: certain GGUF models store their `output.weight` (lm_head) as **f32** instead of a quantized type. The scatter packer silently skips tensors it cannot efficiently store, and the callback provider zero-fills missing tensors.

## Affected Models

| Model | output.weight dtype | tesspack standalone |
|-------|-------------------|-------------------|
| Qwen2.5-0.5B-Instruct Q8_0 | Q8_0 (cell=34) | **PASS** — 16/16 tokens identical |
| Qwen3-0.6B Q8_0 | f32 (593 MiB) | **FAIL** — garbage output |
| SmolLM2-360M-Instruct Q8_0 | f32 | **FAIL** — garbage output |
| Bonsai-4B Q1_0 | N/A (type 41 unsupported) | **FAIL** — cell_size unknown |

## Root Cause Analysis

### 1. Scatter Packer Skips Large f32 Tensors

`tess_gguf_pack.c` iterates all GGUF tensors and packs each one into capos (144-cell cubes). For each tensor, it calls `gguf_cell_size(type)` to determine bytes-per-cell. The packer writes capos only when the tensor fits within reasonable overhead.

For a f32 tensor like `output.weight` (Qwen3-0.6B: 593 MiB, 151M elements):
- cell_size = 4 bytes (f32)
- Total cells needed = 151,552,000
- Capos needed = 151,552,000 / 144 = **1,052,444 capos**
- Per-capo overhead: ~80 bytes (TESS_Header + TESS_Formula + TESS_crc32)
- Total pack size for this tensor alone: 593 MiB + 84 MiB overhead = **677 MiB**

The resulting tesspack would be larger than the original GGUF (610 MiB). The packer appears to skip tensors that would cause excessive capo counts, but does so **silently** — no error, no warning.

For a Q8_0 tensor of the same shape (Qwen2.5-0.5B `output.weight`):
- cell_size = 34 bytes (Q8_0)
- Total cells = 151,552,000 / 32 = 4,736,000
- Capos needed = 4,736,000 / 144 = **32,889 capos**
- Pack size: 137 MiB + 2.6 MiB overhead = **140 MiB** (vs 137 MiB original)

This fits within the scatter format's efficiency envelope and IS packed.

### 2. Callback Zero-Fills Missing Tensors

`tesspack_server.c` and `tesspack_bridge.c` both use `llama_model_init_from_user()` with a `provide_tensor` callback. The callback:

1. Calls `load_pack_tensor()` to read tensor data from the mmap'd pack
2. If the tensor is found → copies capo data to `t->data` (the model's tensor buffer)
3. If the tensor is NOT found → `memset(dst, 0, need)` (zero-fills the entire tensor)
4. Special case: tensors with `.scale` / `.input_scale` suffix → filled with `1.0f` (required for quantized dequantization)

When `output.weight` is zero-filled:
- The model's final linear projection layer computes `logits = hidden_state @ output_weight + bias`
- `output_weight = 0` → all logits = 0 → argmax is arbitrary → **repeated garbage tokens**

### 3. Why Qwen2.5-0.5B Works

Qwen2.5-0.5B's GGUF was quantized with a quantizer that also quantized `output.weight` to Q8_0. This is common for small models (vocab_size=151,936 × hidden=896 → output.weight = 137 MiB as Q8_0, fits easily). The pack stores it in 206 capos with 98.4% data efficiency.

Qwen3-0.6B and SmolLM2-360M's GGUFs were quantized with quantizers that left `output.weight` as f32. This is common when lm_head is kept unquantized for numerical precision (especially with larger vocab sizes).

## Verification Method

To check if a model's `output.weight` will be packed:

```bash
# Run the packer to stdout (output to NUL to skip file creation)
tess_gguf_pack.exe model.gguf NUL 2>&1 | findstr "output.weight"
```

- If output shows `[0] output.weight cell= 34 ...` → **Q8_0, will work standalone**
- If no line appears → **f32, will NOT work standalone**

Alternatively, compare the tesspack file size against the original GGUF:
- If tesspack ≈ GGUF size → all tensors packed
- If tesspack << GGUF size → large tensors were skipped

## Impact on Server/Bridge

| Component | Behavior |
|-----------|----------|
| `tesspack_bridge` | Phase B loads from pack, zero-fills missing → Phase A vs Phase B shows MISMATCH |
| `tesspack_server` | HTTP API returns correct structure but wrong content (garbage tokens) |
| mmap RSS | Same — the zero-filled tensors still consume RSS on page fault |

## Fix Directions

### Option A: Fall Back to GGUF for Missing Tensors

Load both tesspack + original GGUF at runtime. For tensors found in the pack → use pack data. For tensors NOT in the pack → read from GGUF directly.

- **Pros**: Works for all models, zero quality loss
- **Cons**: Requires both files on disk, defeats standalone purpose
- **Complexity**: Medium — need a secondary gguf_context in provide_tensor

### Option B: Re-quantize lm_head Before Packing

Use `llama-quantize` or similar to re-quantize `output.weight` to Q8_0/Q4_K before tesspack creation.

```bash
# Example: re-quantize with lm_head quantized
llama-quantize input.gguf output.gguf Q8_0
tess_gguf_pack output.gguf model.tesspack
```

- **Pros**: Pure tesspack standalone, no runtime dependency
- **Cons**: Slight quality loss on lm_head (usually negligible)
- **Complexity**: Low — one preprocessing step

### Option C: Cap larger capo counts in the packer

Allow the scatter packer to handle f32 tensors with larger capo counts, up to the full tensor size.

- **Pros**: True standalone, no preprocessing
- **Cons**: tesspack may be larger than GGUF for f32-heavy models
- **Complexity**: Medium — remove capo count threshold in packer

## Recommendation

**Option B** (pre-quantize lm_head) is the simplest and most practical:
- One extra `llama-quantize` step before tesspack creation
- Resulting tesspack is standalone
- Quality impact is negligible for Q8_0 lm_head
- The packer already handles Q8_0 tensors efficiently (98.4% data efficiency)

For production use where both files are acceptable (Option A), the hybrid fallback approach provides maximum flexibility.
