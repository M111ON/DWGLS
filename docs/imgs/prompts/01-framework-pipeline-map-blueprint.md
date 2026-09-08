---
illustration_id: 01
type: framework
style: blueprint
article: docs/PIPELINE-MAP.md
seaart_model: Seedream 5.0
seaart_model_id: d6eqbble878c73dhco9g
seaart_model_version_id: 1ad9c231-1945-4e50-a24d-d2c7570338ad
resolution: 2848x1600
gen_mode: 1
aspect: 16:9
---

DWGLS PIPELINE-MAP - Blueprint Framework

Layout: left-to-right engineering schematic with grid.

ZONES:
- Zone 1 Source: model GGUF qwen3-4b-moe-q4_k_m.gguf
- Zone 2 Bake: tess-bake to 1181 .tess files, 722 MB; tess-gguf-pack direct
- Zone 3 Field template: 18 tes x 8 cube x 144 slots = 20736; capo = 1152; stride-37 scatter; cube0 index frame; cube1..7 data
- Zone 4 Container: one .tesspack file, 2.87 GB, 44,319 capos
- Zone 5 Serve: per-capo stream, graft to GGUF, view, breathe
- Zone 6 MoE plus llama.cpp inference; proof gates: TIER1 121/121, TIER2 4/4, verify 44,319 capos with 0 failed

LABELS: 20736, 1152, 1181, 722 MB, 2.87 GB, 44,319, TIER1 121/121, TIER2 4/4, 93.8 percent, 2.1x, 48 MB RSS
COLORS: deep blueprint blue background, white and cyan schematic lines, amber highlights. Color values are rendering guidance only; do not display color names or hex codes as visible text.
STYLE: technical blueprint schematic, precise thin lines, grid, monospace labels, clean composition with generous whitespace, centered zones, no photorealism, no people.
ASPECT: 16:9
