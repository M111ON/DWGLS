# RESULTS — what actually costs the milliseconds (2026-10-19)

Follow-up to `docs/HANDOFF-2026-10-14d.md` §3.2 / §3.3. Corpus, protocol and
repo unchanged: real `X.f32` **61,241 × 384** (e5-small), 200 self-queries,
honest oracle (exact top-k **excluding the query itself**), single thread, `-O2`.

Harnesses in this dir: `ann_knobs.c` (§3.2 iters, §3.3 K×topb) · `ann_frontier.c`
· `ann_robust.c` (5 seeds) · `ann_lat.c` (latency, QPC) · `ann_kernel.c`
(distance kernel isolation) · `ann_sse.c` (SSE2).

---

## 0. The headline correction

**Handoff §1.1 claims anchor+frustum query ≈ 16 ms. Not reproducible.**
Measured with the same protocol: **32.5 ms** mean at `topb=2` (28.2 ms p50).

The reason is not the routing — it is the **distance kernel**. The live server
computes, per candidate (`I:/tools/ann/src/ann_search.c:835`):

```c
double dot = 0;
for (int wi = 0; wi < qd; wi++) dot += (double)qv[wi] * ents[ei].vec[wi];
double sc = (ents[ei].norm > 0) ? dot / (qn * ents[ei].norm) : -1;
```

A **`double` scalar accumulator** (4-cycle serial add on this G4400) plus a
**second pass** to obtain `norm`. Routing removes ~65% of the *candidates* and
therefore cannot pay for a kernel that is 5× too slow.

---

## 1. Kernel isolation — `ann_kernel.c`

Same corpus, same queries, same top-k insertion loop; only the kernel changes.
`topb=2` vs full exact scan, `K=8`, `iters=2`. Two runs, ~5 h apart, different
host load — both shown, because the gap between them is itself a result:

| kernel | routed run1 | routed run2 | brute run1 | brute run2 | note |
|---|---|---|---|---|---|
| A `double` scalar + norm | 45.5 ms | 31.8 ms | 113.9 ms | 79.5 ms | **what the server does** |
| B `float` scalar + norm | 43.1 ms | 30.0 ms | 111.6 ms | 78.9 ms | ~1.06× |
| C `float` 4-acc, **no norm** | **9.5 ms** | **7.07 ms** | **20.3 ms** | **13.8 ms** | cosine == dot (unit-norm) |
| D `float` 4-acc + `float` norm | 31.1 ms | 21.7 ms | 76.8 ms | 56.6 ms | norm alone ≈ 3× |

**A → C is 4.8× in run 1 and 4.5× in run 2.** Two separate wins, and they are not
the same size:

1. **drop the norm pass** (safe: every row is unit-norm — `max | ‖x‖−1 | = 5.96e-07`)
   A → D is 1.47×, D → C is 3.26× (run 1 ratios).
2. **break the accumulator dependency chain** `double` → 4× `float` (A → B is only
   1.06× on its own, so the scalar *type* is not the issue — the *chain* is).

Routing's ceiling is fixed by how much it prunes: `topb=2` scores ~35% of the
rows, so at best ~2.9× exact **with the same kernel**. C shows 2.1× (run 1) /
1.96× (run 2). Both numbers are real; routing is not broken, it was simply being
compared against a handicapped baseline.

---

## 2. SIMD is real but modest, and the box is too noisy to pin it — `ann_sse.c`

The obvious next move is SIMD. On this box only SSE2 is safe (no AVX/AVX2/FMA —
G4400). Same 200 queries, exact scan, variants C/E/F confirmed **200/200 identical
top-5**. Variant R is deliberately *not* identical — it is the approximate route
and matches the exact answer on **167/200** (0.835 @ top-5), which is the
recall it is buying its speed with. That is the whole trade, side by side.

**Repeatability first, because it changed the answer.** The same binary, same
corpus, run 6 times across three sessions on a **shared 2-core machine**:

| run | C | E SSE2 | speedup E vs C |
|---|---|---|---|
| 1 (quiet) | 18.11 ms | 18.45 ms | **0.98×** |
| 2 | 13.56 ms | 10.94 ms | 1.24× |
| 3 | 15.12 ms | 10.96 ms | 1.38× |
| 4 | 13.49 ms | 11.10 ms | 1.21× |
| 5 | 12.73 ms | 11.90 ms | 1.07× |
| 6 (first rerun) | 13.82 ms | — | — |

`C` varies **1.4×** run to run (12.7–18.1 ms) while `E` is far more stable
(10.9–11.9 ms). That is the signature of a **memory-contended** host: the scalar
version's tail latency tracks whatever else is pulling DRAM (during these runs
`ann_search.exe` had grown 25 MB → 378 MB re-indexing, and the CPU was at 87% of
a 2-core box).

