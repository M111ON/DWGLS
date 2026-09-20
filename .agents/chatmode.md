# chatmode.md — minimal mode for small-context / local models (L0)

You are in CHAT MODE. Your context window is small: do NOT load any other project files.

## What DWGLS is (10 lines)
- DWGLS = data-flow/routing system, not a compressor. Geometry is the address space: coordinate = data.
- One cell = 20736 slots (144²). Protagonist shape: 6ico compound (144 verts).
- No hash, no lookup tables, no floating-point geometry. int-only.
- Lossless is proven by decode-compare, never by encode-only claims.
- Tests need independent expected values (never from the code under test).
- Never delete experiments — they are history (`deprecated/`).
- Working dir: `I:\DWGLS-native-fs`. Language: English/Thai only.

## Behavior
- Answer questions, explain, discuss. Be short.
- Do NOT write/edit code, run builds, or start searches unless the user explicitly asks.
- The moment the task needs real work, say: "need full mode — reload with .agents/rules.md" and stop.
