# HANDOFF — RID Direct Pipe Complete (2026-08-23)

## State: MAINLINE SOAKED — all layers proven on real GGUF

Branch: `feat/geo-native-fs`

## What's Done (this session)

### Proof chain (all oracle-pass, soak 3/3)

| Tool | Make target | Layer | Proof |
|---|---|---|---|
| `tools/geo_snub_test.c` | manual | geometry | snub dodeca = RID + chiral diagonals, parity solve → exactly 2 enantiomorphs (F=92=GEO_GOLDBERG_92) |
| `tools/geo_rid_serve.c` | — | data plane | bake real GGUF → RID slots, 3-language XOR verify + damage drill |
| `tools/geo_rid_graft.c` | `make rid-graft` | weights | llama b9733 inference through slot region: tokens + logits@0 BITWISE (151936 dims) |
| `tools/geofs_rid.c` | `make geofs-rid` | filesystem | GeosVolume ⇄ slot region twin mmap: persist/reload byte-identical + damage drill |
| `tools/kv_rid_serve.c` | `make kv-rid` | state | llama state checkpoint/restore: logits@restore BITWISE + tokens identical |
| `tools/gguf_roundtrip.c` | `make gguf-roundtrip` | file | FULL 675.7MB GGUF roundtrip byte-identical × 3 languages |

All four storage layers (weights / files / state / whole-file) share ONE addressing scheme:
`addr = layer·60 + viewpos(slot)` with language views pent/tri/snub over 60 RID slots.

### Key lessons (recorded)

- Snub proof: local diagonal rules FAIL on odd cycles — solve globally via union-find parity; exactly 2 solutions = enantiomorph pair.
- llama state restore: logits capture is position-sensitive — off-by-one shows as maxdiff ~10 (not noise). Compare at matching decode position.
- KV/state files are NOT prefix-nested → delta compression net loss; direct link wins.
- GeoFS block store is only ~1.3MB (20736×64B); full GGUF needs streaming parts (gguf_roundtrip pattern), not bulk volume serialize.

## Next Queue (priority order)

1. **Open stocked branches** (`docs/ARCHIMEDEAN-STOCK-2026-08-22.md`):
   - Hosoya/circle 4th language view — bijection roundtrip word-index ↔ slot-index on same window
   - snub chiral switch — runtime enantiomorph toggle (bit per edge)
   - Zeckendorf decomposition — word sums → Fib ladder mapping
2. **Interop bridge** (from discussion): wrapper GGUF↔DWGLS reader so external tools read without knowing formula; or embed formula metadata in container header.
3. **18tes upgrade path** (GEO_COMPOUND_144 protagonist): current 60-slot hub → 144-slot field when multi-model serving needed.

## Environment

- Model: `I:\model\Qwen2.5-0.5B-Instruct-Q8_0.gguf` (675.7MB)
- llama.dll: b9733 chain (see AGENTS.md Build section)
- Twin mmap files in `build/` (gitignored)

## Vault

Session summary: `[[Memory/Sessions/2026-08-23_dwgls]]`
