# octfold — 8-fold ladder as 10th language (2026-10-14)

**Status: experiment, wired behind an opt-in Makefile target.** Not in `make test`.
Runs only via `make octfold` (probes + real-GGUF roundtrip + multi-view bridge).

Source: regolo54 2018 — 8-fold Islamic star (square + octagon). Question was whether
the 8-fold **scale ladder** (√2 per level) is a real *addressing language* on the
existing 60-slot carrier, not just a pretty drawing.

**Full report: [`REPORT.md`](REPORT.md)** (address law, ring-24 embed, quadtree
comparison, G1–G6 evidence, honest status).

## Files
- `octfold_view_probe.c`   — G1–G5: bijection, inverse, pure-int grammar, oracles, mutation
- `octfold_roundtrip.c`    — G6: full GGUF bake → readback → rebuild → damage → persist
- `octfold_bridge_probe.c` — bake ONCE via octfold, cross-read hosoya/limacon/reverse
- `evidence/`              — pixel crops from the source image (gitignored, regenerable)

## Run
```sh
make octfold            # all three
make octfold-probe      # G1-G5 only (no model needed)
make octfold-roundtrip  # needs $(LLAMA_GGUF)  [Qwen2.5-0.5B-Instruct-Q8_0.gguf]
make octfold-bridge     # needs $(LLAMA_GGUF)
```

## Result (2026-10-14, Qwen2.5-0.5B Q8_0 · 675.7 MB · 5156 parts)
- G1–G5: ALL GREEN (9/9)
- G6: 5156 parts lossless · rebuild byte-identical · damage localized+restored · twin 676.3 MB
- Bridge: cross-read hosoya/limacon/reverse bad=0 · XOR-consensus YES

## Honest scope
`octfold` is a **drawing-derived constructed** view (same category as pascal/hexagram,
languages 7–8), NOT an independently-discovered one like the snub enantiomorph pair.
Proven: it is a valid 60-slot permutation passing all six `docs/LANGUAGES.md` gates on a
real 675 MB GGUF. NOT claimed: that the 8-fold star is *forced* by the carrier.
