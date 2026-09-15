# llama.cpp user-buffer patch (build_zc2) — 2026-09-15

Goal: serve GGUF weights from user-owned memory (field mmap) via
`llama_model_init_from_user()`, with normal context/compute allocation.
Proven: Huihui MoE 1B Q4_K_M, `gguf_lazy_serve` FINAL 12/12 PASS, EXIT=0,
lazy 5 tokens vs reference 5 tokens identical (L3 bitwise PASS).

## Hunk 1 — `I:/llama/llama.cpp/src/llama.cpp:444`

In `llama_model_init_from_user()`: `params.no_alloc = false;`
(was `true`). `no_alloc=true` propagates to `model->hparams.no_alloc`,
which makes `llama_context` skip compute-buffer reservation
(`llama-context.cpp:630,649,664`) → crash in context init / first decode.
`false` keeps the normal compute/KV allocation contract while tensor
payloads stay user-owned via the callback.

## Hunk 2 — `I:/llama/llama.cpp/src/llama-model-loader.cpp:1472-1479`

In `load_all_data()`, `files.empty()` branch: return `true` immediately
when `set_tensor_data == nullptr` (callback already consumed and cleared
during buffer binding). Without this, a null function-pointer call →
0xC0000005.

## Hunk 3 — `I:/llama/llama.cpp/src/llama-model.cpp:1593-1620` (reference)

Per-tensor `CPU_Mapped` binding branch, gated on
`ml.set_tensor_data && ml.no_alloc`. NOTE: under Hunk 1 (`no_alloc=false`)
this branch is currently DEAD — model weights take the `else` path
(`:1626-1627`, `ggml_backend_alloc_ctx_tensors_from_buft`, one full
model-sized buffer), then `load_all_data()` re-invokes the still-set
callback which overwrites `t->data` with user pointers. Works, but the
allocated buffer is wasted (see overhead below).

## DWGLS side

- `tools/plain_user_mmap.c` (new, untracked): minimal control — plain GGUF
  read-only mmap through the same callback API. Gate result: model=ok,
  291/291 served, first_decode_rc=0, EXIT=0.
- `tools/gguf_lazy_serve.c` (modified): synthetic fallback storage,
  64B index-header alignment, tied-output L2b tolerance, backend path argv.
- Runner links against `build_zc2/bin/Release` DLLs; DLL rebuilt from
  `build_zc2/src/llama.vcxproj` (Release x64).

## Hunk 4 (dedup, 2026-09-15) — `llama-model.cpp:1593` + `:1625`

Re-gated user-callback binding on `ml.set_tensor_data` alone
(dropped `&& ml.no_alloc`), and narrowed the dummy-buffer branch to
`ml.no_alloc && !ml.set_tensor_data`. Verified green:

- plain control: model=ok, 291/291 served, first_decode_rc=0, EXIT=0
- Huihui lazy serve: FINAL 12/12 PASS, L3 bitwise identical YES, EXIT=0

## Honest WS accounting (Huihui 717MB MoE, post-dedup run)

| phase | WS | private |
|---|---|---|
| reference generation (40 tok) | 1597.9 MB | 656.5 MB |
| lazy generation (40 tok) | 2221.3 MB | 669.2 MB |
| delta | +623.5 MB | +12.3 MB |

