# TESSPACK RESIDUAL REGION — Specification

**Version:** 1.0 · 2026-09-07
**Status:** Design document (not yet implemented)

---

## 1. Problem

Models like Qwen3-4B-MoE use **weight tying**: `output.weight` doesn't exist as a
separate tensor in the GGUF. It shares `token_embd.weight`. The `llama_model_init_from_user`
callback creates a phantom `output.weight` request (F32 type) but the pack has no source for it.

Current workaround in `tesspack_bridge.c:216-228`:
- Detects `output.weight` missing → patches `t->type` to Q8_0 → shares `token_embd.weight` pointer
- Hardcoded, fragile, doesn't handle type mismatches cleanly
- Crashes if buffer was already allocated as F32

Other exceptional cases: internal llama tensors (`*.scale`, `*.input_scale`), MoE routing, future
quantization format changes.

## 2. Design Principle

**No data duplication.** The residual region is a redirect/transform table only.
Source data lives in SCATTER or ONION regions. The residual tells the callback *where
to get it* and *how to transform it*.

## 3. File Format Addition

### 3.1 Header Extension (64 bytes → still 64 bytes)

The existing header uses 16 `uint32_t` slots (hdr[0]–hdr[15]). All 16 are occupied.
The residual region reuses **hdr[6]–hdr[7]** (previously zero/padding):

```
offset  field               bytes   notes
────────────────────────────────────────────────────────────
 0      magic               4       0x5450414B "TPAK"
 4      version             4       2 (bumped from 1)
 8      n_entries           4       scatter+onion+gguf entry count (unchanged)
12      index_offset        4       byte offset to index section
16      onion_data_offset   4       byte offset to onion blob (0 = none)
20      onion_data_size     4       byte size of onion blob
24      residual_offset     4       byte offset to residual entries (0 = none)
28      residual_count      4       number of residual entries (0 = none)
32–63   reserved            32      zero (for future use)
```

**Version bump:** hdr[1] = 2 signals the presence of residual fields. Version-1
readers ignore hdr[6]–hdr[7] (they were zero). Version-2 readers check hdr[6]
for the residual region.

### 3.2 File Layout (version 2)

```
[64-byte header]                   hdr[0..7] — now includes residual_offset + residual_count
[scatter capo data]                scatter-encoded capos (unchanged)
[gguf header blob]                 raw GGUF header (0..data_offset)
[onion data blob]                  f16 contiguous tensors
[scatter index entries]            name_len(1) + name(N) + capo_id(4) + offset(8) + size(4)
[gguf header entry]                __gguf_header__ entry (capo_id=0)
[onion index entries]              same format, capo_id=0xFFFFFFFF
[residual entries]                 ← NEW section, at hdr[6] offset
```

The residual section lives **after** the main index, so existing index walkers are
unaffected — they stop at `hdr[2]` entries and never read the residual section.

### 3.3 Backward Compatibility

- hdr[1]=1 → no residual fields (hdr[6..7] = 0). Existing readers work unchanged.
- hdr[1]=2 → residual fields present. Old readers ignore hdr[6..7].
- residual_offset=0 → no residual entries. Safe fallback.

## 4. Residual Entry Format

Each entry is a fixed 8-byte struct (no variable-length fields):

```c
#pragma pack(push, 1)
typedef struct {
    uint8_t  name_len;          /* length of tensor_name (max 255) */
    char     name[255];         /* tensor name (e.g. "output.weight") */
    uint8_t  src_len;           /* length of src_tensor_name */
    char     src[255];          /* source tensor name (e.g. "token_embd.weight") */
    uint8_t  src_type;          /* GGML type of source data in pack */
    uint8_t  dst_type;          /* GGML type callback should report */
    uint8_t  transform;         /* RESIDUAL_TRANSFORM_* enum */
    uint8_t  _pad;              /* alignment padding */
} TESS_ResidualEntry;           /* total: 522 bytes */
#pragma pack(pop)
```

