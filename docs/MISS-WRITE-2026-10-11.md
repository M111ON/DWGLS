# MISS-WRITE DISK COST 2026-10-11 (parked #3, counted)

**Question:** what does a query cost in disk bytes — do misses cost more?

**Answer: misses cost the SAME as hits. ~154 B/query, unbounded.**

## Measured (scratch, frozen 210q, vec mode, prune protocol, fresh log/class)

| class | n | log bytes | B/query | scored med | modes |
|---|---|---|---|---|---|
| miss (nbr top-1 ≠ truth) | 183 | 28,227 | **154.2** | 8000 | anchor+p |
| hit | 27 | 4,163 | **154.2** | 8000 | anchor+p |

Identical to the decimal. The miss penalty is recall, not disk.

## Where the bytes go

- Fog log (`lz_route.log`): **32 B/record × ~4.8 buckets/query**.
  Records EVERY visit, not just latch transitions (882 records over 7
  distinct slots in the miss run — repeats re-log). The latch state is
  monotone; the log is not. So the log grows ~154 B/query FOREVER:
  1M queries ≈ 154 MB, and boot replay pays it back linearly
  (live `replayed=5628` and climbing).
- Vec sweep: 8000 × 1536 B = **12.3 MB touched/query**, mmap-resident
  (faults only when cold; entries individually malloc'd so cold ≈ up to
  ~8000 scattered pages). Fallback/brute full sweep: 59756 × 1536 B =
  **91.8 MB**.
- fflush per query (no fsync) — syscall, no disk force.

## Corrections to prior readings

- "Steady state 0 B/query" (assumed from transition-only logging) is
  WRONG — measured 154 B/query on fresh AND would continue; the 7 shut
  latches don't stop the writes.
- Consistency: phase-1 hits=27/210 reproduces tonight's trained 27/210
  exactly (independent run, same frozen truth).

## Method

`miss_cost.py` (scratch): phase 1 classifies hit/miss on throwaway log,
phase 2 replays each class on a fresh log. Self-inflicted lesson: the
harness recopied a headerless order over the stamped one mid-run
(new-binary scratch silently brute-forced everything) — caught by
`modes={'brute': 210}` in the receipts, fixed by copying the stamped
live order. Watch the `mode` column, not just the numbers.
