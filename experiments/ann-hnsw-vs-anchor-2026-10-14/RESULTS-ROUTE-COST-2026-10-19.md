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
`topb=2` vs full exact scan, `K=8`, `iters=2`.

| kernel | routed topb=2 | brute (exact) | note |
|---|---|---|---|
| A `double` scalar + `double` norm | 45.5 ms | 113.9 ms | **what the server does** |
| B `float` scalar + `float` norm | 43.1 ms | 111.6 ms | 1.06× |
| C `float` 4-accumulator, **no norm** | **9.5 ms** | **20.3 ms** | cosine == dot (unit-norm) |
| D `float` 4-accumulator + `float` norm | 31.1 ms | 76.8 ms | norm alone costs 3.3× |

**A → C is 4.8×.** Two separate wins, and they are not the same size:

1. **drop the norm pass** (safe: every row is unit-norm — `max | ‖x‖−1 | = 5.96e-07`)
   A → D is 1.47×, D → C is 3.26×.
2. **break the accumulator dependency chain** `double` → 4× `float` (A → B is only
   1.06× on its own, so the scalar *type* is not the issue — the *chain* is).

Routing's ceiling is fixed by how much it prunes: `topb=2` scores ~35% of the
rows, so at best ~2.9× exact **with the same kernel**. C shows 2.1×. Both
numbers are real; routing is not broken, it was simply being compared against a
handicapped baseline.

---

## 2. SSE2 buys nothing — `ann_sse.c`

The obvious next move is SIMD. On this box only SSE2 is safe (no AVX/AVX2/FMA —
G4400). Same 200 queries, exact scan, variants C/E/F confirmed **200/200 identical
top-5**. Variant R is deliberately *not* identical — it is the approximate route
and matches the exact answer on **167/200** (0.835 @ top-5), which is the
recall it is buying its speed with. That is the whole trade, side by side.

| variant | mean | p50 | p95 | speedup |
|---|---|---|---|---|
| C scalar `float` 4-acc | 18.11 ms | 15.91 ms | 32.97 ms | 1.00× |
| E SSE2 (4× unrolled, 4 acc) | 18.45 ms | 14.90 ms | 31.98 ms | **0.98×** |
| F SSE2 + 64-row blocking | 16.26 ms | 14.82 ms | 30.58 ms | **1.11×** |
| R routed `topb=2`, SSE2 | 8.03 ms | 6.75 ms | 16.22 ms | **2.26×** |

SSE2 is a **wash** — faster p50, slower mean, no honest gain. The exact scan is
**memory-bandwidth bound**, not compute bound: 61,241 × 384 × 4 B = 94 MB pulled
from DRAM per query at ~5 GB/s effective. Blocking helps only 1.11× because the
tile still has to come from DRAM.

**Consequence for §3.3:** widening the route (bigger `K`, more `topb`) trades
recall for bandwidth, and bandwidth is exactly the scarce resource here.
Routing is worth keeping precisely because it touches less memory.

---

## 3. §3.2 — how few kmeans iterations? (`ann_knobs.c` + `ann_robust.c`)

Recall is **flat across `iters` 2…25** (one seed: 0.9265–0.9310). Repeating over
**5 independent query seeds × 200 queries** confirms it is not one lucky draw:

| iters | topb | mean | min | max | seed spread | train |
|---|---|---|---|---|---|---|
| 2 | 2 | 0.9281 | 0.9055 | 0.9400 | 0.0345 | **0.60 s** |
| 2 | 4 | 0.9896 | 0.9855 | 0.9920 | 0.0065 | 0.60 s |
| 2 | 6 | 0.9982 | 0.9975 | 0.9995 | 0.0020 | 0.60 s |
| 25 | 2 | 0.9315 | 0.9180 | 0.9500 | 0.0320 | 8.93 s |
| 25 | 4 | 0.9894 | 0.9845 | 0.9920 | 0.0075 | 8.93 s |
| 25 | 6 | 0.9980 | 0.9970 | 0.9990 | 0.0020 | 8.93 s |

At `topb=2` the sign **flips** between seeds: `iters=2` wins on seed 12345
(0.9310 vs 0.9305) and loses on the 5-seed mean (0.9281 vs 0.9315). The gap is
**~0.003**, while the seed-to-seed spread is **~0.034** — an order of magnitude
larger. At `topb≥4` the difference is ≤0.0002.

**Verdict:** `iters=2` is safe. The 0.003 at `topb=2` is inside the noise the
query draw already produces, and it buys **14.9×** on train (0.60 s vs 8.93 s)
→ regrow **0.90 s instead of 9.22 s**. Handoff §1.2's 9.9 s was ~89% kmeans
iterations for no measurable recall.

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

## 5. Method notes — why absolute ms differ between these files

`lat.csv` records brute at **57.6 ms**; `kernel.csv` records brute variant A at
**113.9 ms**. Both are real measurements of the same configuration. Causes:

- different `k` (top-10 vs top-5) → different insertion-loop cost;
- cache state (in `kernel.csv` brute runs immediately after a routed pass);
- 200 queries is small: p95/max are dominated by a handful of outliers.

**Cross-harness absolute times are not comparable; only within-run ratios are.**
The 4.8× A→C and 2.1× C→routed ratios are same-run, same-query, same-k and are
the numbers to trust. The `16 ms` in the handoff was never reproduced by any
harness here.

---

## 6. Recommendation

1. **Kernel first** (`src/ann_search.c:835`): single-pass `float` with 4
   accumulators, norm skipped for unit-norm entries — **4.8×** on the scan,
   applies to *every* path including full scan. Store a `unit_norm` flag per
   entry rather than assuming, and keep the norm path for any entry that fails.
2. **Do not add SIMD for speed** on this hardware — memory-bound. Blocking is
   the only true 1.11× and it is marginal.
3. **`iters=2`** for regrow: regrow 9.22 s → **0.90 s**, recall within noise.
4. **`K=8, topb=4`** as the serving default: 0.989 recall at 59% scan,
   ~8 ms with the fixed kernel.
5. Routing keeps its case, but its value is **bandwidth**, not arithmetic —
   re-evaluate it whenever corpus size changes, since the 2.1× is a function of
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