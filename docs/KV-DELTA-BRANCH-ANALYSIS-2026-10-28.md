# Delta Branch · Speculative Block · Minor Root · Adaptive Quota — analysis against what exists

Date: 2026-10-28. Status: analysis only, nothing implemented. Every claim below
names the file/symbol it rests on, or says plainly that it is not yet proven.

Input: three proposals brought as raw material (Delta Branch + Speculative Block
+ Lazy Prune; Minor Root + reserved 1-3% for sideways subagents; Adaptive Quota
from previous usage), each with a "prove 3 things before implement" list.

---

## 0. The one question that decides all three

**What is a "block"?**

| if a block is… | speculative reservation costs | waste test | prune is |
|---|---|---|---|
| bytes in a file (like `kv_blobs.bin`) | real disk; a wrong guess wastes it | meaningful, >10% is a real loss | deleting bytes a reader may hold → needs RCU |
| a coordinate in the 20736 field | **nothing** — address space is not storage (#6192 MAP not COMPRESS) | meaningless (reserving an address spends no byte) | releasing a claim, never touching payload |

The proposal is written as if blocks were bytes. This project's field is
coordinates; only `kv_blobs.bin` is bytes. Until that is decided, "speculative
waste" and "prune safety" cannot be measured at all. My reading of the existing
code supports the second column, and that is why the 1-3% reserve idea is cheap
in the first place.

---

## 1. Delta Branch + Speculative Block + Lazy Prune

### 1.1 There is no merge step, so "merge conflict" is not the failure mode

The pool log is append-only and hash-chained, and exactly one writer holds
`<LOG>.lock` at a time (`pool.py:193`, same lock the C host now takes,
commit `4d2acbf`). Order is **arrival order created by the lock**, not a
timestamp and not a merge.

So a "delta branch" is not a divergent history that later needs a merge: it is
lines from another writer that the chain closes in the order they arrive. There
is nothing to reconcile, and no conflict resolution algorithm to get wrong.

What *can* go wrong, and is worth testing, is different:

| failure mode | already proven? | test |
|---|---|---|
| two writers claim one `prev` (fork) | fixed and receipted, `4d2acbf` | race: 811 lines, chain contiguous |
| two writers compute one blob offset | fixed, same commit | same run, regions unique |
| a delta arrives whose parent turn is not in the log yet | **not proven** | write a delta with `parent=99`, read it → must fail loud, not replay from the wrong anchor |
| two branches both number their next turn `t7` | **not proven** | merge two branches, one log: duplicate turns must be refused at write, not silently kept |

The user's test 1 ("simulate 2 writers, merge must be deterministic, no data
lost") is therefore half already done and half mis-aimed. The determinism claim
is trivially true here: the lock *is* the total order. The interesting half is
the two unproven rows above, which the same harness can drive.

### 1.2 RCU is the wrong tool, because nothing is deleted

The system's deletion primitive is a **totem**: the record that a path was
walked, kept and replayed, never erased (#7502, naming #7799). Paths are opened
and closed, not freed (`#7166` append-only sub-graph). A reader therefore never
holds a byte that a pruner wants to take away; the only things that can be
released are:

1. an unused *reservation* (nothing has been written into it yet), and
2. a residency decision (hot → cold), which moves nothing and changes no address
   (`#7162`: light index in RAM, cold bytes in storage).

Neither case can produce the read-during-free crash that RCU exists to prevent.
What can still go wrong is a **stale reference**: the main side rotates or
re-purposes a block while a subagent's minor-root entry still points at it. The
correct primitive is a **generation counter**, not RCU: the read must compare
generations and fail loud.

So test 2 should be re-specified from "reader + pruner → must not crash" to:

> hold a minor-root reference to block B, rotate B's generation, then read
> through the reference → must return a mismatch error naming both generations,
> and must never return bytes.

and a second, sharper one:

> reader is mid-read of B while the lease on B expires → the read must either
> complete with the bytes it already validated (fnv) or fail; it must never
> return a mixture of two tenants' bytes.

### 1.3 "Eager prune" is already the default, and that is the real risk

Lazy prune is proposed as an improvement over eager prune. But nothing here
prunes eagerly today: records are appended and never removed; the only time-based
action in the whole path is the *anchor cadence* (`((turn-1) % KVT_ROUND)==0`,
`core/kv_temporal.h`) and lease expiry would be the first real "delete" the
system has ever had. That is an argument for doing it once, carefully, as a
**lease close** (path closed, bytes stay), rather than introducing a pruner that
reclaims bytes.

### 1.4 Speculative waste, restated

If the reservation is a coordinate, waste is zero and the 10% gate measures
nothing. If it is bytes, the honest metrics are:

- `reserved_bytes_unused / reserved_bytes` at expiry — the user's 10% gate;
- **lease age** distribution — how long a reservation sat unused before expiry.

Proposal: gate on **lease age** (a reservation that has not been written within
N rounds is reclaimed regardless of percentage), and report the percentage as a
diagnostic. Percentage-only gates let one huge stale reservation pass a
per-count test, and let 50 tiny short-lived ones fail it.

---

## 2. Minor Root + reserved 1-3% for sideways subagents

### 2.1 It maps onto an existing rule, and the rule constrains it

Minor routes are **outbound-only walks with no retreat**; expansion happens only
through a passed layer, and the frustum is closed and non-growing (`#7564`).
A sideways subagent is therefore not a new geometry: it is a minor-root entry
plus a region it may write, and **the way back is a receipt, not a pointer**
(totem/roster, `#7799`).

That single constraint kills the "pointer to the parent block" design and
replaces it with: parent writes the entry (who, which region, which generation),
subagent writes its own deltas, parent reads back through the roster.

### 2.2 The 1-3% reserve has a granularity problem: one tesseract is 5.6% of the field

Measured against the code, not the drawing:

- `20736 = 18 tesseracts × 8 cubes × 144 slots` — `core/moe_expert_addr.h:16`.
- one tesseract = `8 × 144 = 1152` slots — `core/iso_fold.h:6`.
- one tesseract folds onto **9 anchors**; `18 × 18 = 324`… the fold in
  `core/iso_fold.h:8` gives `162 anchors` because `18 × 9 = 162`, and
  `162 × 128 = 20736`, so **one anchor = 128 slots**.

So the candidate reserve, in the field's own units:

| request | slots | anchors (128 slots) | tesseracts (1152) |
|---|---|---|---|
| 1% | 207.36 | 1.6 | 0.18 |
| 2% | 414.72 | 3.2 | 0.36 |
| 3% | 622.08 | 4.9 | 0.54 |

Two consequences, both checkable before building:

1. **The reserve is anchor-granular, not tesseract-granular.** A whole tesseract
   is 5.6% of the field — above the 3% ceiling on its own. So "reserve a
   tesseract per sideways subagent" is out; "reserve 2-5 anchors" is in.
2. The 1% floor and the anchor granule are the same order of magnitude, so the
   percentage has to be specified as **a count of anchors** (e.g. 2-5), never as
   a percentage that then gets rounded up into something much larger.

The 1-index+7-data tesseract rule (#7530) is **doctrine, not code**: no
`CUBES_PER_TES`-style constant exists in `core/`, so any argument that leans on
"the index band is free" must first locate the rule in code. The safest version
of the reserve does not need it: reserving addresses is free (#6192), so a
2-5-anchor lease costs nothing until a subagent writes into it.


### 2.3 The three tests, made runnable

| user's test | what it becomes | acceptance (numbers marked † come from the user, not measured) |
|---|---|---|
| orphan: subagent dies → block returns to free list within X s | lease with heartbeat; expiry = close path + bump generation; bytes stay | reclaim within X = 5 s (2 missed beats) †; zero silent reuse before reclaim |
| dangling pointer: main prunes subagent block → minor root must fail loud | generation counter on every entry; read compares | mismatch error naming both generations; negative control (matching generation) must pass |
| under-provisioning: >3% used often → dynamic scale | measure borrow rate per round over a scripted 3-load profile | escalate when borrows exceed 20% of rounds †, after ≥3 rounds of evidence, never mid-round |

### 2.4 What the proposal gets right that the existing code does not have

The reservation is **made in advance and pointed at**, which is a real addition:
today a subagent that starts work has no address until it writes. Pre-reserving
gives it a stable identity to hand back. That is worth building — as a lease on
a coordinate, with the cache block it points at being an ordinary cold/hot
decision (`#7162`), not a new storage tier.

---

## 3. Adaptive quota from previous usage

O(1) and <1 ms are not the hard parts: a quota computed from fixed-size counters
with an EWMA is a handful of arithmetic ops. The hazards are:

1. **Oscillation** — quota recomputed each round from the usage it caused can
   flap. Needs hysteresis (adjust at most one step per round, and only after the
   deviation persists ≥2 rounds).
2. **Starvation** — a heavy subagent borrowing every round starves main unless
   the borrow is capped and main's floor is absolute.
3. **Coupling** — the history used to compute the quota is itself shaped by the
   quota. Any test must therefore drive a *fixed* external load profile
   (light/medium/heavy scripts), not replay recorded usage.

The user's test list (converge in 2-3 rounds †, burst → emergency borrow without
breaking main, O(1) < 1 ms †) is the right shape; add the two measurements that
would actually fail first: steps-to-converge **per level change**, and
borrow-events-per-round during a burst.

---

## 4. Honest diff: what these lists add, what they miss, what is already done

**Already proven before these proposals** (commit `4d2acbf`): two writers on one
log produce one chain with no lost line (811 lines, `CHAIN OK`), one producer per
log enforced (`exit 3`), offset collision impossible under the shared lock.

**Worth building, in this order** (each cheap because the harness exists —
`tools/kv_log_check.py`, `tests/kv_temporal_test.c`, `tools/kv_temporal_llama.c`):

1. **Orphan delta / parent-not-yet-present** → reader must fail loud. No new
   storage, one negative test.
2. **Duplicate turn numbers across two branches** → refuse at write. Today two
   producers are refused wholesale; that is stricter but coarser, and the
   coarseness is what a "branch" proposal would want to relax.
3. **Generation counter + stale reference** → the only genuinely new mechanism in
   all three proposals; needed by both the prune story and the minor-root story.
4. **Lease + expiry + heartbeat** → first deletion-like action in the system;
   implement as close, never as free.
5. **Waste/age metric** → only after deciding coordinate-vs-bytes.
6. **Adaptive quota** → last, because it depends on 4's numbers.

**Not worth building**: RCU (nothing is deleted), git-style merge (no merge
step), dynamic frame growth (violates the closed frame, `#6337`), and any
percentage-only waste gate.

**Missing from all three proposals**: what a *reader* sees while a branch is
mid-append, and what happens to a `parent` that never arrives. Those two are
cheaper to test than anything on the lists and are the ones that would actually
corrupt a recall.

---

## 5. Numbers: user-supplied vs measured

Marked † above and repeated here, because they are candidates to try, not
settled parameters (#7129):

- 1-3% reserve — user's;
- 10% speculative waste gate — user's;
- 20% under-provisioning escalation — user's;
- "2-3 rounds to converge" and "<1 ms quota" — user's;
- "X seconds" reclaim — user left it open; I propose 5 s / 2 missed beats.

Measured in this repo so far: 811-line mixed-writer chain (0 losses), 158-turn
KV log with 2 anchors, 330 parser assertions, 13 KV-tier tests. Nothing about
leases, generations or quotas is measured yet.
