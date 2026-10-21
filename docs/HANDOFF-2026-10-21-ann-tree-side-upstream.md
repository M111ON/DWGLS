# HANDOFF-2026-10-21-ann-tree-side-upstream.md

> **This is an upstream pointer doc. The live ANN root is `I:/tools/ann`.**
> Full handoff: `I:/tools/ann/docs/HANDOFF-2026-10-21-ann-tree-side.md`
> Prior: `docs/HANDOFF-2026-10-20-ann-isolate-upstream.md` (A: perm-sidecar, B: door)

## What changed in the ann fork (2026-10-21)

The session started from the 10-20 handoff's open item (flip `ANN_DOOR` after a live A/B) and found a bigger problem first: **the live server was silently broken** — `items_scored: 4` out of 62,204 rows while `/health` reported `routed: 1` (a false green).

**The layer fix (owner-verified architecture, session 2026-10-10 §5):** routing labels belong to the TREE, not to the FRUIT. `build_index.py` rebuilding the corpus WITHOUT the `anchor` fields is correct — the fruit carries no tree state. The fork now keeps the labels in the tree's channel (`<index>.stamp`) and never rewrites the corpus.

1. **Dead route → fail loud.** The incremental regrow no longer re-derives the prefix labels from the fruit's vectors; it READS them from the tree's sidecar. If they are gone it fails loud (`mode:"no_tree"`, rc=-3) instead of silently wiping 62k labels. Search in that state reports `mode:"dead-route"` and does an exact full scan rather than serving a handful of rows as the top-k.
2. **A regrow never rewrites the fruit.** The full path used to rewrite the 233 MB jsonl and re-serialize every vector with `"%.6g"` (6 significant digits) while `build_index.py` writes `round(x, 6)` (6 decimals) — a lossy re-encoding. Labels now go to the sidecar only; the jsonl stays byte-identical (verified by md5 + mtime).
3. **New `POST /v1/state/restamp`** — re-places the fruit on the tree without touching it (jsonl md5 unchanged).
4. **`/health` gained `stamps_live` + `route_dead`** (`status` unchanged — the keepalive keys on it).
5. **The door's recall contract.** The door REPLACES the below-floor fallback (which scans every row). Keeping the pruned cap made it score fewer rows than the path it replaced — measured against a numpy oracle on 300 real queries: recall@5 0.9307 vs 1.0000, 56/300 queries worse. The handoff's "recall cannot drop" claim was false. Fixed (`cap = nent`): now 1.0000 both arms, 300/300 paired equal. It is still slightly SLOWER than the scan it replaces and is almost never reached at the production floor, so it **ships OFF** (`ANN_DOOR=0`).
6. `ann_route_watch.py` gained an H2 heal (dead route → restamp → fallback full regrow); `ann_sync.py` now always regrows after a rebuild (the old "regrow == sig" shortcut skipped it and left the tree dead).
7. Upstream bug found while reviewing our own diff: `d48bc08` had `pseen2 = pseen2` (self-assign) → buffer leak on every door query. Fixed.

## Interface deltas (for anything calling the ANN server)

- New `POST /v1/state/restamp` → `{status, mode:"restamp", nent, K, ms, restamp:{mode, rc}}` (rc 0 ok / -1 no usable table / -2 alloc fail).
- `GET /health` adds `stamps_live` and `route_dead`.
- `route.mode` may be `dead-route`.
- `regrow.mode` may be `no_tree`.
- New env: `ANN_DOOR_EXIT` (default OFF) + `ANN_DOOR_EXIT_FLOOR` (separate from the trigger floor).

## What this means for DWGLS-native-fs

No code changes here. The 20736/CAPO/frustum/metatron headers remain the address-layer library. The tree/fruit separation ("the fruit is a closed cluster; the tree can break and the fruit stays the same") is now enforced in the fork's code rather than only in prose — the pattern for anything that separates payload from routing.
