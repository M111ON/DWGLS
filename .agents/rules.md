# rules.md — MANDATORY for every app, every session (L1)

Read this file fully before doing ANY work. No exceptions, no matter how small your context is.

## Language / Scope
- Communicate in English and Thai only.
- Working dirs: `I:\DWGLS-native-fs`, `I:\model`, `F:\model`, `I:\llama`.

## Core contract
- **MAP not COMPRESS** — geometry IS the address space. Coordinate = data. No hash, no lookup table for address resolution (static-geometry LUT only).
- **No geometry construction** — never compute vertex/face/projection/coordinate. int-only, static LUT, modular arithmetic. Combinatorial structure is a mapping template, not a calculator.
- **Timeline-first**: int, base-2 scale, no 0, every state deterministic + replayable.

## Sacred constants
12 (dodeca) · 20 (icosa) · 24 · 30 · 60 · 72 · 92 · 120 · 132/192 · **144 (★ 6ico protagonist)** · 576 · 20736 = 144² (one cell, not the whole system).

## Verification (binary truth only)
- Lossless = decode → compare every value at every position. `geo_codec_verify()` decides.
- Ratio < 1.0 must prove via decode. Never trust encode-only.

## Test integrity (will be checked)
- Expected values ONLY from an independent oracle: spec / math / source data / reference implementation. Never from the function under test (`f(x) == f(x)` = tautology, rejected).
- Never "run then paste output as expected" — that freezes bugs.
- Never copy spec/comments from implementation into tests — spec comes first.
- Every test must be able to actually fail (mutation check: change 1 line in core → test must go red).

## History, not deletion
- Every experiment/debug scratch is history — move to `deprecated/`, never delete.
- Only regenerable large artifacts (`.tesspack`/`.gguf`) may go, and only after asking.

## Stocked branches (do NOT open before mainline is done)
- `docs/ARCHIMEDEAN-STOCK-2026-08-22.md` — Hosoya/circle view · snub chiral · Zeckendorf · circle-config catalog.
