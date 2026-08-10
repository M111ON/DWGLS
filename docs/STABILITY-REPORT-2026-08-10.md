# STABILITY REPORT — DWGLS Breathing FS (2026-08-10)

**Scope**: anchor seeker · BIMG v3 persist · dynamic codec · breath engine ·
mmap zero-copy · CLI  — commits c5365b6..80f0743 (4 phase-1 commits) + audit fixes below
**Method**: source audit + runtime canary proof + static analyzer +
95k-check invariant stress monitor + full regression + real CLI cycle

---

## 1. Findings (3) — fixed in this audit

### F1 (REAL BUG, 132-byte buffer overrun) — plain-read path
`bfs_read` decoded a full 144-slot block into the caller buffer even when a
file's last block was partial (`total_bytes % 144 != 0`). Caller contract is
`out_size ≥ total_bytes`; writes extended up to 143 B PAST it → UB.
Root cause: overrun writes were zero-padding content, so data still read
back correctly and tests (block-multiple sizes) never noticed — silent.
**Proof**: canary harness (exact 300-byte buffer, 0xA5 sentinels) showed
`CANARY-OVERRUN=132` on the pre-fix binary, `0` after. Regression locked
into `tests/test_bfs_stability.c` T1 (plain + mmap).
Fix: `dyn_decode(..., bsz)` at `core/breathing_fs.h` + `core/bfs_persist.h`.

### F2 (LATENT UB) — seeker window overflow at extreme scale
`window = (uint32_t)(K/scale)` overflows (float-cast UB, wraps silently)
for `scale < ~1.2e-6`. All real paths use scale ≥ 0.05, but the API didn't
guard the extreme. Fix: floor `scale` at 1e-6 in `seeker_scale`
(preserves hyperbolic semantics for any scale < 1.0); `space_size` was
already clamped ≥ 1 (no div-by-zero). Locked as stability T4.

### F3 (HARNESS SEMANTICS — NOT a system bug, documented)
The monitor's first run flagged 2,746 mismatches. Investigation showed the
checks contradicted the design: anchor delta (`home×(scale−1)`) is
UNBOUNDED by design (±20,735); the |Δ|≤127 bound belongs to the BREATH
side channel (re-anchored) only — a category error in my invariants, not
the code. Corrected monitor; also confirmed write-time `current_pos ==
home_pos` (anchor formula applies post-move, same as parse).
**Takeaway**: two delta semantics must stay distinct in docs (anchor
formula vs breath bound).

## 2. Verified invariants — live numbers

| Invariant | Method | Result |
|---|---|---|
| Anchor: `current == home×scale % space`, every used block | INV-1, all scales 0.05..2.0 | ✓ every round |
| Parse derives current/delta identically to live move | INV-parse-consistency | ✓ every round |
| Breath |Δ| ≤ 127 through 200 ticks/round | `bfs_breath_all_bounded` | ✓ every round |
| Lossless: plain read == written, EVERY value | decode+compare × 8 files/round | ✓ 100% |
| Lossless: mmap read == written, EVERY value | decode+compare × 8 files/round | ✓ 100% |
| RDH bijection `encode(decode(x))==x` per block | `bfs_rdh_verify_all` | ✓ all used blocks/round |
| CRC detects ANY corruption in [0,data_end) | flip byte in TOC → parse rc=-4 | ✓ |
| Partial-last-block no overrun (plain + mmap) | canary (0x A5 sentinels) | ✓ 0 bytes |
| Edge slot 20735 write/read + scale<1 wrap | stress edge.bin | ✓ |
| Σ file n_blocks == n_blocks_used (image integrity) | parse invariant | ✓ every round |

**Total: 95,256 invariant checks (1,000 rounds × 8 files + edge),
0 failures — 1.98 s.**

## 3. Static analysis

- GCC `-fanalyzer` (16.1.0) on the full BFS stack: 0 real findings
  (only `-Wanalyzer-too-complex` path truncation on deep inlined codecs).
- Constraint honesty: ASAN/UBSAN runtimes are NOT installed in this MSYS2
  (gcc 16.1 + clang 22.1 both lack libasan/libubsan; compiler-rt package
  ships headers only) — anomaly coverage comes from canary runtime proof
  (F1), analyzer, and bounds audits instead. **Recommend** running the
  suite under ASAN once on a Linux box (WSL) for the same 5 test binaries
  (harness: `build/san_sweep.sh`).

## 4. Regression (fresh, zero warnings)

```
make clean && make test  → TIER1 27/27 · TIER2 3/3  (30/30)
test_bfs_stability       17/17   (overrun canary, edge wrap, CRC, scale guard)
test_bfs_persist         66/66   (v3 derived-TOC layout)
bench 56 files           24,492 B (1.215x) — all paths lossless
CLI cycle (v3)           create 4,332 → write 4,476 → INTEGRITY-OK → mmap
                         RDH 1 block → get → cmp == source ✓
```

## 5. Residual risks (honest)

1. ASAN not run here (see §3) — WSL run recommended before Phase 2.
2. `bfs_verify_file` uses malloc (diagnostic/CLI helper only, not hot
   path). If strict no-malloc is wanted, convert to a static 20,736-byte
   buffer — but it's not on the read/write path.
3. `block_owner` runtime table: derived at parse, no longer on disk —
   any future delete/defrag feature must maintain the contiguous-run
   invariant (e_sizes/owners derivation depends on it).
4. Thread-safety: static scratch buffers (`bfs_save_img/load_img/mmap_sync`)
   are single-threaded by design — document if multi-worker pull lands.

---
**Verdict**: system is stable for Phase 1 scope (persist/codec/seek/breath/
mmap/CLI). One real bug (F1) found and fixed with runtime proof; two
harness-side confusions clarified. Green: 30/30 regression, 95k stress,
zero warnings.