# ANN Upgrade Plan 2026-10-22 — note-as-source-of-truth → 3-axis latch

**Status:** plan (no code yet) · **Date:** 2026-10-22 · **Live root:** `I:/tools/ann` (git root `I:/tools`, branch `main`)
**Upstream pointer:** this file (the ANN side gets its own copy next session)
**Origin:** owner session 2026-10-22 (ANN / hourglass / frustum-anchor thread) — idea only, nothing built yet

---

## 0. Scope guard — read this first

**IN SCOPE:** the ANN search service in `I:/tools/ann` — the route layer, the tree channel
(stamp sidecar + order), the fog latch (P3 / IR note), the watcher, `/health` truthfulness.

**OUT OF SCOPE — do not mix these in (owner emphasis 2026-10-22):**

| Not this | Why it is separate |
|---|---|
| **model weights** (GGUF / tess / tesspack / breathing_fs) | different chain, different repo layer. The "shift / delta" language here is about **the ANN route**, not about compressing or relocating weights. |
| **KV × ANN split** | already decided and parked: KV is a *user* of ANN, a different field (memory #7587; artifacts moved to `experiments/kv-user-2026-10-11/`). This plan does not touch KV. |
| **hourglass / frustum composite rescope** | different cards (`task-td7fa1248`, `task-t5289298c`). This plan only *borrows the 6-face / 3-pair idea* for the latch id space — no frustum headers, no `pattern_probe`, no `minor_forage`. |
| **re-anchoring the anchor table onto 3 axes** | parked as phase 3 (§2.3) — no code, no measurement yet. |

---

## 1. Current state — verified this session (file:line receipts)

| Thing | Where | Note |
|---|---|---|
| 3 layers separated | `I:/tools/ann/docs/HANDOFF-2026-10-21-ann-tree-side.md` §1 | FRUIT = `ann_index.jsonl` · TREE = `<index>.stamp` (8 B/row: anchor, anchor2) + `ann_index.order` · TABLE = `ann_anchors.bin` |
| fog latch id | `src/ann_search.c:588-591` | `cell = bucket % 144` → flat 1-D, no direction, no neighbours |
| latch replay at boot | `src/ann_search.c:570-585` | `lz_fog_init()` → `wl_replay_log_file()`; warm path behind `ANN_TOMBSTONE_WARM` (≈`:571-575`) |
| the "note" | `include/geo_id_route_log.h` | `IR_LOG_RECORD_SIZE 32` (`:44`) · actions WRITE/CLEAR/MOVE/ANCHOR (`:49-52`) · crc32 (`:75`) · replay (`:202`) |
| route cache | `src/ann_search.c:1061-1063` | key = (mtime, size) of anchors+order → a cache, not a truth |
| H2 binding | `src/ann_search.c:1073-1075` | `ord_load()` must carry the table's K/ntr/checksum — mismatched pair refuses |
| P3 compaction | `scripts/p3_compact.py` | 5661 → 7 records, lossless by construction (card `task-tdb4e839d`) |
| warm path | `src/ann_search.c` + `docs/WARM-PATH.md` | built + measured 2026-10-14: warm scores fewer, novel recall identical; budget bug fixed (de73665: only skip when `cov < budget`) |
| watcher | `ann_route_watch.py` (2026-10-12) | exists and runs; the **remaining** part is card `task-t87ad8462` (watcher owns rebuild+retrain+swap + a gate that ALERTS) |
| baseline to protect | `docs/HANDOFF-2026-10-22-ann-scope-loader.md` §1 | recall@5 vs global oracle: **route 0.910** · kind 0.698 · file 0.442 · tag 0.34 (300 queries) |
| HNSW framing | `docs/ANN-CLIMATE-CAMPAIGN-2026-09-24.md:23-24,96,106-107,184` | ours: recall@10 0.9617 / 32.1 ms / 0.44 MB anchors / regrow 0.4 s per slice · HNSW: 0.9894 / 0.38 ms / ~300 MB / ~356 s |

**The problem this plan attacks:** the tree channel has **two sources of truth** (the regrow-written
sidecar + the walk note) joined by a positional overlay. The overlay is where the silent deaths
live — dead route (sidecar overlay of `(-1,-1)` over 62k rows) and append window (sidecar shorter
than the index → every label dropped) — both in `HANDOFF-2026-10-21-ann-tree-side.md` §2.1 / §2.4,
and in both cases `/health` still said `routed:1`.

---

## 2. The upgrade, in three phases

### 2.1 Phase 1 — the note becomes the source of truth; sidecar + order become caches

**Change:** the regrow writes ANCHOR / MOVE records into the IR note. `stamp` + `order` are then
*derived* from (note + table), exactly like the latch already is (replayed at boot). A lost,
truncated or stale sidecar becomes a **cache miss**, not a dead tree.

**Cost:** 32 B/row on a full rebuild ≈ 2 MB (index is 258 MB) · boot replay of 62k records
(must be measured, not assumed) · compaction already exists (`p3_compact.py`).

**Correctness gates — every one is measured, none asserted:**

| Gate | What must be true |
|---|---|
| G1.1 | truncate the sidecar to 0 / write garbage → the server must **fail loud** (or regenerate) — it must never silently widen to a brute scan |
| G1.2 | regenerate from the note → `stamp` + `order` **byte-identical** to the originals (hash compare, not "looks right") |
| G1.3 | keep the sidecar, delete the note → the server must refuse (or regenerate the note from the stamp) — pick one, assert it, document it |
| G1.4 | recall@5 unchanged: 0.910 ± 0.005, same 300 queries, same numpy oracle |
| G1.5 | `/health` truth: `stamps_live == entries`, `route_dead == 0`, and the gate **ALERTS** when they disagree (this is the same gate card `task-t87ad8462` wants — build it once, use it for both) |
| G1.6 | boot: replay of 62k note records ≤ the current stamp+order load time (report both numbers) |
| G1.7 | the fruit is never rewritten by the regrow (already true since 9c532e8 — assert it, do not re-litigate) |

**Explicit non-goal:** no change to the *routing* (anchor table, budget, topb) → no recall risk in phase 1.

### 2.2 Phase 2 — the latch gets geometry: 144 = 4 × 6 × 6, six faces = three pairs

**Change:** the latch cell id becomes `(face 0..5, u 0..5, v 0..3)` instead of a flat `0..143`.
Bijective remap of the same 144 ids → **the route and recall do not move at all**.

**Where the numbers come from:**
- `144 = 4 × 6 × 6` — `docs/ANN-SYSTEM-REPORT-2026-10-08.md` §5.3 (the storage ladder and the
  tesseract ladder are the same product)
- six faces = three opposite pairs — `core/isometric_map.h:88-99` (faces 0-5 = X+,Y+,Z+,X-,Y-,Z-),
  `core/frustum_trit.h:8` `FACE_COUNT 6` ("cube faces / half-axes")

**Why it is worth doing:** the warm path currently skips a *flat* cell. With a face axis it can say
"the +X face of this cell is shut, the −X face is open" → a spatial skip, and the six faces are
one object seen isometrically (`core/isometric_map.h:271` `im_project`).

**Gates:**

| Gate | What must be true |
|---|---|
| G2.1 | the remap is a bijection on 0..143 — proven in a test, not a comment |
| G2.2 | warm path still scores **fewer** items than cold at the same budget (the de73665 rule stands: only skip when `cov < budget`) |
| G2.3 | novel recall unchanged (1.0000 at 60 queries, the C1 arm of the warm bench) |
| G2.4 | an isometric/hex view of the latch renders, and the faces it shows shut are exactly the ones the walk skipped — a receipt, not a claim |
| G2.5 | the three pairs are **separate slots**: shutting +X must not imply −X is shut (a one-bit-per-pair bug would silently over-skip) |
| G2.6 | the latch id space is not confused with the *scoring* space: bucket ids come from centroid clustering and carry no spatial meaning. A structured latch must not be read as a semantic neighbourhood |

### 2.3 Phase 3 — PARKED, no work: re-anchor the table onto three axes

Bucket ids are centroid indices → no spatial meaning. Re-anchoring the table changes the routing
table → recall risk with no measured gain. Revisit **only** if phase 2 shows a real spatial win in
the walk. If it happens, it is a separate card (and it needs a new recall baseline).

---

## 3. Rules that must hold through all phases

- The **fruit** (`ann_index.jsonl`) is never rewritten by the regrow. Tree-side only.
- The regrow **never touches vectors** (owner rule 2026-10-10: regrow cannot move vectors; capo
  rearrangement has no effect on the vectors).
- The note is **append-only**; never edit a line. Each record carries its own CRC.
- **No cross-include** from `DWGLS-native-fs` into `I:/tools/ann` (rule #7587: take the *structure*,
  never another repo's code). Phase 2 needs the "three axes × two poles" *idea* — write it locally.
- Everything in this plan is **value-blind**: no code may look at what a row means (memory #7171).

---

## 4. Open decisions for the owner (do not start these before answering)

1. **G1.3 authority:** if exactly one of note / sidecar survives, is the server allowed to
   regenerate it, or must it refuse and wait? (Regenerate = self-heal, refuse = loud.)
2. **Note granularity:** one append-only file for the whole index, or per-slot segments
   (P3 compaction already works per slot)?
3. **Note record size:** 32 B/record is what `geo_id_route_log.h` has today. The fibo path in
   DWGLS used a 5 B/route variant. Is 32 B acceptable, or should the record be packed down?
4. **Phase 2 scope:** latch geometry only, or also the isometric view as a live view?

---

## 5. Where the work lands

| Path | What |
|---|---|
| `I:/tools/ann/src/ann_search.c` | route layer, latch, `/health`, loader |
| `I:/tools/ann/include/geo_id_route_log.h` | the note format (32 B records) |
| `I:/tools/ann/include/geo_wang_latch.h` | the latch (wl_* API) |
| `I:/tools/ann/scripts/` | `p3_compact.py`, `build_index.py`, `ann_sync.py`, `ann_route_watch.py` |
| `I:/tools/ann/tests/` | `run_tests.py` is the entry point (`python run_tests.py`, ALL PASS expected) |
| `I:/tools/ann/docs/` | handoffs live here; add `HANDOFF-2026-10-22-ann-note-truth.md` when phase 1 lands |
| `I:/DWGLS-native-fs/docs/` | this file (upstream pointer) |

**Board cards:** see the two cards posted 2026-10-22 (parent = plan, children = phase 1, phase 2).
Existing ANN cards that must not be duplicated or contradicted: `task-t87ad8462` (H1 watcher + gate),
`task-tdb4e839d` (warm path + P3 compaction, done), `task-t4fbea5cf` (per-zone mini vector DB),
`task-t5ba6ae63` (tags endpoint), `task-tabbd47c5` (soft zone bias), `task-t5289298c` (frustum rescope).

---

## 6. One-line summary

Make the walk note the only truth for the tree channel (sidecar and order become caches that are
regenerated, so a lost sidecar can no longer kill 62k labels silently), then give the latch a
three-axis geometry so the warm path can skip spatially instead of flatly — verified by
fail-loud gates and byte-identical regeneration, never by assertion.
