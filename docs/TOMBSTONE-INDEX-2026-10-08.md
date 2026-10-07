# Tombstone Index — the fingerprint that survives restart (2026-10-08)

## Question

The tombstone framing: a route is *learned by walking*. First visit → run →
open the gate → drop a fingerprint. Second visit → the fingerprint is already
there → faster. Never prune; learn from the access pattern. Static k-means
clustering (cut by vector position) can never work, because it does not know
which query is asked often. A tombstone does, because it *was walked*.

Does the code already do this, and if so, does the tombstone actually survive a
restart?

## Answer (technical)

The doctrine already exists and is named in the code: **"enter by walking, do
not place"** (`tools/pattern_probe.c`, `core/geo_wang_latch.h`). The latch is
the map; `wl_traverse` lights a cell; the lit set IS the fingerprint. Two
walkers see two different lit sets; the data never moves.

What was missing is persistence. The latch was a process-lifetime bitset:

- `wl_traverse` set a bit but wrote nothing down;
- `LZ_ROUTE_LOG` had a *reader* (`lz_fog_init`) and no *writer*;
- so a restart reset the fog to all-OPEN — the "training" evaporated.

That gap is now closed. The bitset is explicitly a **derived cache**; the P3
route log (`core/geo_id_route_log.h`, 32 B/record, CRC-32) is the truth. Every
walk appends its fingerprint; boot replays the log into a fresh latch.

## What changed

`core/geo_wang_latch.h` — new append/replay sink (the pure `wl_traverse` stays
side-effect-free, so existing tests are untouched):

- `wl_log_open(lg, path)` — open/create the P3 log for append; writes the
  3-byte header (`IR` + version) only when the file is new/empty, and flushes
  it, so reopening an existing log never truncates.
- `wl_traverse_log(L, id, lg)` — traverse **and** append one
  `IR_LOG_ACTION_WRITE` record tagged `WL_NAME_HASH`. Reserved ids are refused,
  so nothing is appended for them.
- `wl_replay_log_file(L, path)` — boot path: fresh latch ← whole log file.

`tools/gguf_lazy_serve.c`:

- `lz_fog_init` now replays the log at boot and opens it for append;
- the search path calls `wl_traverse_log` (not `wl_traverse`) and `fflush`es,
  so a `kill -9` cannot lose the walk just done;
- the `/v1/state/search` receipt gains `fog.replayed` — records read back from
  the log at boot (0 on a cold log, >0 after a restart).

## Receipts

### Unit — `tests/test_wang_latch.c` (T1–T4 old, **T5 new**), 47 checks, ALL PASS

T5 pins the restart contract against a real file: header = 3 B, 5 walks = 5
records of 32 B, reserved id refused and not written, a fresh latch replayed
from the file is **bit-identical** to the walked one, un-walked branches stay
OPEN, and reopening appends (6 records, not truncated).

### End-to-end — live serve on the real 2881-entry index

| step | `fog.replayed` | `fog.shut` | log |
|------|----------------|-----------|-----|
| cold boot (no log), query 1 | 0 | 6 | 6 records |
| 3 more queries | 0 | 7 | 25 records |
| **kill serve** | — | — | 803 B on disk |
| **restart, first query** | **25** | **7** | tombstone already present |
| walk one new branch | 25 | 7 | 37 records (appended, not truncated) |

The warm boot answers its first query with the tombstone **already shut**, read
from the file alone — no query walked it in that process. Independent check
(`tools/wl_log_verify.c`) replays the same file and reports the 7 shut branch
ids `72,144,216,288,360,432,504` (= cells 1..7, choice 0), with a fresh latch at
0 shut for contrast.

## Honest limits (do not overclaim)

1. **This is persistence, not pruning.** The tombstone now survives restart,
   but the selection rule is still coverage
   (`anchor in walked || anchor2 in walked`) — the latch is report-only for
   candidate selection. Making it *cut* (visit hot branches first, budget-stop)
   is the next lever and is not done.
2. **Two different axes.** `docs/ANN-PRUNING-LIMIT-2026-10-08.md` measured that
   exact pruning fails for a *geometry* reason (a query's true top-8 spreads
   across clusters; k-means fit 0.934→0.944 moves exact-recall cost only
   100%→~60%). A tombstone does not fix that axis. It fixes the *hot/cold*
   axis: a repeated query reads a route that is already shut.
3. **A novel query still pays cold.** "Cut the path nobody walks → recall
   unchanged" holds only for routes already walked. The tombstone is a front
   tier (O(1) warm for repeats), not a replacement for exact search. Recall of
   novel queries is untouched by design — but that claim still needs the
   warm-vs-cold measurement (see next).

## Next (ordered)

1. Route order reads the latch: shut branches first, OPEN after, with a budget
   stop → the warm speed R8 (`docs/REAL-USAGE-CAMPAIGN-2026-10-05.md`:
   64/64 first-visit open, 64/64 second-visit shut) actually pays.
2. Measure recall **warm (repeat query) vs cold (novel query)** separately, to
   show the tombstone does not touch novel recall.
3. Compact/rotate the log (CLEAR epoch) so it does not grow unbounded.
