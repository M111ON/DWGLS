# Session report — BFS eviction (#46) + HJ-Jet doctrine (#47) + MoE streaming (#3 v1) — 2026-09-26

## #46 BFS eviction + residency bounding — DONE
- `core/breathing_fs.h`: LRU file evict + spill/fill hooks + `max_blocks` cap (v6res pattern reuse).
  `bfs_write` evict-and-retry (no hooks → v1 `-2/-3/-4` preserved); `bfs_read` faults evicted files back via fill.
- New API: `bfs_evict_oldest`, `bfs_set_spill`, `bfs_set_capacity`, `bfs_residency`,
  `bfs_set_planets` (runtime watcher on/off), freeride counters + `bfs_jet_ratio` (per-mille) + `bfs_jet_alarm` (>5%).
- `tests/test_bfs_evict.c` 49 asserts, mutation-checked (victim-flip, counter-drop, planet-guard).
  BFS group 15/15 + vol6/scale/gjc neighbors green throughout.
- Real proof `tools/bfs_evict_real.c`: full Qwen2.5-0.5B-Q8_0 — 291/291 tensors, 32,505 slices,
  669,763,072 B, **0 mismatches**, peak 144 ≤ cap 144.
- Real testing caught 2 bugs: evict-retry loop bound reread shrinking `n_files` (fixed + T7),
  bit-by-bit CRC bottleneck → table-driven (values verified identical vs `CBF43926` vector).
- Batch-law measured FLAT 0.87 MB/s at all slice sizes — no knee; per-tensor spill batching YAGNI-cancelled.

## #47 HJ-Jet doctrine + meters — DONE
- `docs/HJ-JET-DOCTRINE-2026-09-26.md` (6 sections) + memory #6350.
- Rules: 3 layers never mix (address→residency→integrity, planet is layer 3);
  GJ similarity-entry → HJ deterministic refine → one-way return; discrete decides without fetch,
  continuous faults before trust; jet is ambulance not highway (park only if payload valid);
  freeride metric (>50% bytes via jet per query, >5% system = fix addressing, never widen spur).

## #3 expert streaming — v1 COMPLETE, card stays open
- `tools/moe_stream_proof.c`: baseline (stock) → sanity (callback, zero commit) → minimize/measure/static → gate on tokens.
- huihui-moe-1b: subset-minimize **52/52/48 slots (57–62%), identical, reproduced 3× across shells/binaries**.
  Lesson: prefix-only search inflates K to 97%; full-E pre-apply before probes is mandatory (else false diverge).
- LFM 8B: fire-measure (EmptyWS/mincore + residency snap) → **375 slots / 53.1% union, moat 100/0**,
  commit-fired verify **identical**; `expert_used_count=4` (truth ≈ 12.5%/token, needs per-token swap).
- Replay-from-seed **PASS both models** (LFM 6 tok, huihui p1 16 tok, fresh process, no router).
- Chain v1: `tools/zone_lookup.c` + `build/zones.map` (5 entries) + 4 seedfiles (`build/*.seed`).
- GPU verdicts: cpuonly+ngl silently runs CPU; Vulkan ngl=35/20 → device lost (TDR); ngl=12 works stock
  (1.24+1.31 GB VRAM) but **callback+Vulkan segfaults** (CPU-correct only). Serve lifecycle locked:
  init-from-user (no warm-up) → zone-prior commit (K) → serve+gates. Zone key becomes
  `(personality_id, zone)` when onion-shell lands (no infra change).
- msys2 segfault root causes: wrong cwd + 2 MB default stack → relinked 16 MB (`-Wl,--stack`).
- Side findings: model reads wider than sufficient set (reserve-gate crashes, zeros tolerated — deferred);
  9 GB commit = charge not RAM (pagefile covers zeros); per-prompt buffer free keeps charge transient.
- OPEN: TRACE cross-check, per-token swap (serve-path, the true 12.5%), Colab T4 (ngl-35),
  per-personality zone split. Colab assets verified: `I:/llama/archive.zip` (Linux CUDA .so,
  has `init_from_user`), HF models (Edge-Quant huihui…, LiquidAI LFM2.5…-GGUF), `colab` CLI in WSL;
  `moe_stream_proof.c` now has mincore branch (measure works on Linux).

## Doctrine notes (owner, kept verbatim in spirit)
- 4D ant-nest: untaken paths stay mapped with valves closed; outside sees one root; route = unique seed.
- First route is a prior, gate guarantees correctness; zone pre-map REPLACES warm-up content (commit K, not E).
- Same mask serves as zone-prior, replay-seed, personality-profile — one object, many contexts.
