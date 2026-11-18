# HANDOFF — ANN route budget + LC closed (2026-11-15)

Continued from `HANDOFF-2026-11-14-nomic-migration.md`. Two workstreams this
session: **(A) what actually limits the ANN route** on the live `:8096` stack,
and **(B) closing the LC_tilecube question** on the current nomic 768-d data.

Everything here is measurement + receipts. **The live service was never changed**
(still `LZ_ROUTE_BUCKETS=15`, floor 0.5, 54,373 x 768, K=56).

---

## 1. Commits

| repo | branch | commit | what |
|---|---|---|---|
| `I:\tools` | main | `93b1b4e` `bfd53e7` `ae47c44` `e8f9ce7` `a73c302` | LC repro, gate constant, growth stability, share sweep + coverage correction, frontier |
| `I:\tools` | main | `fd70a63` `8bfd703` `ca84880` | icosphere vs SizeRand (PASS), icosphere vs anchor (loses), LC_PACKAGE suite |
| `I:\tools` | main | `14cdec9` `dbe37e9` `39886ad` | text topb sweep → **overturned** → budget sweep |
| `I:\tools` | main | `4beac35` | remaining measurement scripts + the e5 archive move |
| `I:\DWGLS-native-fs` | feat/geo-native-fs | (this commit) | this handoff + the Z2-a probe header fix |

---

## 2. ANN — what actually limits the route

### 2.0 The live config is not a fixed value — check it before believing any baseline
`ann_keepalive.py` derives the whole env from **`USE_COS = os.path.exists(build/ann_search_cos.exe)`**:

| knob | USE_COS true | USE_COS false |
|---|---|---|
| `LZ_ROUTE_BUCKETS` | 64 | 15 |
| `LZ_SEARCH_BUDGET` | 41,000 | 24,000 |
| `ANN_WALK_FLOOR` | **0.0** | 0.5 |
| `ANN_ROUTE_ORDER` | cos | (empty) |

`ann_search_cos.exe` was built 2026-11-16 and the service was **restarted onto it
2026-11-18 13:55**, so the live route is now that column: measured on the running
process, `items_scored = 41,000`, `fallback = 0`, 56 buckets, `mode anchor+p` for
text, vector and junk queries alike.

**Every "live" column below that quotes floor 0.5 / 15 buckets / 24,000 rows
describes the PREVIOUS process (11-14/15), not the service as it runs now.** The
current live arm corresponds to the measured `budget 41,000 + floor 0` row in
§2.3 — text 0.9765, self 0.9820, both at 75.41% scan. Judge any proposal against
that row, not against the floor-0.5 one.

### 2.1 Fidelity
`experiments/memcore_fidelity_v2_2026-11-14.py`, 200 corpus vectors posted as
`{"vec":...}` (no embedder in the loop), self excluded, vs exact cosine oracle:
overlap@10 **0.9295**, top-1 ∈ oracle top-10 **0.99**, scan **21.9%**, p50 11.5 ms.
Misses are score-neutral (gap p50 0.0, p90 0.009) but spread evenly over ranks
1..10, not confined to the tail.

### 2.2 The binding constraint is the BUDGET, not the bucket count
Confirmed on the scratch harness with the floor sweep's own 119-query text set:

| config | text ovl | text tol | scan | lat | fallback |
|---|---|---|---|---|---|
| floor0.5-b15 (**live**) | 0.9924 | 0.9933 | 1.00 | 89.3 ms | 96 |
| floor0.30-b15 | 0.8966 | 0.9176 | 0.4414 | 54.1 ms | 4 |
| b30-floor0 | 0.8429 | 0.8706 | 0.4414 | 54.4 ms | 0 |
| b40-floor0 | 0.8429 | 0.8706 | 0.4414 | 54.6 ms | 0 |
| b40-floor0.5 | 0.9933 | 0.9941 | 1.00 | 89.6 ms | 101 |

**b30 and b40 are identical in every column** — member rows exceed
`LZ_SEARCH_BUDGET` (`min(N, max(24000, 0.40·N))` = 24,000 = 44.14% of N), so the
walk stops there. Any `LZ_ROUTE_BUCKETS` above ~20 is inert.

### 2.3 The budget/scan/recall curve (buckets=56, floor=0)
`experiments/text_budget_sweep_2026-11-15.py`

