# Address Formula Decision — 2026-10-07 (#58 item 4)

**Question (from #6540):** does the new address formula replace, or coexist with,
`addr = layer·60 + viewpos(w)`?

**Answer: COEXIST, in a fixed layer hierarchy.** They are not competing formulas
— they address different layers of the same stack and compose.

## The two formulas

| formula | scope | source |
|---|---|---|
| `addr = l·60 + viewpos(w)` | **L2 slot address** — one part's 60 RID slots × pent/tri/snub/hosoya/zeck view | `core/layer_manifest.h` L11, `tools/geofs_rid.c`, `tools/kv_rid_serve.c`, `docs/LANGUAGES.md` |
| `addr_slot ∈ [0, 20736)` | **flat field coordinate** — the single outermost frame, top-level identity | `core/geo_id_contract.h` (P2 5-tuple) |

## Why coexist (not replace)

1. **Different layers.** `layer_manifest.h` defines the stack explicitly:
   L0 bytes → L1 parts (128KB) → **L2 slots (`addr = l·60 + w`)** → L3 views →
   L4 frames. The P2 contract's `addr_slot` is the *frozen flat coordinate* the
   bytes live at under all layers. One is a per-layer sub-address, the other is
   the flat coordinate it resolves to.

2. **They compose.** `l·60 + viewpos(w)` produces a flat index that lands in
   `[0, 20736)` when `l` counts layer-parts (`60 × layer_count ≤ 20736`). So the
   L2 formula is the *producer* of `addr_slot`; `geo_ident_from_name` /
   `geo_ident_resolve_addr` are the *consumers* that bind that slot to
   (name_hash, bytes_hash, walk_round, walk_tick).

3. **The 20736 frame is inviolable (#6337).** No container may be placed below
   KIS/hyper that multiplies the frame beyond 20736. `l·60+viewpos(w)` stays
   inside the frame by construction; the flat `addr_slot` IS that frame. Replacing
   one with the other would either lose the per-layer view structure (replace
   flat→L2) or lose the top-level identity binding (replace L2→flat).

## Composition rule (the contract)

```
name ──hash──▶ name_hash
  │
  └──rid/language──▶ addr = l·60 + viewpos(w)   [L2 slot address]
                        │
                        └──(flat, mod 20736)──▶ addr_slot ∈ [0,20736)   [P2 flat]
                                                    │
                                          bytes at slot ─▶ bytes_hash
                                                    │
                                              walk ──▶ (walk_round, walk_tick)
```

`geo_id_contract.h` owns the **flat** end (`addr_slot`, 5-tuple, replay).
`layer_manifest.h` owns the **L2** end (`l·60 + viewpos(w)`, view bijectivity
R2, multi-view XOR consensus R3). The two ends are joined by `addr_slot =
(l·60 + viewpos(w)) % 20736`.

## Status of code

- Both formulas are live and used (RID/KV serve tools use the L2 form; P2/P3/P4
  contract+log+feedback use the flat form). **No divergence to fix** — the
  decision is that this is *by design*.
- Any future tool that addresses a model part must go through both: pick the
  L2 slot (`l·60+w`), fold mod 20736 to `addr_slot`, bind via the 5-tuple.

## Receipts

- `core/layer_manifest.h` — layer stack + R1/R2/R3 rule constants.
- `core/geo_id_contract.h` — 5-tuple, `addr_slot`, resolvers, replay.
- `tests/test_id_contract.c` 29/29 (GEO_FAST).
- `docs/LANGUAGES.md`, `docs/HANDOFF-RID-PIPE-2026-08-23.md` — L2 formula spec.
