# AGENTS.md Patch — Latest State Update (2026-09-15)

> This patch updates the "Pending" and "Open Next" sections of AGENTS.md.
> Apply manually or via approved write.

## Updated Pending Section

### Pending
- **GPU scatter decode standalone kernel**: DONE — `bench/tess_scatter_decode.cu` compiled on Colab T4 (sm_75), verified lossless on **real model**: Qwen3-0.6B Q8_0 (610 MB GGUF, 991 capos, 18.6M elements, 633 MB decode output). No llama.cpp dependency. mmap → cudaHostRegister → stride-37 scatter decode on GPU. Decode: 1771 ms, sig32 integrity: 0/991 mismatch, CPU/GPU bitwise identical. Deploy script: `colab-pack/deploy_scatter_decode.sh`.
- **sig32 XOR-fold integrity**: Added to `tess_gguf_pack.c` (hdr[8]). Bake on Gemma 4 (601 tensors, 3792 capos, 2921 MB) + Qwen3-0.6B (310 tensors, 991 capos, 672 MB) — sig32 verified on both.
- **Gemma 4 bake proven**: GGUF v3 (601 tensors, 601 Q4_0 capos + 289 onion). gguf_reader.h handles v3 (u64 fields) natively. No reader changes needed. Ratio 100.7%.
- **Multi-format lossless**: 3/3 models on GPU — Qwen3-TTS Q4_K_M (426 capos, 0/426 mismatch), Qwen3-0.6B Q8_0 (991 capos, 0/991 mismatch), Bonsai-4B Q1_0 (145 capos, 0/145 mismatch). Mixed cell_sz bug fixed (per-capo byte offsets).
- **Planet v2** (`core/geo_planet.h`): u64 keys, 32B→40B tomb (origin+final), fail-closed thaw, self-gating entangle link (auto-open/close, mask 8, 3 clean), contraction journal (4 reanchor epochs + audit). 12-pentagon face-spawn registry (PlanetSys). Tested on real GGUF + real FGXLog replay.
- **BFS wiring**: planet born at write, verified per tick (idle-zero), `bfs_delete` retire-then-free + tomb archive, `bfs_migrate` wrap-relocate surgery, image **v4** (tomb tail + rebirth, v3 tolerant). Fold operator `bfs_fold_compact()` on demand.
- **Dual loop** (`core/dual_loop.h`): 20→12 merge (5→1 XOR), 12→20 split, idempotent settle (merge∘split=id). 32-unit field view (20+12=32, 648=8×81) exhaustive 20736/20736.
- **Goldberg neighbors**: GP(4,0) 162×6 neighbor table (subdivide+dual). G5-G9 levels mechanical partitions ready.
- **RDH twin** (`collection/rdh/`): high-nibble second fuse, preset 64×81=K, nibble-tamper localizes to one key.
- **Fold-net engine**: committed graph walk, tetra+cube Euler. Wonder-cube Fig3 net + SameSum sets (paper oracle). Dodeca closed net W9/W10.
- **llama.cpp user-buffer patch** (build_zc2, `docs/LLAMA-USER-BUFFER-PATCH.md`): 4 hunks — (1) `no_alloc=false` in `llama_model_init_from_user()`, (2) null callback guard in `load_all_data()`, (3) per-tensor CPU_Mapped binding, (4) re-gate on `set_tensor_data` alone. Huihui MoE 1B Q4_K_M 14/14 PASS, L3 bitwise identical.
- **Bounded cache** (`core/win_cache.h`): `DWGLS_WIN_CACHE=<cap>` + `DWGLS_EVICT=1`. Eviction via UnmapViewOfFile+MapViewOfFile proven (506 MB body released, re-fault 16544 pages, tokens bitwise identical).
- **ARM Termux deploy** (`scripts/build_termux.sh`, `deploy_termux.sh`): SM-G988B, 24/27 compile, 23/24 pass, ARM 3-6× geometry win. Report `docs/ARM-DEPLOY-REPORT-2026-09-13.md`.
- **Inner field digit-extension** (approach A, address-only): spec + test 9/9, GEO_FAST 24/24.
- **Scale-teleport probe**: 1 divergence, deferral verdict (metric fiction confirmed).
- **Walkthrough 7-layer recheck** (2026-09-13): FIELD/KIS(9/9)/hyperbolic(ALL)/breathing(22/22)/fan24(19/19)/tesspack(3/3+ledger44319)/serve-skeleton all live. 4 bijections registry. Doc `docs/WALKTHROUGH-2026-09-13.md` (304 lines).

### Open Next (from HANDOFF-2026-09-13-planet-dualloop.md)
1. RDH-twin placement (keys live, downstream mapping missing)
2. Goldberg levels 32/72/192/432/2592 (mechanical partitions)
3. birth/death W from real scale (currently 0, YAGNI)
4. Lucas rulebook, residue check
5. Dual-loop consumer: fresh seeds per round (no perpetual motion by design)
6. **WIRING phase**: proven islands (D4/A2/fractal/quadtree/L-block/geo_jump) have zero product includes — priority voronoi test fix, then D4 views into serve/GPU, L-block+jump adapters, quadtree iff slow. Research pause, integration phase.
