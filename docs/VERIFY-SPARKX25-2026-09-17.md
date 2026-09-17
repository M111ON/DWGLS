# Spark-X2.5-1.7B Verification — REPORT (2026-09-17)

## Question
Does a brand-new architecture (iFlytek Spark-X2.5, `spark2_5`, fused `attn_qkv`,
hybrid 1-full + 3-sliding attention) pass the full DWGLS route, and does its
1M-context claim hold on real hardware?

Source: https://huggingface.co/XHToken/Spark-X2.5-1.7B-GGUF
File: `F:/model/Spark-X2.5-1.7B-Q4_K_M.gguf` (1107457856 B, 226 tensors)

## Checks (all run 2026-09-17)

1. **Bake** (`tools/tess_gguf_pack.c`): 226 tensors → **415 capos / 57 onion**,
   `.tesspack` 1345.4 MB (+21.5%), sig32 ok. Zero reader changes: fused qkv and
   Q6_K (type 14) `token_embd` residual auto-handled (rule #906). 28 blocks.
2. **Assemble + compare**: 10.4 s → `gguf_stream_compare` vs original =
   **226/226 tensors PASS, 0 FAIL, EXIT=0**.
3. **Inference parity** (llama-cli b10830, temp 0, seed 42, thinking model):
   original vs assembled **20/20 tokens identical** ("Bangkok." probe).
4. **Speed** (this box: Pentium G4400 2C/2T no AVX2 + 2× GTX 1050 Ti 4GB):
   CPU-only prompt ~3.0 / gen ~2.5 t/s; Vulkan (`-ngl 99`) prompt 12.3 /
   gen **45.4 t/s** (~18×). The 2.5 t/s scare was the Pentium, not the model.
5. **Needle retrieval** (~55k tokens, 164 KB prompt, `-ngl 55`, q8 KV):
   planted `BLUE-RIVER-77` at position 0, asked at end → **quoted verbatim**.
   Prefill ~250 t/s, gen ~15 t/s.
6. **200k attempt**: ❌ `ggml_vulkan: device lost` — 4 GB VRAM insufficient.

## KV math (from GGUF metadata: `spark2_5`, d_model 2048, 8 Q / 2 KV heads,
sliding window 512, `context_length = 1048576`)

- Per token per layer = 2·2·256·2 B = 2 KB. 7 full layers → **~14 GB for 1M**
  (f16); 21 sliding layers see only last 512 tokens (~22 MB).
- 200k ≈ 1.4 GB q8 KV + 1.1 GB model + buffers > 4 GB budget → device lost.
- "1M on-device" rides on **7/28 layers**. No independent needle test exists yet.

## Verdict
New arch passes the whole route with no code changes (bake → assemble →
inference parity → 55k needle). 1M is datacenter-only; on this box 55k is
proven, 200k is not. The honest split: 7 full layers = hot/mirror zone,
21 sliding layers = cold/carry — Saturn zones map onto this arch unmodified.

## Env notes (for next runs)
- Spawning `llama-cli` with a model kills the opencode pwsh shell; the child
  survives as orphan — poll output files from a fresh call, then `Stop-Process`
  (each orphan eats ~1 GB; box has 8 GB). Upstream: #859, #22142 (#972).
- `-ngl 55` used per owner (28 layers → full offload either way).
- Stale `build/*.exe` (15/09) masked real status twice today — always force
  rebuild before trusting PASS/FAIL.
