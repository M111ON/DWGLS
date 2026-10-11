# HANDOFF 2026-10-11 (late) — D5 recall, P3 log leak, KV/ANN separation

**DWGLS repo:** `I:\DWGLS-native-fs` · `feat/geo-native-fs` · HEAD **`e7326dc`**
**ANN repo:** `I:\tools\ann` · `main` · HEAD **`5b20435`** (ann core clean)
**Earlier today:** `docs/HANDOFF-2026-10-10-session.md`, and the mid-day
`docs/MISS-WRITE-2026-10-11.md` (owner) which predicted the log leak fixed here.
**Live:** ann :8096 healthy, `index=…\data\memcore\ann_index.jsonl`, entries
59756, `routed:1`, embed ok.

---

## 1. Three things done this session (all measured, all committed)

### A. D5 — the search budget was never actually 8000 (fixed + tuned)
`LZ_SEARCH_BUDGET_DEF` and the live keepalive env both sat at 8000, but a
silent branch `else if (nent > budget) budget = nent` widened any budget
smaller than the index to a **full scan** — so the default number was inert
and truncation was never reported (health said `trunc=0`).

Measured with `bench_budget_recall.py` (40 real index rows as raw-`vec`
queries vs a numpy brute-force oracle, production env prune=1/top-5):

| budget | R@1 | R@5 | R@10 |
|---|---|---|---|
| 8000 (old) | 0.4250 | 0.4700 | 0.4675 |
| 16000 (knee) | 0.9750 | 0.9000 | 0.8950 |
| **24000 (new default)** | 1.0000 | 0.9550 | **0.9600** |
| full 44938 | 1.0000 | 0.9950 | 0.9950 |

⇒ old default cost **~53% of R@10**. Fix: raise default to **24000** and
**delete the widen trap** (a default is meaningless if anything larger than
the index silently becomes full scan). `truncated` is now honest: live route
reports `scored=24000/59756 trunc=1`.

**Gotcha that nearly produced a wrong verdict:** the first scratch sweep ran
WITHOUT the production env (`ANN_WALK_PRUNE=1`, `LZ_ROUTE_BUCKETS=5`,
`ANN_WALK_FLOOR=0.5`) and measured R@10 = scored/N (uniform-sample shape).
The env is what makes the route steer — always mirror production env before
measuring recall.

### B. P3 latch log leak (real bug, fixed in both repos)
`lz_route.log` had **5661 records for 7 distinct slots (808×)**. Both serve
paths call `wl_traverse_log` on *every* walked bucket, but the latch is
monotone — only the first walk of a bucket is a fingerprint. Records grew
~154 B/query forever and boot replay grew linearly. Owner's `MISS-WRITE` note
had already flagged it; the transition-only guard was never coded.
Fix (caller-side, vendored headers untouched): guard with `wl_is_open` before
`wl_traverse_log`. Live proof: 6 repeated queries → log delta **0 bytes**.
Regression gate: `tests/test_wang_latch.c` T6 (both repos).

### C. KV separated from the ANN core (owner ruling)
**KV is a USER of the ANN route, not part of ANN — different fields.**
KV manages its own storage and *feeds a result to ANN*; ANN must never carry
KV vocabulary/paths/format.

- Moved `kv_tess_cache.h` + `test_kv_tess.c` out of `I:\tools\ann` into
  `experiments/kv-user-2026-10-11/` (compiles standalone with vendored route
  headers; ALL PASS). Non-breaking: neither was in `TESTS`, neither was
  included by `ann_search.c`.
- Renamed the leaked `kv_*` defaults in the ANN core to `ann_*`
  (`data/ann_index.jsonl`, `ann_anchors.bin`, `ann_index.order`); data copied
  to both names; keepalive prefers `ann_*`, falls back to `kv_*`.
- `/v1/state/search` endpoint name kept (live API contract) — legacy naming
  noted; rename to `/v1/search` only with caller migration.

## 2. Receipts

- DWGLS: `6c11cef` (A-series gates + A2 variance + A3 ray verdict),
  `3e309ff` (P3 transition-only), `07cf198` (D5 24000, trap removed),
  `e7326dc` (KV→experiments).
- ANN: `aa68faa` (budget-recall bench), `ca3a212` (D5 24000, trap),
  `b077552`+`4fb4b5e` (P3 guard + T6), `5b20435` (KV separation).
- Tests green: DWGLS TIER1 154/154 + TIER2 4/4; ANN 5 + 13 + wang_latch ALL PASS.
- Memory: #7584 (P3 transition-only rule), #7585 (budget-vs-recall data),
  #7586 (D5 applied config), #7587 (KV=user architecture).
- Board: `task-t284a0b95`, `task-td93679eb`, `task-ted7652e7` (done).

## 3. Honest caveats

- The 24000 default came from **40 queries**. Raise to 200–500 before treating
  it as permanent (oracle is 0.2s; cost is one server spawn per budget).
  Tunable via `LZ_SEARCH_BUDGET` without rebuild.
- **A3 ray-order finding stands as a warning**: the frustum ray key
  (`af_anchor_descent` FNV over branch_path+level) *varies* (64 rays / 4096
  anchors) but is **orthogonal** to pattern-L1 nearest — nearest-shares-ray
  only ~1%, mean in-bucket rank 195. Bucket-major pruning helps (3.3× at equal
  budget) but any further "prune by walked bucket" work must first show the
  walked set correlates with true top-k.
- One transient `test_pair_table` RUN FAIL appeared once right after a
  rebuild window (it mmaps 4 real GGUFs from `I:/model/`; a transient lock).
  Direct 10/10 + suite 3/3 PASS — not a regression, but it is flake-prone.

## 4. Left open (not done — do not confuse with broken)

- **A1/A4/A6**: Metatron 13-cell wiring / 96 grid / hex address.
- **Frustum rescope probes**: `pattern_probe.c` L6 (grid→frustum overlay),
  `minor_forage_probe.c` Visit needs direction (card `task-td7fa1248` rev 3).
- **H1 cold-path watcher** (from 2026-10-10 handoff §7.1) — still the only
  live-touching job on the list; unstarted.
- **MoE router join + KV park path** in `tools/tesspack_server.c` (dense-only).
- Other session's untracked `tools/exp_partition_ray.c`,
  `tools/exp_tetra_octa.c` — **not mine, untouched** (per the do-not-touch rule).
- Historical docs (FORK/GRAPH-ROUTE/HANDOFF/STRESS-REPORT) intentionally keep
  the old `kv_` names — records, not live config.

## 5. Re-use rule for KV ↔ ANN

KV calls ANN **through ANN's own interface** (index/route returning
candidates). Take the *structure* (anchor route), never a cross-include. ANN
never learns KV's storage format.
