# HJ–Jet Doctrine (locked 2026-09-26, owner session)

Perspective-relative addressing + rendezvous rules for the 4D→3D serve path.
Status: doctrine (must-hold invariants). Violations collapse at machine speed;
pin each with a test, never with supervision.

## 0. Layer order (never mix)

```
address (stride/coords, GJ/HJ) → residency (BFS + spill) → integrity (planet)
```

- Planet is layer 3 (watcher), NOT layer 2. Excluded from residency accounting.
- Residency counts blocks/bytes only.

## 1. HJ query protocol (one way)

1. **GJ similarity-entry** — outside query need not be exact; approximate
   nearest is enough for HJ to appear at the spot.
2. **HJ deterministic refine** — from the appearance point, walk exact to
   the target (`R` → `R[3,1]`).
3. **One-way return** — bytes flow back to the requester only. HJ never
   initiates a query (stars point; they don't travel — the seeker moves).

Rationale (owner): there is nothing to ask back — a find-command is one-way
by nature. `R,3,1` held by GJ *is* the answer in compact form; heavy weights
stay inside unless reconstruction is demanded.

## 2. Discrete decides, continuous fetches

- **Discrete codes decide without fetch** — `3` = 3rd cat, `1` = white.
  Lossless, sufficient, done. No fetch needed.
- **Continuous values must fault before trust** — a code over continuous
  space is a quantized claim, not the value. Fault the bytes, then believe.
- Owner note: the continuous tail is jet bridge's job (see §3).

## 3. Jet rendezvous (ambulance, not highway)

- **Discrete leads, continuous chases.** `R,3,1` travels to compute at once;
  unmatched continuous detail (tiger stripes) is sent after and assembled
  **en route**, before compute touches that field (phase-align arrival).
- A request never stops to wait for precision; precision chases the request.
- Jet bridge exists to **fix** (late-but-valid data), NOT to speed up.

## 4. Park rule (rail discipline)

- **Valid → may park.** Awaited payload verified to exist and be inbound:
  parking at the next stop is worth it.
- **Not found → never stop.** No idling on speculation; keep moving.
- `fill = 1 (no data)` → proceed with zeros is this rule, not a bug.
- Waiting for the mod-12 sync is waiting for the *schedule* (valid by
  construction), not for data.

## 5. Freeride detection (health metric)

- Any query served **>50% of its bytes via jet** = freeriding (primary data
  must arrive direct, not via ambulance) → flag the query.
- System-wide **>5%** of queries touching jet = upstream addressing is
  broken → stop and fix addressing. Never widen the spur (dilutes density).
- Counters live on the fill/fault path (card #46 follow-up).

## 6. HJ batch law (MEASURED 2026-09-26 — hypothesis falsified)

Prediction was a knee: small slices drown in per-trip fixed cost, big slices
amortize it. Scratch bench (`knee.c`, mem spill, planets off, stream+verify
400 slices) says otherwise — the line is FLAT:

```
blocks/slice | 1 | 8 | 72 | 144  →  0.88 / 0.83 / 0.86 / 0.87 MB/s
```

Cost is purely per-byte (~0.9 MB/s: v6b encode + decode + digest per 144 B
block, paid twice). There is NO per-trip term in the codec path. The tiny-
slice slowness seen in the real proof (97k files, ~0.4 MB/s) was per-FILE
*disk* overhead — a harness artifact, gone at 20736 B slices (full model
with disk spill: ~0.85 MB/s ≈ mem-spill rate). Consequence: per-tensor spill
batching is YAGNI — disk is already amortized at field-size slices. Do NOT
revisit without new numbers.

## Provenance

- #46 BFS eviction + residency bound (LRU + spill/fill + cap) = the
  materialize-on-touch mechanism for §1–§2. Real proof: full Qwen2.5-0.5B,
  291 tensors / 32,505 slices / 669,763,072 B, 0 mismatch, peak 144 ≤ cap.
- Bug found by real proof: evict-retry bound reread shrinking `n_files`
  (fixed via snapshot + T7 regression in `tests/test_bfs_evict.c`).
- v6b CRC bit-by-bit → table-driven (values verified identical).
