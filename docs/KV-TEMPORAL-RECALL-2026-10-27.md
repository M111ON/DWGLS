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

1. **Anchor cadence is the round rule** — `((turn-1) % KVT_ROUND)==0`, checked by
   `kvt_anchor_due()`; never "every K frames".
2. **Replay reproduces batching**, not just tokens (per-turn decode).
3. **The anchor-turn frame is not comparable** — reported, never scored.
4. **Bytes are addressed, never inlined** — the log carries `{off,len,fnv}`.
5. **Every read verifies** — blob FNV is checked before resume, so a stale or
   truncated side file fails loudly instead of resuming wrong KV.
6. **The log is the source of truth** — recall and append-mode resume both go
   through `core/kv_temporal_read.h`; the producer's arrays are not consulted.
7. **Kill-safety** — a partial last line (no closing `}`) is skipped, not parsed
   into a shortened token array; foreign pool kinds are ignored; `blob_fnv`
   above `INT64_MAX` survives (unsigned parse).
8. **Records carry event time** — lines now include `"t"`, because pool's indexer
   defaults a missing `t` to 0 and an undated record then looks ancient (#7795).
   A log can therefore mix records written before this change (no `t`) with
   later ones (with `t`); nothing reads `t` for replay, so mixed logs are valid.
9. **KV kinds stay out of the embedder** — already in `INDEX_SKIP_KINDS`
   (`pool.py:272`), verified, so wiring into the live pool adds no embed cost.

## Boundaries (not done, stated plainly)

- **Single writer assumed.** Two processes appending to one log can interleave
  the tail-read and the append. The chain follows the file per write, but there
  is no lock. Before the pool dir is used, a writer lock (or one writer) is
  required.
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

1. Point a run at a scratch copy of the pool dir, with the writer question
   settled first (lock or single writer).
2. Streaming reader (`kv_temporal_read.h` keeps the same API).
3. KV→ANN handoff via the index section — separate chain, separate card.