**Why fixed-size?** Simplicity. The index section is variable-length but the residual
section has few entries (<10 in practice). Fixed-size avoids index walking bugs.
The 522-byte per-entry cost is negligible (<1 KB total for typical models).

## 5. Transform Enum

```c
enum {
    RESIDUAL_TRANSFORM_NONE       = 0,   /* pass-through: copy src → dst as-is */
    RESIDUAL_TRANSFORM_TYPE_CAST  = 1,   /* dequantize src_type → dst_type */
    RESIDUAL_TRANSFORM_FP16_TO_F32 = 2,  /* f16 blob → f32 callback buffer */
    RESIDUAL_TRANSFORM_F32_TO_FP16 = 3,  /* f32 data → f16 pack storage */
    RESIDUAL_TRANSFORM_IDENTITY   = 4,   /* fill dst with identity (1.0f per element) */
    RESIDUAL_TRANSFORM_ZERO       = 5,   /* fill dst with zeros */
};
```

| Transform | src_type | dst_type | Operation |
|-----------|----------|----------|-----------|
| `NONE` | any | same | memcpy from source tensor (weight-tying, same dtype) |
| `TYPE_CAST` | Q8_0 | F32 | dequantize Q8_0 → F32, write to callback buffer |
| `FP16_TO_F32` | F16 | F32 | IEEE 754 f16 → f32 conversion |
| `F32_TO_FP16` | F32 | F16 | f32 → f16 (for pack storage) |
| `IDENTITY` | any | F32 | fill with 1.0f (for .scale, .input_scale) |
| `ZERO` | any | any | fill with 0 (for missing optional tensors) |

## 6. Reader API Additions (geo_tess_container.h)

### 6.1 TESS_PackIndex extension

```c
typedef struct {
    /* ... existing fields ... */

    /* residual section (version 2+) */
    const uint8_t *residual_data;   /* pointer to residual entries (in mmap or malloc) */
    uint32_t       residual_count;  /* number of residual entries */
} TESS_PackIndex;
```

### 6.2 New functions

```c
/* Find a residual redirect for a tensor name.
 * Returns pointer to residual entry, or NULL if not found.
 * The returned pointer is valid until tess_pack_close(). */
const TESS_ResidualEntry *tess_pack_find_residual(const TESS_PackIndex *pi,
                                                   const char *tensor_name);

/* Apply a residual transform: read source data from pack, transform, write to dst.
 * Returns bytes written, or 0 on error.
 * dst must hold enough space for dst_type output (computed from n_elems). */
int tess_pack_apply_residual(const TESS_PackIndex *pi,
                             const TESS_ResidualEntry *re,
                             uint32_t n_elems,
                             void *dst);

/* Execute full residual redirect: find entry + load source + transform.
 * Returns bytes written, or -1 if no residual entry found.
 * This is the primary API for the callback. */
int tess_pack_resolve_residual(const TESS_PackIndex *pi,
                               const char *tensor_name,
                               uint32_t n_elems,
                               void *dst);
```

### 6.3 Implementation sketch