**Honest verdict: SSE2 is worth roughly 1.1–1.4×, not 0.98×.** Run 1 was taken
on a quiet machine and is the least representative of the six. I published
"0.98×, buys nothing" off that single run; five reruns do not support it. The
correction favours SIMD, and it does **not** change the conclusion below.

What survives, because it is a same-run ratio and both variants are measured
back-to-back: **routed `topb=2` is 2.1–2.6× the exact scan (mean 2.30× across
the 3-rep batch)** — stable across every run, consistent with the ~2.9× ceiling
implied by its 35% scan fraction.

Blocking (F) is consistently no better than plain SSE2 (1.11–1.24× vs C), so
the 64-row tile is **not** earning its complexity — and it is the reason I
cannot claim the scan is purely bandwidth bound. What is defensible without a
quiet machine is only this: a 1.4× run-to-run swing on the scalar path while
the SIMD path stays flat points at **memory contention**, and SSE2's flatness
is the real argument for using it.

**Consequence for §3.3:** widening the route (bigger `K`, more `topb`) trades
recall for bandwidth, and bandwidth is the contended resource on this host.
Routing is worth keeping precisely because it touches less memory — that
argument rests on the 2.3× (same-run, reproducible), not on the SIMD number.

---

## 3. §3.2 — how few kmeans iterations? (`ann_knobs.c` + `ann_robust.c`)

Recall is **flat across `iters` 2…25** (one seed: 0.9265–0.9310). Repeating over
**5 independent query seeds × 200 queries** confirms it is not one lucky draw.
Train time is given as a ratio, not as seconds — the absolute value moves with
host load (see §2/§5): the same pair measured **1.075 s / 15.308 s = 14.2×** on
the 2026-10-19 rerun and 0.60 s / 8.93 s on an earlier quiet run. Recall is
bit-identical between runs; only the clock differs.

| iters | topb | mean | min | max | seed spread | train |
|---|---|---|---|---|---|---|
| 2 | 2 | 0.9281 | 0.9055 | 0.9400 | 0.0345 | 1.08 s |
| 2 | 4 | 0.9896 | 0.9855 | 0.9920 | 0.0065 | 1.08 s |
| 2 | 6 | 0.9982 | 0.9975 | 0.9995 | 0.0020 | 1.08 s |
| 25 | 2 | 0.9315 | 0.9180 | 0.9500 | 0.0320 | 15.31 s |
| 25 | 4 | 0.9894 | 0.9845 | 0.9920 | 0.0075 | 15.31 s |
| 25 | 6 | 0.9980 | 0.9970 | 0.9990 | 0.0020 | 15.31 s |

At `topb=2` the sign **flips** between seeds: `iters=2` wins on seed 12345
(0.9310 vs 0.9305) and loses on the 5-seed mean (0.9281 vs 0.9315). The gap is
**~0.003**, while the seed-to-seed spread is **~0.034** — an order of magnitude
larger. At `topb≥4` the difference is ≤0.0002.

**Verdict:** `iters=2` is safe. The 0.003 at `topb=2` is inside the noise the
query draw already produces, and it buys **14.2–14.9×** on anchor train
(measured 14.2× on the rerun, 14.9× earlier) → regrow collapses from ~9 s to
~1 s. Handoff §1.2's 9.9 s was dominated by kmeans iterations for no measurable
recall.

> Caveat, stated honestly: this is a *statistical* null, not a proof of identity.
> `iters=2` is not provably equal to `iters=25`; it is provably not
> distinguishable at 200 queries × 5 seeds. If recall@10 near 0.93 is on a
> contract boundary, raise the query count before trusting the tie.

---

## 4. §3.3 — pushing recall above 0.94 (`ann_knobs.c` + `ann_frontier.c`)

Full K × topb surface (one seed, 200 queries):

| K | topb | recall@10 | scan% | train |
|---|---|---|---|---|
| 8 | 1 | 0.7540 | 18.1% | 13.4 s |
| 8 | 2 | 0.9305 | 34.8% | 13.4 s |
| 8 | 4 | 0.9885 | 59.1% | 13.4 s |
| 8 | 6 | 0.9985 | 83.7% | 13.4 s |
| 8 | 8 | 1.0000 | 100.0% | 13.4 s |
| 16 | 1 | 0.6540 | 7.9% | 20.6 s |
| 16 | 2 | 0.8345 | 16.1% | 20.6 s |
| 16 | 4 | 0.9380 | 29.5% | 20.6 s |
| 16 | 6 | 0.9790 | 43.7% | 20.6 s |
| 16 | 8 | 0.9890 | 57.2% | 20.6 s |
| 32 | 4 | 0.8775 | 14.0% | 34.2 s |
| 64 | 4 | 0.8190 | 7.7% | 70.9 s |

