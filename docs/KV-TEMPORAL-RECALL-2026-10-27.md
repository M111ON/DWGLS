# Temporal KV — recall from the log, end to end (2026-10-27/28)

Status: **proven on a real model, with the boundaries named at the bottom.**
Scope: `core/kv_temporal.h` (format + rules), `core/kv_temporal_read.h` (reader),
`tools/kv_temporal_llama.c` (host), `tests/kv_temporal_test.c` (fixtures),
`I:\tools\chat-pool\kv_pool_bridge.py` (Python side).

## The claim

A chat turn is a **seekable frame**:

- the frame's *content* in the log is only its **token delta** (`kv-delta`);
- at the first step of every **round** (144 steps, memory #7501) a **anchor**
  holds the full KV base as a side BLOB, and the log carries only
  `{off,len,fnv}` (addresses in the log, bytes outside it);
- recall = resume the nearest anchor blob, then replay that round's deltas at
  their exact token positions.

The replay reads **the log**, never the producer's memory. That is the property
that was still missing before this session and is now receipts-backed.

## What was actually broken (found by running, not by reading)

Prior state: `10/12 MATCH, exit 1`, and the cross-process claim was untested.

| Symptom | Cause | Fix |
|---|---|---|
| t9 argmax flipped 785→40 with identical tokens | replaying the whole delta span in ONE batch; a quantized KV (K=Q8_0, V=Q4_0) rounds per batch, so batch shape changes the arithmetic (same class as #7032) | replay mirrors the producer's **per-turn** batching |
| t1 compared `argmax=-1` against itself | the anchor-turn frame ends *inside* the anchor blob → nothing left to decode | reported as `n/a` and counted as **skipped**, never as MATCH |
| second run pointed at the first run's bytes | blob offsets restarted at 0 while the side file was appended | offsets seed from the file size; every anchor FNV verified on read |
| appended records ignored a foreign writer's tail | `prev_chain` was read and never used | the chain follows the file tail before each write |
| every delta cost 16 KB in the log | fixed 4096-slot token array per record | per-record width (`[hdr][tokens…]`, length from `n_tokens`) |
| 764 KB log for 22 turns | same as above | 22-line log ≈ a few KB |
| anchor cadence was "every 8 frames" in the host | the host, not the header, decided | `kvt_anchor_due()` (round rule) decides; host only obeys |
| `parent` always 0 | frame was persisted before the anchor decision | persist after it, so every frame names the anchor its round hangs off |
| `%llu` / `%zu` warnings | mingw `-Wformat` rejects both in MS mode | `PRIu64` / `PRId64` |
| `kv_temporal_test` was never built | not registered in the Makefile | added to the `KV` tier |

## Receipts (commands + numbers as printed)

```powershell
# pure model + log, no llama:
mingw32-make test-kv                  -> PASS: 13  FAIL: 0   (KV tier, 13 tests)
.\build\kv_temporal_test.exe          -> === kv_temporal: 327 PASS, 0 FAIL ===
```

Long runs: a rebuild from an anchor is O(turns since that anchor), so a full
sweep of a 145-turn log is quadratic (measured: >30 min). Two modes avoid that,
both honest: verification samples long runs (first 8, last 8, every 16th), and
`noseek` runs the producer alone (`... <NT> noseek`) when what you want is the
log + anchor blob; a later `append` run then proves the resume point.

# real model, fresh 12 turns (Qwen2.5-0.5B-Instruct-Q8_0, Vulkan build):
.\build\kv_temporal_llama.exe <model> I:\Temp\kv7 <backend> 12
  anchor blobs self-verified: 1/1
  log re-read: 1 anchor(s), 12 delta(s)
  === kv_temporal_llama: 11/11 MATCH (1 skipped: anchor-turn frame) ===   exit 0

# same log, 12 MORE turns appended (turn numbering continues, ctx rebuilt from the log):
.\build\kv_temporal_llama.exe <model> I:\Temp\kv7 <backend> 12 append
  resumed from log at t12: 1 anchor(s), 12 delta(s), ctx pos=187
  log re-read: 1 anchor(s), 24 delta(s)
  anchor blobs self-verified: 1/1
  === kv_temporal_llama: 12/12 MATCH (0 skipped: anchor-turn frame) ===   exit 0

# round boundary: 145 turns -> anchors cut at t1 AND t145
.\build\kv_temporal_llama.exe <model> I:\Temp\kv8 <backend> 145 noseek
  147 log lines = 145 kv-delta (turns 1..145) + 2 kv-anchor (t1, t145)
  log 52 KB, side blob 15.9 MB   (anchor@t1 = 70,656 B; anchor@t145 = 15,808,236 B)

# a later process resumes AT the round-boundary anchor and keeps numbering:
.\build\kv_temporal_llama.exe <model> I:\Temp\kv8 <backend> 12 append
  resumed from log at t145: 2 anchor(s), 145 delta(s), ctx pos=3159
  log re-read: 2 anchor(s), 157 delta(s)
  anchor blobs self-verified: 2/2
  === kv_temporal_llama: 12/12 MATCH (0 skipped: anchor-turn frame) ===   exit 0

# one more continued turn, on the finished binary (exercises the "t" field):
.\build\kv_temporal_llama.exe <model> I:\Temp\kv8 <backend> 1 append
  resumed from log at t157: 2 anchor(s), 157 delta(s), ctx pos=3346
  [seek t158] anchor=yes replay=201 tok  argmax=151643 live=151643 MATCH
  === kv_temporal_llama: 1/1 MATCH (0 skipped: anchor-turn frame) ===   exit 0
```

Python re-read of that 158-turn log (no C in the loop):

```
deltas 158 anchors 2
anchor turns [1, 145]   base_tokens [14, 3159]   blob [(0,70656), (70656,15808236)]
deltas with tokens 158   turns 1..158 unique 158
blob fnv verify [True, True]
seek 1 -> anchor@1 replay 0
seek 158 -> anchor@145 replay 13
chain ok True lines 160
```

### Concurrency: a second writer on the same log

`tools/kv_log_check.py check <dir>` verifies the whole log as a reader would:
every line's hash covers its own fields, every `prev` is the previous line's
hash, anchors sit on round boundaries and their `[off,off+len)` regions are
unique and inside the blob file with a matching fnv.

```
# C host producing 10 turns while pool.append() writes 800 lines into the same log
python tools/kv_log_check.py check I:\Temp\kv9
  lines 811  delta turns 1..10 (unique 10)  anchors 1 [1]  blob 70656 B
  CHAIN OK — every line hashes to itself, prev links contiguous,
              anchor regions unique and fnv-verified
  (the C run itself: 9/9 MATCH, 1 skipped)

# second producer on the same live log, refused before it can touch anything
build\kv_temporal_llama.exe <model> I:\Temp\kv10 <backend> 5
  another producer already owns this log (I:\Temp\kv10\kv_producer.lock)
  exit 3     (the first producer's 10 turns stay intact: check = CHAIN OK)
```

### Scratch copy of the real pool log (the wiring the owner asked for)

`I:\tools\chat-pool\log.jsonl` copied to `I:\Temp\poolscratch`, then the C host
appended 12 turns into the copy (`append` mode, one producer claim). Source dir
was only read: its sha256 was identical before and after
(`11eafcb0…04bfa`, 32,978 lines).

```
baseline  32978 lines, 34,864,904 B, sha256 11eafcb0…04bfa
run       exit 0, 11/11 MATCH (1 skipped: anchor-turn frame), blobs 1/1 verified
after     32991 lines (+13 = 12 deltas + 1 anchor)
append    first record's prev == the baseline last hash (1caced73…aa78)
proof     per-line sha256 of all 32,978 baseline lines recomputed after the run:
          0 mismatches, 0 missing
check     lines 32991 (first prev '') … anchors 1 [1] blob 70656 B
          CHAIN OK — every line hashes to itself, prev links contiguous,
                      anchor regions unique and fnv-verified      exit 0
```

Two defects the scratch run exposed, both fixed:

- `kv_log_check.py` assumed the first line says `prev:"GENESIS"`; a real pool log
  starts with `prev:""`, so the checker failed a perfectly good 32,978-line log
  (it failed the *pristine* control copy too — the checker was wrong, not the
  log). Now only lines 2..N are required to point at their predecessor.
- The reader rejected every CRLF line (`line[ll-1] != '}'`): a log re-serialized
  on Windows read as 1 anchor + 1 delta instead of 158 records, silently. Fixed
  by trimming `\r` before the completeness test, with a CRLF fixture in
  `tests/kv_temporal_test.c`.

### A record from another branch is refused, not stitched

Doctored copy of the 158-turn log (`tools/mkbranch.py`): one delta re-parented
from 145 to 1, **with every hash recomputed** so the chain is perfectly valid.
Only the parent rule can see it:

```
python tools/kv_log_check.py check I:\Temp\kv11
  lines 160 … CHAIN-level checks all pass
  FAIL 1: delta t150 hangs off parent 1, nearest anchor is 145    exit 1

build\kv_temporal_llama.exe <model> I:\Temp\kv11 <backend> 1 append
  log: turn 150 hangs off parent 1, which is not its nearest anchor —
       refusing to resume from a branched log
  exit 1,  log unchanged at 160 lines
```

This is the honest form of the "merge conflict" test: with one append-only chain
there is no merge to get wrong, but there IS a way to splice a foreign branch into
a path, and replay would have produced a plausible wrong state instead of an
error.

The two defects that earlier race found (single-writer testing cannot see
either) stay fixed here:

- the tail scan treated the string `"GENESIS"` as "no tail found", so a run that
  started on an empty file never adopted a line another writer added later —
  chain forked at line 301 with `prev=GENESIS` mid-file. Now a `tail_found` flag
  records what the scan saw; the sentinel is no longer overloaded.
- `pool.py` writes `json.dumps` defaults (`"hash": "…"`, `\"turn\": 2`) while the
  C host writes compact JSON. Every scanner that matched `"hash":"` silently
  skipped pool-written lines. The reader now skips punctuation instead of
  matching it (`kvl_after_key`, `kvl_kind_is`), with a spaced-log fixture in
  `tests/kv_temporal_test.c` so it cannot come back.

Cross-language read of a C-written log (Python, `kv_pool_bridge`, no C in the
loop), 24-turn log:

```
deltas 24 anchors 1
anchor turns [1]
delta turns head [1, 2, 3] tail [22, 23, 24]
blob fnv verify [True]
seek 1 -> anchor@1 replay 0
seek 7 -> anchor@1 replay 6
seek 24 -> anchor@1 replay 23
chain ok True lines 25
```

Measured sizes (K=Q8_0, V=Q4_0, 0.5B): anchor base ≈ 5.0 KB/token (70,656 B at
14 tokens), a delta record carries only its turn's tokens — the log stays in the
KB range for dozens of turns while the bases live outside it.

## Rules the code now enforces

1. **Two locks, two scopes.**
   `log.jsonl.lock` is the **same** lock file `pool.append()` uses
   (`pool.py:193`), held only for the microsecond-critical section:
   read the tail hash → compute → append one line, and separately
   reserve → write one blob. Without it, two writers read the same `prev` and
   both claim it, and two writers compute the same blob offset.
   `kv_producer.lock` is claimed **non-blocking for the whole run**: a second
   live producer is refused (`exit 3`) instead of interleaving two turn
   sequences into one log. Sequential appends stay the supported pattern.
2. **Anchor cadence is the round rule** — `((turn-1) % KVT_ROUND)==0`, checked by
   `kvt_anchor_due()`; never "every K frames".
3. **Replay reproduces batching**, not just tokens (per-turn decode).
4. **The anchor-turn frame is not comparable** — reported, never scored.
5. **Bytes are addressed, never inlined** — the log carries `{off,len,fnv}`.
6. **Every read verifies** — blob FNV is checked before resume, so a stale or
   truncated side file fails loudly instead of resuming wrong KV.
7. **The log is the source of truth** — recall and append-mode resume both go
   through `core/kv_temporal_read.h`; the producer's arrays are not consulted.
8. **Kill-safety** — a partial last line (no closing `}`) is skipped, not parsed
   into a shortened token array; foreign pool kinds are ignored; `blob_fnv`
   above `INT64_MAX` survives (unsigned parse).
9. **Records carry event time** — lines now include `"t"`, because pool's indexer
   defaults a missing `t` to 0 and an undated record then looks ancient (#7795).
   A log can therefore mix records written before this change (no `t`) with
   later ones (with `t`); nothing reads `t` for replay, so mixed logs are valid.
10. **KV kinds stay out of the embedder** — already in `INDEX_SKIP_KINDS`
    (`pool.py:272`), verified, so wiring into the live pool adds no embed cost.
11. **The log must be ONE path** — `kvl_check_parents()`: every delta's `parent`
    must be the nearest anchor at or before its turn. A record written under
    another branch is refused (exit 1) instead of replayed on the wrong base.
    Verified with a doctored log whose hashes were all recomputed: the chain is
    valid and only this rule sees the foreign record.
12. **No turn twice** — `kvl_check_unique()`: two branches that number the same
    turn both produce chain-valid lines, so replay would silently pick one. Also
    enforced in `tools/kv_log_check.py`.
13. **Both JSON shapes and both line endings parse** — `pool.py` writes spaced
    `json.dumps` output while the host writes compact JSON, and any re-serialize
    on Windows writes CRLF; the reader skips punctuation and trims `\r` instead
    of matching a fixed byte pattern (both were measured to silently drop whole
    logs).

## Boundaries (not done, stated plainly)

- **One producer per log, by design.** Concurrent producers are refused, not
  merged: two runs would both write `t1..tN`. Any number of *pool* writers is
  fine (the per-line lock covers them), and the C host itself is safe next to
  them — proven above, not assumed. If a second producer is ever needed, the
  turn numbering has to be allocated first; that is a format change, not a lock.
- **The break between runs is not crash-atomic.** A run that dies between the
  blob write and the record write leaves orphan blob bytes (wasted space, never
  a wrong address); a run that dies mid-line leaves a partial line the reader
  skips. Neither corrupts a chain.
- **KV records carry no `rkey`/`enc`, and that is a visible edge in pool today.**
  Measured on the scratch copy: `derived()["cells"]` skips them (guarded by
  `if rk is not None`) so the plane map is unpolluted, but `timeline()` returns
  13 rows with address `None`, and feeding that `None` to the plane helpers
  raises `TypeError` in `_block_of` / `_coarse_map` (`pool.py`). Not reachable
  through `pool_server.py` today (it only calls `explore_cell`, which tolerates
  `None`), so it is a latent trap, not a live bug. Two ways out, not both:
  give KV records a real address (port `rkey`/`app_phase` to C bit-exactly), or
  make the pool helpers refuse `None` explicitly. Filed for the pool
  workstream rather than patched here, because `pool.py` is not this chain.
- **Reader slurps the file** (guarded at 512 MB). A production log needs a
  streaming reader; today's logs are small.
- **Demo-sized ctx** (`NCAP 3800`, `KVT_WINDOW 8` frames). The hot window policy
  covers *frames*; a long conversation still needs a real ctx-window policy.
- **Long runs are sampled.** A rebuild from an anchor costs O(turns-since-anchor),
  so a full sweep of a 145-turn log is quadratic; verification compares the
  first 8, the last 8 and every 16th turn — each of them a complete rebuild.
- **Not wired to production paths.** Nothing writes to `I:\tools\chat-pool`
  yet, and nothing here touches ANN: per #7587 KV *feeds a result to* ANN, and
  the two must not cross-include.

## Next

1. Point a run at a scratch copy of the pool dir (concurrency is covered now;
   what is left there is the scratch-vs-live decision and the `rkey`/`enc` gap:
   KV records carry no address key, so they stay out of the pool's plane index —
   correct while they are machine records the embedder skips, but it must be
   stated before they live in the live log).
2. Streaming reader (`kv_temporal_read.h` keeps the same API).
3. KV→ANN handoff via the index section — separate chain, separate card.
