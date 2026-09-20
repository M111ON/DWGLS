# project.md — DWGLS project detail (L2, load on demand)

## Core Architecture

### Parameterized Geometry Layer (`core/geo_param_grid.h`)
**One family: Dodeca Root** → all shapes derive from the same parent.

| GeoType | verts | edges | faces | cells | Notes |
|---------|-------|-------|-------|-------|-------|
| GEO_DODEC_BASE | 20 | 30 | 12 | 1 | dodecahedron (root) |
| GEO_ICO_BASE | 20 | 30 | 20 | 1 | icosahedron (dual) |
| GEO_COMPOUND_24 | 24 | 48 | 24 | 6 | inverted dodeca compound |
| GEO_DODEC_EDGES | 30 | 60 | 32 | 1 | edge-based |
| GEO_COMPOUND_60 | 60 | 90 | 32 | 1 | pentakis dodeca |
| GEO_PENTAKIS_72 | 72 | 90 | 32 | 1 | 12 base + 60 pyramids |
| GEO_GOLDBERG_92 | 92 | 270 | 92 | 1 | goldberg dual |
| GEO_COMP_SPIKE_120 | 120 | 180 | 62 | 1 | spike compound |
| GEO_GOLDBERG_132 | 132 | 270 | 92 | 1 | goldberg level 2 |
| **GEO_COMPOUND_144** | **144** | **576** | **576** | **144** | **★ 6ico = 18tes (protagonist)** |
| GEO_GOLDBERG_192 | 192 | 270 | 92 | 1 | goldberg level 3 |

**★ 6ico Compound** — V=144 · E=576 · F=576 · C=144. "18tes": 18-tesseract with triangle tessellation field. The WORKING field for KIS-timeline.
Mechanism: parameters before entry → GeoType selects shape → `sort → distinct count → codebook size` → mask = distinct values fitting geometry vertices. No hash, no lookup — coordinate = address.

### KIS-Timeline (`core/kis_codec_v4/v5/v6.h`) — KIS = FIELD, not pipeline
`∞ ← contraction ← 0 ← expansion → ∞`, enter anywhere. Forward = expansion, backward = contraction. Direction = value. Loop transition dodeca ↔ icosa through spike vertex. Spike = dual transform (face ↔ vertex).

### Core Principle
> **MAP not COMPRESS** — Geometry IS the address space. Coordinate = data. No hash, no collision, no lookup table.

## Rescope — Scale Timeline + 1 Tesseract (2026-08-14)
We don't build geometry — combinatorial structure is only a mapping template.
- **Scale** = constant multiplicative rate `s(t) = s₀·kᵗ` — no 0, no infinity. Window `(0, 20736)`; 20736 = 144² = 1728×12 = 18 tes × 8 cube × 144. One global scale → append needs no scale tag.
- **Hyperbolic side** = passive scale-change log (entries = short routes). delta ∝ event count, not data size. Matching scale → lossless directly; else replay log → deterministic lossless.
- **1 tesseract** = 8 cube × 144 = 1152. cube 0 = index frame (base/len/stride/checksum), cube 1..7 = data scattered by route (stride coprime to 144).
- **18tes** = 18 × 8 × 144 = 20736, full field tested (`test_tess_index_frame` 7/7, `test_tess_scale_log` 10/10, `test_tess_frame_seek` 8/8, `test_tess_magnify` 12/12, `test_tess_hex_delta` 10/10).

## Latest State (condensed — full log in git history)
- **Proven lossless**: Platonic Field P1-4, `test_platonic_integration` 6/6, MoE bake 108/108 + graft bitwise-identical, `.tess` 291/291 (1181 files), `.tesspack` 2.87 GB / 43,596 capos, tesspack graft/view/stream/breathe on Qwen3-4B-MoE, KV/state/GeoFS/RID, Breathing FS mmap proof (3.5 GB file, 48 MB RSS).
- **Scale bridge**: `core/scale_bridge.h`, `test_scale_bridge` 35/35, TIER1 122/122 → `docs/PIPELINE-MAP.md` §5.
- **Baseline**: TIER1 121/121, TIER2 4/4 (re-verified 2026-09-05; 44,319 capos 0 fail).
- **Multi-format**: Kokoro-82M ONNX, Bonsai-4B Q1_0, Qwen3-VL-2B Q4_K_M, LFM2.5-8B Q4_K_M — all lossless.
- **GPU**: DRamTile zero-copy 47 GB/s (T4), sig32 XOR-fold descriptors 63.56 GB/s, Qwen3-0.6B Q8_0 scatter decode verified lossless on real model.

## Tools Reference (`make <target>`)
| Tool | Purpose |
|------|---------|
| tess-bake / tess-load / tess-stream | GGUF → .tess / .tess → weights / per-capo streaming |
| tess-assemble | .tess + metadata → GGUF |
| tess-packer / tess-gguf-pack | dir → .tesspack / GGUF → .tesspack directly |
| tess-graft / tess-view / tess-breathe | .tesspack → GGUF / assemble+inference verify / mmap RSS |
| moe-bake / moe-graft / moe-stream / moe-route | MoE expert bake / graft / top-K streaming / combined |
| scale-follow | sequential vs random page-touch proof |
| docs-svg | re-render `docs/*.svg` from `docs/*.excalidraw` (never drift) |

## Build
```bash
gcc -O2 -Wall -o tests/kis_codec_v4_test tests/kis_codec_v4_test.c -lm
./tests/kis_codec_v4_test
```
Extra tools live at `I:\tools`. Deep docs: `docs/PIPELINE-MAP.md`, `docs/PLATONIC_FIELD_DISCOVERY.md`, `graft/INDEX.md`.