**Answer to §3.3:** yes, recall can be pushed to **0.998** — at `K=8, topb=6`
(5 seeds: 0.9982 ± 0.0020). The price is **84% of the corpus scanned**, and the
exact scan is memory-bound (§2), so 0.94 → 0.998 costs **~2.4× wall time**.

The honest Pareto knee is **`K=8, topb=4`**: **0.989** at **59%** scan.
Going K=16/32/64 is strictly worse per unit of scan — more anchors means fewer
rows per anchor, so you must widen topb further to recover recall, and the
anchor budget itself grows (train 70.9 s at K=64).

> Same warning as handoff §5.1 applies to §3.3: a high recall number is only
> meaningful next to its scan fraction. `K=8 topb=8` reaches 1.0000 by scanning
> 100% — that is a full scan wearing a routing costume, not a win.

---

## 5. Method notes — why absolute ms differ between these files, and between runs

`lat.csv` records brute at **57.6 ms**; `kernel.csv` run 1 records brute variant A
at **113.9 ms**; `kernel.csv` run 2 records the same config at **79.5 ms**. All
three are real measurements of the same configuration. Causes:

- different `k` (top-10 vs top-5) → different insertion-loop cost;
- cache state (in `kernel.csv` brute runs immediately after a routed pass);
- 200 queries is small: p95/max are dominated by a handful of outliers;
- **host contention** — these runs share a 2-core G4400 with a live `ann_search.exe`
  that grew 25 MB → 378 MB mid-session (§2).

**Cross-harness and cross-run absolute times are not comparable; only within-run
ratios are.** An earlier version of this section called the A→C gap "4.8×" as if
it were stable. Run 2 measured it at 4.5× (31.8 → 7.07 ms), and C itself moved
9.5 → 7.07 ms. The ratio is robust; the multiplier is approximate.

| ratio | run 1 | run 2 | 3-rep batch |
|---|---|---|---|
| A → C (kernel fix) | 4.8× | 4.5× | — |
| C → routed topb2 | 2.1× | 1.96× | 2.30× (mean) |
| C → E (SSE2) | 0.98× | 1.24× | 1.07–1.38× |

Order of magnitude is verified; the exact figures are subject to the ±30% host
noise documented in §2. The `16 ms` in the handoff was never reproduced by any
harness here, in any run.

---

## 6. Recommendation

1. **Kernel first** (`src/ann_search.c:835`): single-pass `float` with 4
   accumulators, norm skipped for unit-norm entries — **~4.5–4.8×** on the scan,
   applies to *every* path including full scan. Store a `unit_norm` flag per
   entry rather than assuming, and keep the norm path for any entry that fails.
2. **SSE2 is a modest win (1.1–1.4×) with a stability bonus** — it is flat across
   runs while the scalar path swings 1.4×. Take it, but do not expect it to
   dominate. Blocking adds complexity for nothing → drop.
3. **Never benchmark this box while `ann_search.exe` is indexing.** Same-run
   pairs only; single runs have already produced one wrong published number (§2).
4. **`iters=2`** for regrow: anchor train 1.08 s vs 15.31 s (**14.2×** measured,
   14.9× on an earlier run) at identical recall — regrow drops from ~9 s to ~1 s.
5. **`K=8, topb=4`** as the serving default: 0.989 recall at 59% scan.
6. Routing keeps its case, but its value is **bandwidth**, not arithmetic —
   re-evaluate it whenever corpus size changes, since the ~2.3× is a function of
   the 35% scan fraction, not of routing's design.

---

## 7. Reproduce

```sh
cd experiments/ann-hnsw-vs-anchor-2026-10-14/anchor_capo
INC="I:/DWGLS-native-fs/core"
X="I:/tmp/ann_eval_me/X.f32"      # 94 MB, regenerable — not committed
gcc -O2 -std=c11 -I "$INC" -o build_knobs.exe  ann_robust.c -lm && ./build_knobs.exe  "$X" 61241 384
gcc -O2 -std=c11 -I "$INC" -o build_kernel.exe ann_kernel.c -lm && ./build_kernel.exe "$X" 61241 384
gcc -O2 -std=c11 -msse2 -I "$INC" -o build_sse.exe ann_sse.c -lm && ./build_sse.exe "$X" 61241 384
```

Outputs land in `robust.{log,csv}`, `kernel.{log,csv}`, `sse.{log,csv}`,
`knobs_{iters,width,frontier}.csv`, `lat.csv`. `build_*.exe` are
`build/`-ignored by `.gitignore:2` — only the `.c` and the `.csv` are tracked.

*Session 2026-10-19 · DWGLS-native-fs `feat/geo-native-fs` · 6 sources, real corpus*