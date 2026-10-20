# HANDOFF-2026-10-20-ann-isolate-upstream.md

> **This is an upstream pointer doc. The live ANN root is `I:/tools/ann`.**
> Read the full handoff there: `I:/tools/ann/docs/HANDOFF-2026-10-20-ann-isolate.md`
> (pushed to `https://github.com/M111ON/FS_tools.git` main `6e2dc43`, 2026-10-20).

## What the ann fork did today (summary for DWGLS readers)

1. **[A] perm-sidecar isolate** (`00d60c5`): incremental regrow no longer rewrites the ~222 MB `ann_index.jsonl` — a delta stamp sidecar `<index>.stamp` (8 B/row `anchor,anchor2`) carries only the new rows; `lz_index_load` overlays it when sized `n*8`. Unchanged fast path 1 ms; append regrow leaves the jsonl byte-untouched. Apple stays in place; the branch moves.
2. **[B] tombstone DOOR** (`d48bc08`, env `ANN_DOOR=1` default OFF = byte-identical): below-floor walk, when a routed bucket's tombstone is SHUT, is re-ordered door-first (shut bucket's members → other routed buckets → rest). Mode label `door`; one-shot (falls through to the existing full scan after one pass); graft soft-bias and full2 stay the fail-loud final fallback. Probe fact encoded: **bucket 0 maps to wl-id 0 = `WL_RESERVED_LO` and can never shut** — the door picks the first truly-shut routed bucket.
3. **Race guard** (`1e1e342`, earlier today): ann-sync appending during regrow now fails loud (`mode:"race", rc:-2`) instead of silently corrupting; incremental regrow re-saves anchors ntr (the root cause of the 2026-10-20 routed:0 brute-fallback incident).
4. Tests ported out of %TEMP% into `ann/tests/` (`ef3ab65`): `test_incr_fix.py` (8-case regrow contract), `verify_kernel_equivalence.py`, `test_batch_route.py`, plus NEW `test_perm_isolate.py` (9 cases) and `test_door_path.py` (8 cases). `rato run_tests.py` ALL PASS at `6e2dc43`. A pre-existing `test_vec_format.py` pin contradiction was fixed (`d48bc08`) — the store-oracle check now reflects the file's own docstring (99.93% near-tie tolerance), exactness stays pinned at 1e-9 on the float32==text ranking.

## Interface deltas (for anything calling the ANN server)

- New POST ` /v1/state/search-batch` (queries[], k) — telemetry has `took_ms`/`embed_ms`.
- New env `ANN_DOOR` (off unless set) and `ANN_DOOR_TRACE` (stderr debug).
- Search may now report route mode **`door`** (in addition to anchor/warm/p/graph/brute).
- Regrow may report mode **`race`** (fail-loud, retry) — never silent.

## What this means for DWGLS-native-fs

No code changes here. The 20736/CAPO/frustum/metatron headers remain the address-layer
library; the door/stamp architecture ("apple stays, branch moves, tombstone is the door")
is a consumer pattern built on the fork's side. If a merge of these mechanisms back into
this tree is ever approved, the touch points are `core/anchor_route.h` consumers and the
`geo_wang_latch` shut-first ordering, both documented in the fork handoff above.
