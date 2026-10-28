# KV → ANN split: the two-layer lookup, the KV index supervisor, and what an expired lease really means

2026-10-28 · commits: this doc + `tools/kv_lookup.py`,
`tools/kv_index_supervisor.py`, `tools/kv_expiry_probe.py`

Three things were open after the split was measured (`a0cfe98`): who keeps the
second index alive, how a caller actually reaches it, and whether a reaped lease
means a frame is gone. All three are answered below with receipts.

## 1. Two-layer lookup — `tools/kv_lookup.py`

```
query → embed (e5-small, dim 384, the corpus's own embedder)
      → layer 1: memory index :8096  ── mapped sid in top-k? → plan
      → layer 2: KV index :8510      ── only if layer 1 had no mapped hit
      → plan gates: map-vs-log  +  anchor blob FNV/offset
```

The map is built from the frames sidecar, not from the ANN rows, and every plan
is gated before it is written: `--frames … --build-map …` wrote **32 plans,
0 refused**.

Measured on 32 real pool messages (their own vectors, so the embedder is not in
the loop):

| arm | layer 1 | layer 2 | miss | turn match |
|---|---|---|---|---|
| self **excluded** (the honest arm, `#7697`) | **0** | **31** | 1 | exact 0 · ±1 11 · farther 20 |
| self **included** (tautology arm, labelled) | 0 | 32 | 0 | **exact 32/32** |

Read it as two separate facts:

* **layer 1 = 0 is the split's justification, measured.** These 32 messages are
  chatdrop/pool rows; they are not in the memcore corpus, so the memory index
  cannot name a frame for them at all. Without a second index they are
  unreachable — which is exactly what the mixed arm showed earlier (KV recall
  0.0000 at 63,947 rows).
* **layer 2 = 31/32 proves reachability, not precision.** Only 32 of the 2,032
  KV rows are mapped to a frame, so the nearest mapped neighbour is usually a
  different turn (exact 0, ±1 11). Precision is a function of how many turns are
  framed, not of the lookup.
* the tautology arm is the plumbing check: with the query's own row kept, the
  answer is its own frame 32/32 at rank 1. Quoting it as a quality number would
  be the tautology `#7697` warns about.

Cost p50: memory 7.9–8.6 ms, KV index 2.8–4.0 ms.

## 2. Supervisor — `tools/kv_index_supervisor.py`

`ann_keepalive.py` is pinned to `:8096` and prefers
`build/ann_search_batch.exe`; it will never keep a second index alive. This
supervisor refuses to run on the memory port, requires the five sidecar files
before starting, treats `entries > 0` as the health bar (`--require-routed` when
a fresh index should already be routed), and appends every start/stop to
`supervisor.log`.

Verified by killing the KV server (pid 12716) and running `--once`:

```
unhealthy: down
start pid 15748 port 8510
recovered ok entries 2032 routed 1
```

and the memory index was untouched across the whole test
(`:8096 entries 63915 routed 1` before and after).

## 3. What an expired lease means — `tools/kv_expiry_probe.py`

The lease owns **space**; the log owns **bytes**. The probe reserves a block
(owner 42, 128 slots), writes 64, lets the 100 ms lease age out, reaps, then
checks the frame:

```
reap     reaped 1 (lease 100 ms, oldest live before reap 429 ms)
status   [ 0] owner 42  CLOSED   slots 128   written 64    gen 2
plan after reap: PASS ok
stale write rc=1 refused rc=-2 (handle gen 1, block gen 2)
resume   seek t27 ok: pos=2382 argmax=98282 (exit 0)
```

**Verdict: a reaped lease leaves the frame resumable.** Reap closes the block
and returns the slots (128 → 0 held); it does not delete a byte. So an expired
lease must never be used as "this frame is gone" — the only thing that
invalidates a plan is the byte-level gate (anchor blob FNV/offset, replay turns
present in the log), which is what `kv_ann_open.py` and this probe both check.
What a stale handle gets is a loud refusal (`handle gen 1, block gen 2`).

## What is still not done

* nothing schedules `kv_index_supervisor.py` yet — it is a tool, not a task;
  until it is installed the second index is still a silent single point of
  failure.
* the lookup has no HTTP endpoint; it is a library plus a CLI. Wiring it into a
  server means deciding who owns the map reload (the map is a file, and a stale
  map is refused by `map-vs-log`, not silently served).
* precision at layer 2 is bounded by the number of framed turns (32 here). That
  is a producer question, not a lookup question.
