# Tesseract Route Verification — REPORT (2026-09-17)

## Question
Does data flow correctly through the tesseract route
(stride-37 scatter → `.tess` capo → `.tesspack` → assemble → GGUF)?

## Checks (all run 2026-09-17)
1. **Unit level**: `make test-tess` → **27/27 PASS, 0 FAIL, 0 SKIP**
   (index frame, scale log, frame seek, magnify, wiring, header, stream,
   moe_bridge, tetra/torus walks, full cycle, …).
2. **Real-data E2E**: `make tesspack-e2e` (Qwen3-0.6B-Q8_0, 604 MB)
   GGUF → `.tesspack` → assembled GGUF → `gguf_stream_compare` byte-verify →
   **310/310 tensors LOSSLESS, 0 FAIL**.

## Verdict
Route verified end-to-end on real weights. Data in → scatter → pack →
assemble → byte-identical out.

## Incidental fix (build break found during verification)
`tesspack-e2e` step 2 failed to compile: `tools/tesspack_assemble.c` defines
local `ggml_type_size`/`ggml_blck_size` stubs AFTER including
`core/geo_tess_container.h`, whose `tess_pack_apply_residual` calls them →
implicit-declaration error (GCC 14). Stale `build/tesspack_assemble.exe`
(2026-09-13) masked it; the residual commit (4dbf81c) broke it silently.
Fix: forward declarations before the header include (minimal diff, same tables).
Normal path unaffected: `make dual-lazy-serve` still 21/21 PASS.
