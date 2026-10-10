# ARENA REVISIT 2026-10-11 — S1–S5 standings after re-label + prune-live

**Protocol tonight (differs from 10-10 — read before comparing):**
frozen `experiments/relabel-2026-10-11/truth.json` (210q: 70 exchange +
70 chatlog + 70 dictionary_entry, seed 20261011), EXACT stored-vec queries
(no sidecar gap), prune-enabled exe (`anchor+p` all runs), restart per
table. Metric: overlap = mean |walk top-5 ∩ brute top-5| / 5 (×5 below
to match the arena's /5 scale).nbr top-1 and self-hit secondary.
10-10 used TEXT queries via sidecar + unknown truth def — same tier,
not same ruler. Do not subtract across protocols.

## Recall column (pruned @budget8000 unless noted)

| side | S5 trained | S3 trap (V0n-relabeled) | Jpc (relabeled, ref) | S4 derived |
|---|---|---|---|---|
| 10-10 | 2.70 | UNMEASURED | 0.65 (no relabel) | 2.17* |
| tonight | **2.23** (nbr 27/210, self 100) | **1.04** (nbr 21/210, self 48) | **1.95** (nbr 38/210, self 99) | not rerun* |

*S4 params (dc/dx/seedmode of the heat's S1n) were lost with temp —
rerunning with guessed params would be worse than none. 2.17 kept as-is.

- S3 goes UNMEASURED → 1.04. The trap direction now has a number; it
  trails (bucket 4: 33,922 members, budget truncates mid-bucket).
- Re-label lift visible inside tonight's frame via the heat baseline:
  Jpc 0.65 → 1.95, V0n 0.39 → 1.04 (see RELABEL-2026-10-11).
- Trained reproduces the heat (2.23 vs 2.70→2.28 big-N) — same table,
  same labels. This is also the evidence the heat's scratch exe HAD
  prune live (stale exe gives 0.63 here, not 2.28).

## Full-scan tier (budget off, prune-gather on, S5)

overlap **4.00/5**, self 209/210, nbr top-1 **1/210**.
The cap is a locality prior, not just cost control: capped walk stays
inside the query's home bucket (neighbors live there); full scan admits
farther-bucket high-scorers and top-1 agreement collapses while overlap
rises. (Part of the top-1 fragility is float32/float64 tie-shuffle among
0.99s — truth was float64, server float32. Overlap is the stable column;
top-1 is not. The arena's choice of overlap/5 as primary was right.)

## Build / steady / determ (re-measured this box, same binary source)

active64 40500 ns | active16 19000 ns | trap-register 25 ns |
trap-first 15000 ns | trap-verify 525 ns | table8x384 167500 ns.
sizeofs: trap 160 B, anchor 128 B, table 12 KB. Determinism 1/1/1.
Same ballpark as 10-10 — no drift.

## Verdict for the first-class question

Tables are NOT first-class yet: best derived (Jpc 1.95) ties trained on
top-1 (38 vs 27 = 1.5σ, a tie) but trails on overlap (1.95 vs 2.23);
S3/V0n trails both (1.04). Re-label closed the lottery gap, not the
partition gap. S4 unrestored — its 2.17 stands unchallenged until its
params are found or rebuilt.