```c
static inline const TESS_ResidualEntry *tess_pack_find_residual(
        const TESS_PackIndex *pi, const char *tensor_name)
{
    if (!pi->residual_data || pi->residual_count == 0) return NULL;
    uint32_t tname_len = (uint32_t)strlen(tensor_name);
    const uint8_t *cur = pi->residual_data;

    for (uint32_t i = 0; i < pi->residual_count; i++) {
        uint8_t nl = *cur;
        if (nl == (uint8_t)tname_len && memcmp(cur + 1, tensor_name, nl) == 0) {
            return (const TESS_ResidualEntry *)cur;
        }
        cur += sizeof(TESS_ResidualEntry);
    }
    return NULL;
}

static inline int tess_pack_apply_residual(
        const TESS_PackIndex *pi,
        const TESS_ResidualEntry *re,
        uint32_t n_elems,
        void *dst)
{
    switch (re->transform) {
    case RESIDUAL_TRANSFORM_ZERO:
        memset(dst, 0, (size_t)n_elems * ggml_type_size(re->dst_type));
        return (int)((size_t)n_elems * ggml_type_size(re->dst_type));

    case RESIDUAL_TRANSFORM_IDENTITY: {
        float *f = (float *)dst;
        for (uint32_t k = 0; k < n_elems; k++) f[k] = 1.0f;
        return (int)(n_elems * sizeof(float));
    }

    case RESIDUAL_TRANSFORM_NONE:
    case RESIDUAL_TRANSFORM_TYPE_CAST:
    case RESIDUAL_TRANSFORM_FP16_TO_F32:
    case RESIDUAL_TRANSFORM_F32_TO_FP16: {
        /* load source tensor data from pack */
        uint32_t src_cs = ggml_type_size(re->src_type);
        if (src_cs == 0) return 0;
        uint64_t src_cells = (uint64_t)n_elems;
        uint64_t src_bytes = src_cells * src_cs;

        /* source stored as ONION (f16 blob) or SCATTER (capo) */
        const uint8_t *onion = NULL;
        uint32_t onion_sz = 0;
        if (tess_pack_find_onion(pi, re->src, &onion, &onion_sz) == 0) {
            /* ONION source */
            if (re->transform == RESIDUAL_TRANSFORM_FP16_TO_F32 &&
                re->src_type == TESS_GGML_F16 && re->dst_type == TESS_GGML_F32) {
                const uint16_t *f16 = (const uint16_t *)onion;
                float *f32 = (float *)dst;
                for (uint32_t k = 0; k < n_elems; k++)
                    f32[k] = fp16_to_float(f16[k]);
                return (int)(n_elems * sizeof(float));
            }
            if (onion_sz == src_bytes) {
                memcpy(dst, onion, src_bytes);
                return (int)src_bytes;
            }
        }

        /* SCATTER source: use capo reader */
        uint32_t capos_needed = (uint32_t)((src_cells + TESS_TOTAL_SLOTS - 1) / TESS_TOTAL_SLOTS);
        uint8_t *out = (uint8_t *)dst;
        for (uint32_t c = 0; c < capos_needed; c++) {
            TESS_CapoReader cr;
            if (tess_pack_get_capo_mmap((TESS_PackIndex *)pi, &cr, re->src, c) != 0)
                return 0;
            uint32_t cells = (src_cells >= TESS_TOTAL_SLOTS)
                           ? TESS_TOTAL_SLOTS : (uint32_t)src_cells;
            uint32_t got = (uint32_t)tess_capo_load_range(&cr, 0, cells,
                            out + (uint64_t)c * TESS_TOTAL_SLOTS * src_cs);
            if (got != cells * src_cs) return 0;
            src_cells -= cells;
        }
        return (int)(n_elems * src_cs);
    }
    default:
        return 0;
    }
}
```

## 7. Packer Changes (tess_gguf_pack.c)

### 7.1 New CLI mode

```bash
tess_gguf_pack <input.gguf> <output.tesspack> [tensor_filter] [--residual redirects.json]
```

The `--residual` flag reads a JSON file of redirect rules and writes residual entries
after the main index.

### 7.2 Redirect JSON format

```json
[
    {
        "name": "output.weight",
        "src": "token_embd.weight",
        "src_type": 8,
        "dst_type": 0,
        "transform": 1
    },
    {
        "name": "blk.0.attn_output.scale",
        "src": "",
        "src_type": 0,
        "dst_type": 0,
        "transform": 4
    }
]
```

### 7.3 Packer logic

After writing the main index (scatter + onion + gguf header entries):

```c
/* ── write residual entries (if --residual provided) ── */
uint32_t n_residual = 0;
if (residual_json) {
    n_residual = parse_residual_json(residual_json, residual_entries);
    for (uint32_t i = 0; i < n_residual; i++) {
        fwrite(&residual_entries[i], sizeof(TESS_ResidualEntry), 1, fout);
    }
}

/* update header with residual info */
fseek(fout, 0, SEEK_SET);
hdr[1] = 2;  /* version 2 */
hdr[6] = (uint32_t)residual_offset;
hdr[7] = n_residual;
fwrite(hdr, 1, 64, fout);
```

