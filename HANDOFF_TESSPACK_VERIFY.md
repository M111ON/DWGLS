# Handoff: Tesspack Standalone Verifier + Lossless Proof

## Goal (Next Session)

1. **Fix LFM2.5-8B Q4_K_M verifier failure** — index corruption root cause identified (see Open Decisions)
2. **Pack + verify remaining Q4_K_M models** — Qwen3.5-2B Q4_K_M, Qwen3.8-2B Q4_K_M, Kokoro Q8
3. **Fix Bonsai Q1_0 packer crash** — ACCESS_VIOLATION at capo ~28/38 of last tensor, file > 700 MB

## State of Play

### Proven LOSSLESS via Pure C Verifier (tesspack_verify_pure_c.c)
| Model | Quant | Tensors | Capos | Verified MB | Status |
|-------|-------|---------|-------|-------------|--------|
| Qwen2.5-0.5B | Q8_0 | 291 | 1060+121onion | 638 | LOSSLESS |
| Qwen3-0.6B | Q8_0 | 310 | 991+113onion | 604 | LOSSLESS |
| Qwen3VL-2B | Q4_K_M | 310 | 423+113onion | 1050 | LOSSLESS |
| Qwen3.5-2B | Q8_0 | 320 | 2909+133onion | 1908 | LOSSLESS |
| Qwen3.8-2B | Q8_0 | 335 | 3003+140onion | 1970 | LOSSLESS |

### FAIL — LFM2.5-8B-A1B Q4_K_M
- Pack created: `F:\model\lfm25-8b-a1b-q4km.tesspack` (1084 MB)
- Verifier reads garbage index entries (names are random binary)
- Pack size > 700 MB triggers same header-write bug as Bonsai
- **Root cause identified:** header write-back code at line 278 `fwrite(hdr, 1, 64, fout)` after `fclose`+`fopen("r+b")` — this DOES work (hdr[2]=1806 matches), BUT `onion_data_start=0` in header (hdr[4]=0) despite packer writing 123 onion entries
- Index at correct offset (hdr[3]=1136691888), but content is garbage
- **Need to investigate:** why index data at correct offset contains garbage despite packer writing field-by-field

### FAIL — Bonsai-4B Q1_0
- Packer crashes ACCESS_VIOLATION at tensor [397] blk.35.ffn_up.weight capo ~28/38
- Always same tensor, same crash point — deterministic
- 8 GB RAM machine, file 727 MB
- Old pack (17.2 MB) only had norm weights (Q1_0 packed before ONION V1 support)
- Skip for now per user directive

### FAIL — Bridge/Serve on 672+ scratch tensor models
- SmolLM2-360M (672 scratch), smolVLM-256M (660 scratch), Qwen3-VL-2B (477 scratch)
- `llama_model_init_from_user` creates architecture tensors not in GGUF → callback zero-fills → breaks computation
- Qwen models work because fewer scratch tensors (<500)
- Bridge/servre path NOT needed for verifier — verifier compares raw data bytes

## Open Decisions

### 1. LFM2.5-8B index corruption — PRIORITY
The packer writes the index correctly (field by field, lines 208-220) but the verifier reads garbage. Both programs agree the format is variable-length (name_len + name + 4 + 8 + 4).

**Hypothesis:** The `fwrite` calls for individual fields (line 209-213) may fail on large files when the file position is > 2 GB (near end of 1084 MB pack). Windows `fwrite` can silently fail if the internal buffer or file pointer overflows.

**Quick test:** Add `fflush(fout)` after every fwrite in the index loop and check return values. Or: write the index to a temp file first, then append it.

### 2. `onion_data_start = 0` in header
hdr[4] = 0 despite packer writing 123 ONION entries. `onion_data_start` is set at line 136 `_ftelli64(fout)` only ONCE (when `n_onion == 0`). If the ONION data was written correctly, `onion_data_start` should be non-zero. Need to verify the ONION data actually appears at the reported offset.

### 3. Bonsai packer crash (lower priority)
STATUS_ACCESS_VIOLATION at tensor [397] capo ~28. Total capos written ~6120 before crash. No debug prints survive to pinpoint exact crash line. Suspect: fwrite to large file position, or GGUF mmap read past boundary.

## Code State

### Files Modified This Session
- `tools/tess_gguf_pack.c` — packer, clean (debug prints removed), builds clean with `gcc -O2`
- `tools/tesspack_verify_pure_c.c` — verifier, fixed OOM (per-capo compare instead of 540 MB alloc), builds with `g++ -O2`

### Build Commands
```bash
# Packer
gcc.exe -O2 -o build/tess_gguf_pack.exe tools/tess_gguf_pack.c \
  -II:/llama/llama.cpp/ggml/include -II:/DWGLS-native-fs/core -lm

# Verifier (needs g++ for ggml-base.a C++ symbols)
g++.exe -O2 -o build/tesspack_verify.exe tools/tesspack_verify_pure_c.c \
  -II:/llama/llama.cpp/ggml/include -II:/DWGLS-native-fs/core \
  -LI:/llama/llama.cpp/build_cpu_fresh/ggml/src -l:ggml-base.a -lgomp -lm -lstdc++
```

### Available Models
- `I:\model\Qwen2.5-0.5B-Instruct-Q8_0.gguf` — verified
- `I:\model\Qwen3-0.6B-Q8_0.gguf` — verified
- `F:\model\Qwen3VL-2B-Instruct-Q4_K_M.gguf` — verified
- `F:\model\Qwen3.5-2B-Q8_0.gguf` — verified
- `F:\model\Qwen3.8-2B-Q8_0.gguf` — verified
- `F:\model\LFM2.5-8B-A1B-Q4_K_M.gguf` — FAIL index corruption
- `F:\model\Kokoro-TTS-82M-Q8.onnx.gguf` — ONNX, skip
- `F:\model\bonsai-4b-q1_0.gguf` — skip per user

### Existing Packs (verified)
- `I:\model\qwen2.5-0.5b-instruct-q8_0.tesspack` (111 MB)
- `I:\model\qwen3-0.6b-q8_0.tesspack` (672 MB)
- `F:\model\qwen3vl-2b-q4km.tesspack` (1361 MB)
- `F:\model\qwen35-2b-q80.tesspack` (1969 MB)
- `F:\model\qwen38-2b-q80.tesspack` (2032 MB)

## Skills to Use
- `dwgls-development` — tesspack architecture
- `geometric-pipeline-qa` — roundtrip verification
- `lossless-codec-verification` — bitwise comparison methodology
