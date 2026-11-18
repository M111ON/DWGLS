# Plan: Selective Scatter (MoE) — bottom-up, OOM-safe

## Problem
`tesspack_server` scatter-decode loop allocates every tensor buffer with
`VirtualAlloc(MEM_COMMIT|MEM_RESERVE)` (top-down). On LFM2.5-8B (5.4 GB
tesspack) this commits all pages before the MoE decommit step runs → OOM on
<8 GB machines (memory #8377). Real RSS saving (5.00→0.63 GB, GATE 4b)
is proven only via OS-level mmap+madvise and Python simulation, not the binary.

## Goal
Reserve address space up front (bottom-up), commit pages only when an expert
slice is actually touched, and release unfired slices after warm-up — all
through the real binary, verified on LFM2.5 K=4/E=32.

## Constraints (from proven work)
- #6353: fused MoE kernels read UNFIRED slices → K-only resident is
  unenforceable on stock kernel; streaming saves commit charge + cold IO,
  never resident RSS. Custom MoE op needed for true resident saving.
- #7002: callback model needs `n_gpu_layers=0` + `no_host=true`; provide
  via zero-copy pointer, never memcpy.
- #7587: ANN and DWGLS field are separate layers — do not cross-include.
- #7287: stage only own hunks in Makefile (carries other workstream targets).

## Options

### A. mmap anonymous + madvise(MADV_DONTNEED)  (preferred on POSIX)
- Replace `VirtualAlloc` with `mmap(MAP_PRIVATE|MAP_ANONYMOUS)`.
- Pages commit on first touch (selective scatter by nature).
- Decommit unfired expert slices with `madvise(MADV_DONTNEED)`.
- Pros: simplest, OS handles demand paging.
- Cons: Windows anonymous mmap is emulated; `madvise` may be a no-op →
  need fallback.

### B. Vectored Exception Handler lazy commit  (Windows fallback)
- `VirtualAlloc(MEM_RESERVE)` only (no commit) for the whole buffer.
- Install a VEH that, on `EXCEPTION_ACCESS_VIOLATION` inside a reserved
  region, `VirtualAlloc(MEM_COMMIT)` the touched page and resume.
- Decommit uses existing `VirtualFree(MEM_DECOMMIT)` (works on VirtualAlloc).
- Pros: keeps decommit working, true lazy commit.
- Cons: VEH complexity, must be process-wide and thread-safe.

### C. Zero-copy into pack + per-expert sidecar  (long-term)
- Point `t->data` directly at the mmap'd pack for packed tensors (no buffer).
- For MoE experts, keep a separate committed buffer only for top-K fired
  slices, allocated lazily.
- Pros: eliminates allocation entirely for packed tensors.
- Cons: temp GGUF assembly (`fwrite`) must read from pack pointers; MoE
  decommit becomes a no-op (pack is shared, cannot decommit per-tensor).

## Recommended
Option B on Windows (current target platform), Option A where `madvise`
works. Keep Option C as a follow-up for dense tensors only.

## Steps
1. Add `breathe_reserve` helper: `VirtualAlloc(MEM_RESERVE)` wrapper
   returning a reserved region + a commit function.
2. Replace the two `VirtualAlloc(MEM_COMMIT|MEM_RESERVE)` sites in
   `tesspack_server.c` with `breathe_reserve`.
3. Add VEH (in a new `core/breathe_commit.h`) that commits pages on first
   touch; register at server startup, unregister at exit.
4. Keep the existing `VirtualFree(MEM_DECOMMIT)` decommit loop (works on
   committed pages of the reserved region).
5. Add env `TESS_SELECTIVE_SCATTER_LOG` printing committed-page count per
   tensor and total RSS before/after decommit.
6. Verify on huihui-moe-1b (vacuous K=E=3) first, then LFM2.5 (K=4/E=32)
   on a ≥8 GB machine.

## Verification gates (must all pass before claiming done)
- `make tess-server` builds.
- huihui-moe-1b serves `/v1/completions` with coherent output (already
  proven, must still pass after change).
- RSS during scatter < 500 MB on LFM2.5 (currently OOMs).
- Post-decommit RSS matches GATE 4b ratio (5.00→0.63 GB) within 10%.
- `maxdiff=0` on logits vs baseline (GATE 4b).
- No access violations under load (VEH correctness).

## Risks
- VEH must not mask real bugs; scope the handler to reserved regions only.
- Multi-threaded inference: VEH registration is process-global; commit must
  be atomic (InterlockedCompareExchange on a page bitmap).
- If a reserved page is never touched, it stays uncommitted (good) but
  reading it returns zeros via OS (acceptable for expert slices).

## Out of scope
- True resident RSS saving (needs custom MoE op, #6353).
- tesspack_server top-down → breathing bottom-up allocation (separate task).
- ANN-side structures (bcf/dhnsw) — separate field (#7587).