Duplicate model buffer eliminated: private delta +814.2 → +12.3 MB.
Remaining WS delta is shared/file-backed mappings + KV/compute (expected:
reference 40-tok run holds KV, lazy path adds mmap'd field views).
Field residency honest: 5.7 MB after load, 617.8/683.8 MB after generation,
36 unused MoE expert tensors (~69 MB) never faulted in.

## Eviction proof (2026-09-15) — `tools/gguf_lazy_serve.c` `DWGLS_EVICT=1`

| phase | field residency | WS | private |
|---|---|---|---|
| before evict | 506.5 MB (129669 pg) | 1727.8 MB | 79.3 MB |
| after unmap+remap | 0 MB (0 pg) | 1221.3 MB | 79.3 MB |
| re-load (re-fault) | 506.5 MB | — | — |
| re-generate | 506.5 MB | — | — |

- Mechanism: `UnmapViewOfFile` + `MapViewOfFile` (only way to drop mapped-file pages on Windows; `DiscardVirtualMemory`/`OfferVirtualMemory` are private-memory only, rc=87/INVALID_PARAMETER).
- Re-load after evict: 16,544 page faults, callback re-entered field — **true re-fault**.
- Tokens after evict+reload: **bitwise identical** (E1 PASS).
- WS drop = tensor body (506 MB) released; index+tokenizer (pinned) remain.
- This proves **bounded working set is achievable**: evict body between inferences, re-load on demand, no accuracy loss.

## Bounded cache enforcement (2026-09-15) — `core/win_cache.h` + `gguf_lazy_serve.c`

| flag | effect |
|---|---|
| `DWGLS_WIN_CACHE=<cap>` | allocates shadow cache (cap windows ≈ cap × 20 KB) |
| `DWGLS_EVICT=1` | enables enforcement at phase boundaries |

- `wc_touch()` in `provide_tensor` admits windows during load callback; when full, reports coldest victim (LRU by tick) but does NOT remove — honest accounting.
- `wc_enforce_cap()` called before generate and re-generate: collects all victims from full cache, triggers full-body unmap+remap (Windows limitation: no sub-range evict for mapped files).
- Result: cache cap respected, body evicted between phases, re-load re-faults cleanly, tokens bitwise identical.

**Huihui MoE 1B Q4_K_M (717 MB) END-TO-END: 14/14 PASS**
- 36 expert tensors partially read (~69 MB never faulted) — MoE sparsity verified.
- L3 bitwise identical YES, E0/E1 PASS.
- Bounded cache: cap=1000 → enforcement triggered, 1000 cold windows evicted per phase boundary.

## Status (2026-09-15, done)

Hunk 4 applied, DLL rebuilt, all gates green. Eviction mechanism + bounded cache enforcement proven on Qwen2.5-0.5B and Huihui MoE 1B. Private overhead eliminated (+12.3 MB). Prefetch hint wired (async, 318 faults/0.06s) — architecturally ineffective for generate under Hunk 4 (weights in llama buffers), but scaffold ready for true lazy path. Next: true lazy generate path (field-backed compute) or multi-model field sharing.

## L2 outage + fix (2026-09-15) — DLL shadowing, missing CPU backend, Vulkan_Host buft

Lazy load (`llama_model_init_from_user` → NULL) went red on this box. Three stacked causes, fixed in order:

1. **DLL shadowing (environment)**: `build/*.exe` resolved `llama.dll`/`ggml-base.dll` by name via PATH to `I:\FGLS_new\runner\` (a third, unpatched split-DL build) instead of our patched `build_zc2`. Fix: staged `build_zc2` DLLs (`llama/ggml/ggml-base/ggml-cpu-*`) + `libomp.dll` beside the exe — exe-dir wins search order deterministically. `build/` is gitignored; treat staged DLLs as a documented environment prerequisite.
2. **No CPU backend in registry (mechanical)**: `ggml_backend_load_all()` registers nothing built-in (GGML_USE_CPU off in this build); path-scan found only Vulkan (CPU DLLs failed: `libomp` unresolvable). After staging, scan registers CPU from exe-dir (`ggml-cpu-sse42.dll`). Proven via `[BE]` probe: `devices=2 Vulkan-only, cpu_dev=NULL` → after: `devices=5, CPU type=0`.
3. **Vulkan_Host preferred buft can't wrap field pointers (contract)**: `select_weight_buft` picks first supported entry → `Vulkan_Host` (host-pinned staging for GPU offload); its device caps lack `buffer_from_host_ptr` → our Hunk throws `user tensor callback did not provide a host buffer for Vulkan_Host`. Fix (exe-side, no llama rebuild): `mp.no_host = true` on the **lazy path only** — CPU tensors land on the plain CPU buft → `cpu_buffer_from_ptr` binds field pointers, zero-copy preserved. Reference path keeps defaults.

Re-verified after fix: Qwen2.5-0.5B **12/12 PASS** (L3 40/40 bitwise), Huihui MoE 1B **14/14 PASS** (667.3 MB → 0 MB evict, 15416 re-faults, 40/40 identical re-generate).