| budget | scan | self ovl/tol | text ovl/tol | control ovl/tol | text lat |
|---|---|---|---|---|---|
| 24,000 | 44.14% | 0.9710 / 0.9905 | 0.8429 / 0.8706 | 0.798 / 0.830 | 59.9 ms |
| 32,000 | 58.85% | 0.9765 / 0.9945 | 0.9210 / 0.9420 | 0.886 / 0.914 | 58.0 ms |
| 41,000 | 75.41% | 0.9820 / 0.9985 | 0.9765 / 0.9874 | 0.967 / 0.981 | 62.4 ms |
| **48,000** | **88.28%** | **0.9830 / 0.9995** | **0.9899 / 0.9941** | 0.989 / 0.993 | **70.7 ms** |

**budget 48,000 + floor 0 beats the live config on all three axes at once**:
tolerant text recall 0.9941 vs 0.9933, scan 88.28% vs 100%, latency 70.7 vs
89.3 ms (strict overlap 0.0025 lower). At `buckets=56` the routed set is the whole
corpus, so the floor has nothing to fall back TO — `floor=0` is free there and the
configuration collapses to **one knob (budget)** instead of two interacting ones.

Cost: a single global budget makes self/vector queries pay for the text budget
(32.6 ms at 48k vs 19.6 ms at 24k). **Progressive widening** — start small,
extend only when the best score is weak — is the design change that would keep
self fast; that is not a tuning value.

### 2.4 Text needs a near-full scan today
Real text queries peak at cosine **~0.43**, below the 0.5 floor, so the fallback
fires on ~85% of them — and that full scan is where the 0.993 comes from.
Without it, the 44% budget yields 0.8429. Of the true top-1 buckets, 60/60 sit
within rank 40 of 56 in a 60-query sample.

### 2.5 Progressive widening — measured, and only the two-tier form works
Two runs, same 420 queries and oracle.

**Staged (6k→12k→24k→48k, stop when the best score ≥ τ): FAILS.** Self never
escalated at any τ up to 0.55 (0%), stopping at 6,000 rows (11.00% scan) where
overlap is 0.8320 — because a self query's own neighbourhood is dense, so the
best score is already high while ranks 2..10 are still missing. Max score says
how good the BEST hit is, not whether the top-10 is complete.

**Two-tier (walk 24,000; extend to 48,000 only if the best score < τ): WORKS.**
At τ=0.60: self is untouched (0% extended, 0.9745 at 44.02%) while text reaches
0.9967 at 85.1% scan, versus 0.9983 at 88.04% for a blanket 48,000. Against the
arm the service runs now (41,000 / floor 0: text 0.9765, self 0.9820, both at
75.41%), two-tier buys text +0.020 recall for +9.7pp scan while cutting self from
75.41% to 44.02% scan for −0.0075. The score is being used as a
self/text type signal (self ≈ 0.9, real text ≈ 0.43), not as a completeness test.

Caveats: this is a walk model, not the server. It tracks the server's fixed
budgets within 0.004 (24k) and 0.010 (48k), but does not match the server at
small budgets, so `base=12,000` (self 0.9270 at 22.01%) is **not** trustworthy
and the base should stay 24,000. Implementing the escalation is a code change in
the walk loop of `src/ann_search.c`, not a config value. (`LZ_OVERLAP_TOPB`
defaults to **2** in source; live gets 21.9% scan from `ANN_TOPB` in the env, so
reading the source default instead of the live env will mislead.)

---

## 3. LC_tilecube — closed on this stack

Two different things share the name, and only one of them was ever tested here:

**The real LC** = icosphere addressing (162 cells) + `ReadPath` + RCU + rerank.
Its own claim — topology beats a **size-matched random** partition — **PASSES on
nomic 768-d, with wider margins than on e5-384**: pool recall 5/10/20% =
0.424/0.641/0.818 vs SizeRand 0.171/0.232/0.355, paired diff
+0.253/+0.409/+0.463, 88/97/97 wins of 100, p 1e-24..4e-37.
**But it LOSES to the trained production anchor route** at the same budget:
anchor − ico = +0.2615/+0.1675/+0.0760 (probe 5/10/20%), and the gap only closes
where the budget is so large that routing stops saving anything.

**The seed router** (`lc_hybrid_router.py`, 864 seed cells + IVF union) is the
direction LC's own docs closed on 2026-11-08. Measuring it again adds nothing:
LC 0.0747 / 0.1233 vs IVF 0.9027 / 0.8280; LC placement is 100% stable under
corpus growth while IVF churns 36.5% / 60.7%; the stability target >0.90 with
recall ≥0.88 **does not exist on the frontier** (max stability 0.8629 at recall
0.6333).