### 7.4 Auto-generation from GGUF analysis

The packer can **auto-detect** weight-tying patterns:
1. Scan GGUF tensor list for `token_embd.weight`
2. Check if `output.weight` is absent
3. If so, auto-add residual redirect: `output.weight → token_embd.weight, TYPE_CAST`
4. Scan for `*.scale` / `*.input_scale` not in GGUF → auto-add IDENTITY redirects

This covers the common case without requiring a manual JSON file.

## 8. Callback Changes (tesspack_bridge.c)

### 8.1 Updated `provide_tensor`

```c
static void provide_tensor(struct ggml_tensor *t, void *ud) {
    TensorHook *h = (TensorHook *)ud;
    const char *name = ggml_get_name(t);
    size_t need = (size_t)ggml_nbytes(t);
    uint8_t *dst = (uint8_t *)t->data;

    /* ... existing empty/zero check ... */

    /* ── ONION path (unchanged) ── */
    /* ... existing onion logic ... */

    /* ── SCATTER path (unchanged) ── */
    /* ... existing pack load logic ... */

    /* ── RESIDUAL path (NEW — replaces hardcoded weight-tying) ── */
    {
        const TESS_ResidualEntry *re = tess_pack_find_residual(h->pi, name);
        if (re) {
            uint32_t n_elems = (uint32_t)ggml_nelements(t);
            int written = tess_pack_apply_residual(h->pi, re, n_elems, dst);
            if (written > 0) {
                h->n_pack++; h->b_pack += written;
                tmap_add(h, name, dst);
                fprintf(stderr, "  [bridge] RESIDUAL %s → %s (transform=%u)\n",
                        name, re->src, re->transform);
                return;
            }
            fprintf(stderr, "  [bridge] RESIDUAL-FAIL %s (written=%d)\n", name, written);
        }
    }

    /* ── fallback: optional tensors (identity) ── */
    /* ... existing scale/input_scale logic ... */
}
```

### 8.2 Key improvement

The hardcoded `output.weight` → `token_embd.weight` special case at line 216-228
is **replaced** by the residual table lookup. No more `strstr` matching, no more
`t->type` patching. The residual entry carries the exact transform needed.

## 9. Integration Test Plan

| Test | Input | Expected |
|------|-------|----------|
| `test_residual_write` | GGUF + redirects.json | .tesspack with hdr[1]=2, residual_count > 0 |
| `test_residual_read` | .tesspack v2 | `tess_pack_find_residual()` finds entries |
| `test_residual_apply_none` | redirect NONE | memcpy matches source |
| `test_residual_apply_cast` | redirect TYPE_CAST Q8_0→F32 | dequantized values match |
| `test_residual_apply_identity` | redirect IDENTITY | dst filled with 1.0f |
| `test_residual_bridge_roundtrip` | Qwen3 .tesspack | bridge output matches plain GGUF |
| `test_residual_backward_compat` | .tesspack v1 | residual_count=0, existing behavior unchanged |

## 10. File Size Impact

- Per-entry: 522 bytes (name[255] + src[255] + 6 bytes metadata + 2 pad)
- Typical model: 2-5 residual entries → ~1-3 KB overhead
- Negligible vs. multi-GB weight data

## 11. KIS Scale-Change Consideration

The residual region does **not** participate in KIS scale timeline operations.
Redirects are resolved at load time (single-pass), not at scale-change events.
The source data inherits whatever scale/stride the original SCATTER or ONION
region has — no additional addressing needed.

## 12. Future Extensions

- `RESIDUAL_TRANSFORM_DEQUANT_K` for K-quant block → F32 (Q4_K, Q5_K, Q6_K, Q8_K)
- `RESIDUAL_TRANSFORM_TRANSPOSE` for weight matrix transposition
- `RESIDUAL_TRANSFORM_MERGE` for MoE expert routing (combine multiple source tensors)
- Residual entries referencing other residual entries (chain redirects)
