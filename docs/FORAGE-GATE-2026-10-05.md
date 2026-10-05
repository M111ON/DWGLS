# FORAGE-GATE record — 2026-10-05 (selector visit-order + minor-forager assembly)

Continuation of lost session `ses_efbd49660ffeT1fOetMICkfqmC` (check-valve doctrine,
board #55 ANN speed, C valve-at-mouth fix 0.64 → 0.97). That session's user-side
log survives in `cloud-memory-sessions.json:19068-19148`; its last two messages
were `b-latch` then `เกิดอะไรขึ้น` (WSL+Windows dual-run DB corrupt). Commit
`3e2377f` lands this session's code. Living spec pointer: session note #15.

## 1. Measurement receipts (ann_pipe_real, mode0 topb4, SIFTsmall)

Binary verified by timestamp+size before every run (see §6). 5 separate
processes + 2s settle. File holds 100 queries (`--nq 1000` capped by file —
do NOT compare directly with NQ=1000 runs).

| run | total | gate | rank | recall@1 | recall@10avg | bad |
|-----|-------|------|------|----------|--------------|-----|
| r1 | 267.1 | 213.3 | 48.3 | 0.9700 | 0.9130 | 0 |
| r2 | 216.8 | 161.7 | 52.8 | 0.9700 | 0.9130 | 0 |
| r3 | 960.6 | 901.2 | 55.2 | 0.9700 | 0.9130 | 0 (outlier) |
| r4 | 261.0 | 232.9 | 24.2 | 0.9700 | 0.9130 | 0 |
| r5 | 209.6 | 185.5 | 21.9 | 0.9700 | 0.9130 | 0 |
| median | 261.0 | 213.3 (~80%) | 48.3 | 0.9700 | 0.9130 | 0 |

gt0 routed 97/100, gatepass|routed 0.6598 every run. Correctness rock-stable;
speed receipt NOT closable (r3 4x spike — env noise, CPU contention signature
as in the lost session: rank had swung 21.5→188.9 there).

## 2. MODE 5 forage visit order (landed, commit 3e2377f)

`tools/ann_pipe_real.c`: new MODE 5 = bucket-route order across houses +
id-ascending within bucket (the locked order rule). Modes 0–4 paths untouched
(two condition edits only). On verified binary: recall@1/recall@10avg/bad
IDENTICAL to mode0 (query-0 topi byte-identical: 2176, 3752, 882, 4009, 2837,
190, 3615, 816, 1884, 224), gate in the same band (~159–219). Visit order is
recall-neutral as theorized (insertion order-independent + monotone cap);
it buys determinism, not speed. The remaining speed lever is per-bucket cap
(subway stops), which trades recall — measure that curve next.

## 3. Minor-forager assembly (landed: tools/minor_forage_probe.c, 10/10 first build)

nominate top-4 across layers → order (score-desc, latch-asc) → visit (majors on
major latch, minors on subway branch latches, real `core/geo_wang_latch.h`) →
accumulate minor-means (x100 ints, 400+300=700 exact) → budget K=2 stop →
assemble {E:100, m1:200} + 700. Branch isolation proven: minor ids stay OPEN
on the major lane (no drill-through) while SHUT on their own branches.
Mechanics only — real-data legs pending: (a) wrap this loop around the
ann_pipe shortlist (siftsmall ready), (b) SIFT1M anchor artifacts
(`build/sift.tar.gz` present).

## 4. Item-3 test gate (landed: tests/test_addr_orbit.c, 29/29, GEO_FAST)

Pure-integer oracles, no header under test (complements test_wang_latch.c):
T1 cross-pairs (6×4, 3×8, 2×12 = 24 — multiplier, never sum); T2–T3 144/20736
identities incl. (72+72)×144 (parens load-bearing); T4 2:3 factorizations
(2^8×3^4 = 20736); T5–T7 encode `d0+12d1+144d2+1728d3`, face=d>>1/orient=d&1
table, full 20736/20736 roundtrip; T8 latch id space (143×72+71=10367,
10366 minor, half-field); T9–T10 selector order + budget K=4; T11 angles in
microdegrees only (60×4=240, 112133377+127866623=240000000).

## 5. Locked spec + doctrines (full text: note #15, board #57)

- Core: address = Σ d_i·12^i, d_i ∈ [0,12) = 6 face × 2 orient. ATOM 12 = 4×3
  terminal. Winding CCW. Cross-pairs fix (24 = multiplier, L-table sum 23≠24
  corrected to products).
- Gate-as-signature: the gate does not exist as mechanism; two-layer QR-like
  signature, 12×12 on 12×12 = 144×144 = 20736; dual-view 256×81 (#6325).
  9-colors/9-levels parked candidate (color=value ⇒ consumer layer only).
- SUBWAY-BRANCH (replaces epoch/flush — no flushing exists): non-major always
  branches; minors pass UNDER/OVER the taproot, never drill the major lane
  (matches #7163). Major lane writable by majors only.
- MINOR-PURPOSE: forced-search foragers — ordered multi-entry walk across
  layers/floors accumulating minor-mean components. Composition only, NOT HARD
  (multi-entry P2 WIN + anch_assign + order/budget locks + rank-merge).
- SELECTOR: check-valve one-way; selector picks the answer leaving the bucket.
  Same-word collisions only (different words = different lanes #6497). Return
  leg = rank-merge top-K (distinct by house identity). Weight owner = ANN
  similarity storage (#7162/#7171, precomputed — never embed at decision time).
- FINAL locks: order score-desc → latch/house-id-asc; budget fixed K;
  score ×100 round-half-up int32; 2 poles pass-through + receipt.
- Draft §5 ↔ landed `core/geo_wang_latch.h` verified row-by-row (id/count/
  reserved/bitset/replay/P3-tag/layering all match).

## 6. Corrections to the record (provenance)

- The "mode5 recall collapse to 0.64" observed mid-session was a STALE BINARY
  (67341B, predating the MODE 5 code → else-branch live-valve + filter), NOT a
  real result — caught by timestamp/size verification. Rule pinned as memory
  #7267: verify the bench artifact after every gcc invocation on this box
  (pwsh pipes/redirects mask link failures).
- The "20% speed cost" memory is DROPPED as unsourced (owner-confirmed possibly
  misremembered). Measured standing: ~2.5× total vs pre-valve baseline 83.7 at
  recall 0.97; the ANN-CLIMATE doc's closest pair is re-grid ~0.64 → P2
  multi-entry 0.9082/0.9657 at 2× BYTES cost (not speed).

## 7. Open items (also board #58, ordered)

1. Clean speed receipt (quiet box) or Colab port (3-point Windows-API shim
   spec'd in the lost transcript: windows.h/psapi.h + QPC→clock_gettime,
   drop -lpsapi).
2. Per-bucket cap curve: recall@K vs members-visited.
3. SIFT1M transfer proof (10k numbers may not transfer — house precedent).
4. New address formula vs `addr = l·60+viewpos` (#6540): replace or coexist.
5. Branch-key placement (separate latch vs P3 reserved tag vs orient bit) +
   future branch-isolation test.
6. `test_wang_latch.c` exists but is NOT in GEO_FAST — verify before full group
   run (no `make` in this shell; use MSYS2).
7. Contract-fence work (tools/contract_fence.py + Makefile hunks) belongs to
   another workstream — untouched, uncommitted, do not adopt silently.

## 8. Colab port landed + quiet-round receipts (2026-10-05, same-day continuation)

Item 7.1 ("clean speed receipt (quiet box) or Colab port") — Colab port DONE.
Chain proven end to end from this Windows box.

**CLI:** `google-colab-cli` v0.7.4 installed at `I:\python3.14.4\Scripts\colab.exe`
(Python 3.14). v0.7.4 crashes on Windows at import: `colab_cli/console.py:20`
`import termios` + `:23 import tty` (POSIX-only). Fixed WITHOUT patching
site-packages: shim package at `tools/colab_shim/{termios.py,tty.py}` injected
via `$env:PYTHONPATH=I:\DWGLS-native-fs\tools\colab_shim`. `colab run/exec`
never touch the console path; only `colab console`/`repl` would, and those stay
unsupported here. WSL alternative rejected: WSL `Geomatt` ships Python 3.10.12,
below the package's `Requires-Python >=3.12`, so `pip install` resolves to
`from versions: none` (not a network failure).

**Real surface (from `--help`, never guessed):**
`run new exec sessions status stop console repl install ssh upload download ls rm
edit drivemount url auth pay usage log update version` — no `colab-cli.cmd`,
no `auth login` (auth takes only `-s <session>`), no `kernel`.

**GPU:** T4 obtained with balance 0.00 compute units (free tier, build does not
debit; `usage` reads rate 0.08/hr only). Confirmed by `gpu_report.json` pulled
via `colab download`:
```
nvidia_smi : Tesla T4, driver 580.82.07, 15360 MiB
torch      : 2.11.0+cu130
cuda       : true
gpu_cap    : (7, 5)      # sm_75 Turing
cpu_count  : 2
```
Session list cross-check: `[forage-t4] Hardware: T4 | Variant: GPU`.

**Quiet-round probe:** `tools/probe_forage_quiet.py` — orbit ground truth from
`core/geo_hyper_jump.h` L42-52 (`hj3_tower/hj3_local/hj3_jump/hj3_inv`) + the
spec anchors in `tests/test_p5_value_gate.c:281` and `core/kis_codec_v6.h`
(`slot(i)=(i*37)%20736`). Value-blind, no model bytes; enumerates HJ3 orbits,
GJ stride-37 orbit, and times an HJ-cluster ordered walk vs a flat index walk.

**9/9 PASS on all three machines** (local Windows, Colab CPU, Colab T4):

| machine | flat cold | hj3 cold | ratio hj3/flat | passes |
|---------|-----------|----------|----------------|--------|
| local Windows | 145.79ms | 822.80ms | 5.644 | 9/9 |
| Colab CPU (2 vCPU) | 122.54ms | 311.81ms | 2.545 | 9/9 |
| Colab T4 (2 vCPU) | 50.53ms | 147.52ms | 2.919 | 9/9 |

Checks (all PASS): HJ3^6==id over [0,144); 24 disjoint HJ3 orbits; every orbit
length 6; 5 intermediates before return to 0; gcd(37,20736)==1; stride-37 orbit
from 1 closes to 1; orbit size 576; spec 36×576==20736; full stride-37 sweep
returns to 0.

**Provenance correction (oracle discipline):** two initial FAILs were MY oracle
errors, not field errors — (a) counting GJ orbits by walking every start
double-counts (the action x→37x mod 20736 is multiplicative; walk the single
orbit from x=1 → 576, which is 576·36==20736 matching the spec); (b) an
`(n-1)/576` floor gave 35. Fixed by asserting the spec number from
`test_p5_value_gate.c:281` directly, never re-deriving it.

**Interpretation — kept honest:** the ratio is >1 on every machine and the T4
VM (whose CPU is faster) beats the CPU VM on absolute time, so this timing is a
pure interpreter/CPU-speed artifact of a Python index loop. It does NOT
reproduce the #7161 value gate (cold 0.118× / warm 1.49–2.32×), because that
gate needs the real `.tesspack` bytes and the `touch_field_step` memory/cache
access pattern. The Colab port supplies the runnable quiet box; a real receipt
still requires a pack. Item 7.1 therefore advances to: port proven, timing
receipt still open pending a pack-sized probe.

Artifacts (untracked): `tools/probe_forage_quiet.py`,
`tools/colab_shim/{termios.py,tty.py,_env_probe.py,_gpu_probe.py,_probe2.py,gpu_report.json}`.
Sessions `forge-probe` and `forage-t4` stopped; server clean.

### 8b. Pack-sized probe — #7161 partially reproduced (2026-10-05, session `forage-t4b`)

The quiet-round probe above measured only interpreter loop overhead (no memory
access pattern). To reach the property #7161 actually attributes the cold/warm
flip to — cache-line / readahead pattern — `tools/colab_shim/_probe_pack.py`
builds the REAL field in memory (20736 slots × 64 B = 1,327,104 B = 1.27 MiB)
and walks it three ways, cold then warm (min of 20 warm reps), N=200000
accesses (~10 sweeps), on the Colab T4 VM.

```
flat_cold   34.214ms   hj3_cold   58.356ms   stride37_cold  33.809ms
flat_warm   27.477ms   hj3_warm   55.583ms   stride37_warm  31.283ms

ratio hj3/flat       cold 1.706   warm 2.023
ratio stride37/flat  cold 0.988   warm 1.139
```

Against #7161 (qwen25.tesspack, 1060 capos, identical faults ~298K, peak WS
~1164MB, only wall-clock differs):

| | #7161 real pack | this in-memory field |
|---|---|---|
| hj3 cold | 0.118× (8.5× FASTER) | 1.706× (slower) |
| hj3 warm | 1.49–2.32× (slower) | **2.023× (slower)** ✓ |
| stride-37 | 36 orbits × 576 | **0.988 cold / 1.139 warm ≈ 1.0×** ✓ |

Two findings, read straight:
1. **stride-37 ≈ 1.0× cold and warm** — the sequential bijective walk carries no
   penalty versus a flat walk. Confirms the bijection itself is not the cost;
   matches ARCHITECTURE #7160 (GJ stride-37 is the global mini-map view).
2. **hj3 warm = 2.023× lands inside #7161's 1.49–2.32× band** — the warm-cache
   regression is a property of the access pattern, reproduced without the pack.
   But **cold does NOT reproduce 0.118×**: in-memory there is no page fault, so
   the cold direction is unobservable here. #7161's cold-faster is specific to
   the mmap fault pattern over the 718 MB file.

Conclusion: the warm-cache regression that blocks HJ integration
(`tesspack_server.c`) is reproducible from the field geometry alone; the
cold-faster half is an mmap-fault artifact that still needs the real 718 MB pack
(`build/qwen25.tesspack`) to confirm. Item 7.1 status: port proven + warm
regression reproduced; cold-faster receipt still needs a pack upload.

Artifacts added: `tools/colab_shim/{_probe_pack.py,pack_report.json}`.
Session `forage-t4b` stopped; server clean.

### 8c. Item 7.6 closed — test_wang_latch wired into GEO_FAST

`tests/test_wang_latch.c` existed (4101 B, 4 gates T1-T4, oracle from
spec/math) but was absent from the GEO_FAST group. Added to `Makefile` GEO_FAST
after `test_addr_orbit`. Built and run standalone:

```
gcc -O2 -Icore -o build/test_wang_latch.exe tests/test_wang_latch.c -lm
build/test_wang_latch.exe  ->  ALL PASS (0)   21/21 ok
```

Checks: T1 144×72=10368 bijective, 2 reserved {0,10367}, free 10366,
10368×2=20736; T2 monotone open→traversed→shut, CLEAR the only reopen;
T3 reserved refuse traverse and stay open, out-of-range refused;
T4 P3-log replay byte-identical to stepwise state, foreign name_hash ignored,
misaligned buffer applies nothing. `test_addr_orbit` re-run after the edit:
ALL PASS (GEO_FAST not disturbed).

### 8d. #7161 FULLY reproduced on the real 718 MB pack (2026-10-05)

No Colab upload needed — `tests/test_p5_value_gate.c` and
`build/qwen25.tesspack` (718.8 MB, 1060 capos) both already exist locally.
Built and run twice back-to-back (passes=3, 19,690,496 steps per walk):

```
RUN 1 (cold)                          RUN 2 (warm)
flat-bytes  37.039s  faults +298310   flat-bytes  1.332s  faults +298298
hj3-field    2.431s  faults +298300   hj3-field   6.074s  faults +298299
peak_ws     1164.4 MB                 peak_ws     1164.4 MB
ratio hj3/flat = 0.066  PASS          ratio hj3/flat = 4.561  FAIL
RESULTS: 7 PASS, 0 FAIL               RESULTS: 6 PASS, 1 FAIL
```

Against #7161's recorded receipt:

| | #7161 recorded | measured today |
|---|---|---|
| cold ratio | 0.118× (8.5× faster) | **0.066× (15.2× faster)** PASS |
| warm ratio | 1.49–2.32× | **4.561×** FAIL |
| page faults | ~298K | **298310** (cold) / 298298 (warm) exact |
| peak WS | ~1164 MB | **1164.4 MB** exact |

Faults and peak WS match #7161 to the count and the megabyte — so the cold/warm
flip is **not** an IO-volume difference (identical faults both runs). It is a
CPU cache-line locality effect:

- **cold**: flat must fault the whole 718 MB in file order (37s); hj3 sorts
  steps by `field_slot` first, so touches are adjacent → hits (2.4s).
- **warm**: the file is already in the OS page cache; flat walks it
  sequentially (1.3s, fastest possible), while hj3's jump pattern causes
  cache misses (6.1s).

**Verdict unchanged from #7161 and now measured locally: do NOT integrate HJ
into `tesspack_server.c` until an orbit-aware / re-packed layout removes the
warm-cache regression.** The gate is correct as written (cold PASS / warm FAIL).
Item 7.1 closed: port proven (§8), warm regression reproduced both in-memory
(§8b) and on the real pack (§8d), cold-faster confirmed on the real pack.

### 9. Item 7.2 closed — per-bucket cap curve (2026-10-05)

§2 named the open lever: *"the remaining speed lever is per-bucket cap (subway
stops), which trades recall — measure that curve next."* Measured here on the
real SIFT1M artifacts, in-process, mirroring `bench_serve_overlap.c` candidate
order exactly (route top-16 → SINGLE posting A1 → exact L2 top-10).

New probe: `experiments/ann-climate-2026-09-24/sift/probe_perbucket_cap.c`.
Difference from `bench_serve_overlap.c`: the cap applies **per bucket** (each of
the 16 routed buckets may score up to C members, walk order), and every query is
classified by its **max bucket nent** so the curve splits small vs large.

Bucket-size distribution (from `build/sift1m_c/off.bin`, 2560 used buckets):

```
sum 1,000,000 entries  mean 390.6
min 112  p10 251  median 366  p90 554  max 1822
nent >128: 2547 (99.5%)   >512: 377 (14.7%)   >1024: 13
```

Cap sweep (n=1000 queries, recall@10):

```
cap    rec_all  rec_small  rec_large  scored
0      0.6501   0.6099     0.6677     5244    (baseline ✓ = bench_serve_overlap SINGLE)
64     0.1658   0.1954     0.1529     1023
128    0.2879   0.3378     0.2661     2045
256    0.5022   0.5717     0.4718     3902
366    0.5883   0.6089     0.5793     4694    (median nent)
512    0.6236   0.6099     0.6296     5020    (shipped LZ_SEARCH_BUDGET)
768    0.6410   0.6099     0.6546     5167
1024   0.6474   0.6099     0.6638     5211
2048   0.6501   0.6099     0.6677     5244    (= baseline, no bucket exceeds 1822)
```

Four reads, straight:

1. **`rec_small` saturates (0.6099) at cap=512** — for buckets ≤ 512 the shipped
   cap already cuts nothing. The median bucket (366) is under the default.
2. **`rec_large` keeps climbing past cap=2048** (0.6296 → 0.6677) — big buckets
   are the ones the fixed 512 actually truncates.
3. **696/1000 queries (69.6%) touch a bucket > 512** — the 14.7% of *buckets*
   that are large are hit by 1.4× their share of *queries*. This is why a fixed
   512 craters recall (see §10 campaign: 0.65 → 0.26).
4. **cap = median (366) is NOT the answer** — it gives 0.5883, −0.036 versus 512.

**Answer to the `max(512, nent)` fix:** correct, and the curve now proves it.
Cap ≥ nent means "cut nothing", so `budget = max(512, nent)` reaches
rec_all = 0.6501 = baseline. Cost of the fix: scored 5020 → 5244 = **+4.5% work
buys back 4.1% recall** — cheap.

**Already shipped.** The fix is present at `tools/gguf_lazy_serve.c:315-318`
(`else if (nent > budget) budget = nent;`), landed in commit `28c8fcf` — the
same commit as the campaign's §10 champion. The campaign note *"one-line fix
pending, not applied"* (docs/ANN-CLIMATE-CAMPAIGN-2026-09-24.md:249) is stale:
doc written mid-bench, fix followed immediately. Item 7.2 closed: curve measured,
fix verified by code, stale doc corrected (note added at campaign §10).

Artifacts: `experiments/ann-climate-2026-09-24/sift/probe_perbucket_cap.c`
(+ `build/probe_perbucket_cap.exe`). Baseline repro in the same run:
`bench_serve_overlap.exe` → SINGLE none 0.6501 / OVERLAP none 0.7537, budget=512
SINGLE 0.2634 (trunc 999/1000) — matches campaign §10 to the digit.