**LC identity/persistence** (LC_PACKAGE: geometry, 24/72/216 address, generation
manifest, commit journal, crash recovery) **passes 4/4 on this machine** — but
that is correctness of a self-contained subsystem, with no consumer and no
ANN-side benefit measured.

**Verdict:** LC is a correct subsystem with no job in the ANN on this stack.

---

## 4. Corrections — do not re-quote the retired claims

1. **"topb=40 wins on both axes"** (`14cdec9`) is **withdrawn**: that offline
   sweep scored the full member set of 40 buckets and never applied the budget.
   Corrected in `dbe37e9`.
2. **"oracle 40% vs 100% churn = 0.37 / 0.61"** measures **coverage**, not
   stability: the overlap equals the share of the true top-10 that lives inside
   the base 40%, measured exactly (wikivecs 0.6300 vs 0.6300; memcore 0.4000 vs
   0.3927). The `<0.05` gate is unreachable unless the base is ~100%.
3. **"X-ray threshold needs re-tuning"** — it needs a byte source first:
   `xray_scan_cell(cell_id)` hashes the row ordinal, so its ~17% prune rate is a
   property of splitmix64, not of the corpus (flat across arbitrary domains).
   Card `task-te45d85e0`.
4. **`branch_point_frame` was never "broken"** — the LC prototype's `seed_rng`
   used Python's process-salted `hash()`. Fixed with crc32 (`bfd53e7`).

---

## 5. Open decisions (ranked)

1. **Move the live route from `budget=41000` to the two-tier rule** (§2.5) — walk
   24,000, extend to 48,000 only when the best score is below 0.60. Against the
   arm the service actually runs now (41,000 / floor 0): self drops from 75.41% to
   44.02% scan for −0.0075 overlap (0.9820 → 0.9745), and text rises from 0.9765
   to 0.9967 for +9.7pp scan. A plain `budget=48000` is the simpler fallback but
   costs self 75.41% → 88.28% for +0.008.
2. **Implement two-tier escalation** — walk 24,000, extend to 48,000 only when the
   best score is below 0.60. Measured at §2.5: self untouched, text 0.9967 at
   85.1% scan. A code change in the walk loop of `src/ann_search.c`, not a config
   value. The staged (stop-when-confident) form does NOT work — see §2.5.
3. **LC**: park (identity/scope has no consumer) or delete the prototype tree?
4. **`data/_wikivecs` + `_wikivecs_scratch`** = 1.4 GB, keep or delete.
5. **`F:\e5-archive-2026-11-15\`** = 5.36 GB of 384-d e5 artifacts, parked.

---

## 6. Traps found (they cost real time)

- **`kv_anchors.bin` carries a 5-float header** (43,013 floats = 5 + 56·768).
  Reading from offset 0 yields one inf-norm "anchor" and a 16,170-row bucket;
  every anchor-arm number computed that way is void.
- **A subset measurement cannot predict the full index** — and the metric that
  looks like it tests stability actually measures coverage (§4.2).
- **Stability metrics saturate with candidate-set size**: a wider candidate set
  covers more of the base and raises the number by construction. Compare only at
  matched budgets.
- **`PYTHONHASHSEED`**: `hash((seed, salt))` differs per process; the LC
  prototype was non-deterministic until `bfd53e7`.
- **LC_PACKAGE tests need the root on `PYTHONPATH`** or they fail with
  `ModuleNotFoundError` (invocation, not product).
- **`start_server()` in `memcore_bucket_sweep.py` needs `prepare_scratch()`** or
  it dies on `SCR/server.out`.
- **Deploy path**: `ann_keepalive.py` starts `build/ann_search_batch.exe`, so
  deploying means copying the built binary over it and restarting (#7977).

---

## 7. How to start next session

```
git -C I:\tools log --oneline -3            # 4beac35 must be an ANCESTOR; the tip moves (cef92ec on 2026-11-18 added client.py)
git -C I:\DWGLS-native-fs log --oneline -2  # expect 7374699 (this handoff)
curl http://127.0.0.1:8096/health           # k must be 56; entries GROW (54,373 at measurement, 54,521 on 2026-11-18)
cd I:\tools\ann\experiments && I:\python3.14.4\python.exe text_budget_sweep_2026-11-15.py
```
Scratch port is 8098; the live service is 8096 and must not be disturbed.

**Re-measure after growth.** Every scan percentage in §2 is out of 54,373 rows, and the
route appends continuously (148 rows arrived in the 3 days after this was written), so
`scan` and the absolute row budget move with N: `budget = min(N, max(24000, 0.40·N))`
pins 24,000 until N passes 60,000, then starts scaling again.
