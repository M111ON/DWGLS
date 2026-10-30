# Field arithmetic — containers, factor exchange, and what is NOT evidence

**Date**: 2026-10-30
**Scope**: arithmetic of the field `20736` and of the containers built from it.
Every number below was produced by a script in this folder or by `tools/const_map.py`;
none is estimated. Where a claim is a theorem it is marked THEOREM, where it is a
measurement it is marked MEASURED, where it is a coincidence it is marked COINCIDENCE.

**What this document is NOT**: a container decision, a weight-structure claim, or a
justification for changing the field size. It records which arithmetic is real and
which matches were accidental, so the next session does not re-walk it.

---

## 1. The field's prime set

```
20736 = 2^8 * 3^4 = 256 * 81 = 16^2 * 9^2 = (16*9)^2 = 144^2
```

The field is **5-free and 7-free**. This single fact explains most of the confusion
in the session that produced this document.

| quantity | factorization | has 5? | has 7? |
|---|---|---|---|
| field 20736 | `2^8 * 3^4` | no | no |
| quadrant 5184 | `2^6 * 3^4` | no | no |
| pipe 1728 | `2^6 * 3^3` | no | no |
| row stride 896 | `2^7 * 7` | no | **yes** |
| free space 5888 | `2^8 * 23` | no | no |
| pentagon 360 | `2^3 * 3^2 * 5` | **yes** | no |

Consequence: `20736 / 5 = 4147.2`, `20736 / 360 = 57.6`, `20736 / 40 = 518.4`.
No rearrangement of factors can fix this — see §3.

---

## 2. The 78-number trap (why "it appears in the repo" is not evidence)

MEASURED — `python tools/const_map.py --audit`:

```
distinct #define values in core/include/tests : 162
  2-3-smooth                                  :  63 (38.9%)

2-3-smooth numbers <= 20736                   :  78
  of which already used by some #define       :  50 (64%)
```

There are only **78** numbers of the form `2^a * 3^b` below 20736, and the repo already
uses 64% of them. DWGLS is built entirely from 2-3-smooth constants, so any candidate
of that form has a **~64% chance of matching a constant by accident**.

Therefore: *"this number is already in the system"* carries no information.
The only question worth asking is whether the **derivation** matches.

Worked example from the session — six numbers that all appear in the repo:

| n | derivation used here | derivation used in repo | same? |
|---|---|---|---|
| 64 | `ord(5 mod 2^8) = 2^6` | `GEO_FACE_UNITS = 8^2` | number only |
| 54 | `ord(5 mod 3^4) = 2*3^3` | `GEO_SIDE_FULL = 6*9` | number only |
| 81 | `3^4` | component of `(8*9)^2` | **YES** (`9^2 = 3^4`) |
| 1728 | `ord(5) = N/12` | `FS_PIPES = 12*144 = N/12` | number only |
| 6912 | `phi(N) = N/3` | KIS axis `= N/3` | number only |
| 5184 | `72^2 = 2^6*3^4` | `BFS_SEEKER_K = 64*81 = 72^2` | **YES** |

Only 5184 has a genuinely matching derivation.

---

## 3. Perfect square <=> every exponent is even (the "twin" rule)

THEOREM. For `P = 2^a * 3^b`, `sqrt(P) = 2^(a/2) * 3^(b/2)` is an integer **iff**
a and b are both even.

This formalizes the owner's "twin" rule: a variable holding an odd exponent must
borrow a factor from the other side before a square container exists.

| container | factorization | all even? | sqrt |
|---|---|---|---|
| 20736 | `2^8 * 3^4` | yes | **144** |
| 5184 | `2^6 * 3^4` | yes | **72** |
| 144 | `2^4 * 3^2` | yes | **12** |
| **10368** = `128*81` | `2^7 * 3^4` | **no (7 is odd)** | — |
| 129600 | `2^6 * 3^4 * 5^2` | yes | **360** |

`128 * 81 = 10368` is not a container. That is precisely why `162 = 2 * 81` carries a
twin 2: it pushes `2^7` up to `2^8`. With `256 = 2^8` the exponent is already even, so
the second variable drops its twin and becomes `81`.

```
128 * 162 = 256 * 81 = 144 * 144 = 20736
```

All three are the same monoid element. Moving a factor between the two variables is
free; the prime set is not.

MEASURED — `tau(20736) = (8+1)*(4+1) = 45`: there are **45** ordered ways to split the
field into two factors, and exactly **1** of them is unified (`144 * 144`).

---

## 4. 144 is the geometric mean of the CRT halves

```
CRT split : 256 * 81   = 16^2 * 9^2
unified   : 144 * 144  = (16*9)^2
and       : sqrt(256*81) = 144
```

So the CRT decomposition (`core/geo_crt_bridge.h`, `256 * 81`) **is** the `16^2 * 9^2`
decomposition, and `144 * 144` is not a different container — it is the CRT split
symmetricized. 144 is the geometric mean of the binary half (256) and the ternary
half (81).

---

## 5. The 16:9 case

```
16 * 9 = 144 = 12^2
sqrt(16*9) = 12                  <- 12 is the geometric mean of 16 and 9
16 : 12 : 9                      <- geometric progression, common ratio 4/3
16/9 = 1.777...                  <- 16:9
```

A 16:9 rectangle has exactly the area of a 12x12 square. This is not a special case —
it is `(ab)^2 = a^2 b^2` applied to `a=4, b=3`.

---

## 6. ord(5) = N/12 (THEOREM)

For `N = 2^a * 3^b` with `a >= 3` and 5 a primitive root mod `3^b`:

```
ord_{2^a}(5) = 2^(a-2)
ord_{3^b}(5) = 2 * 3^(b-1)
ord_N(5)     = lcm(...) = 2^(a-2) * 3^(b-1) = N / 12
```

MEASURED for N = 20736:

```
ord(5 mod 256) = 64  = 2^6
ord(5 mod 81)  = 54  = 2 * 3^3        <- NOT 27 (correction, see §12)
lcm(64, 54)    = 1728 = 2^6 * 3^3
ord(5 mod 20736) = 1728               <- agrees
phi(20736) = 128 * 54 = 6912, and 6912 / 4 = 1728
```

The 12 in `N/12` is `4 * 3`: 5 loses 4 on the 2-axis and 3 on the 3-axis.

**Mechanism warning**: `1728` also equals `FS_PIPES = 12 * 144 = N/12`. The number
matches because both are `N/12`; the mechanisms do not. The spine's 12 is a design
choice (12 ticks per round); the ord's 12 is `4*3` from the group structure. Do not
treat one as evidence for the other.

---

## 7. What x*5 actually fixes (MEASURED)

```
5x = x (mod 20736)  <=>  4x = 0 (mod 20736)  <=>  x = 0 (mod 5184)

fixed points = {0, 5184, 10368, 15552} = k * 72^2
```

So `x*5` pins exactly the **four corners of the 72^2 lattice** and nothing else.

It does **not** preserve quadrants as blocks. Image of quadrant 0 under `x*5`:

```
quadrant 0: 2073 points
quadrant 1: 1037
quadrant 2: 1037
quadrant 3: 1037
```

Reason: `x = 5184q + r` gives `5x = 5184(5q) + 5r`, and `5r` can overflow the quadrant
by up to 4 rounds.

Orbit structure: 128 orbits, sizes in
`{1,2,4,6,8,12,16,18,24,32,36,48,54,64,72,96,108,144,192,216,288,432,576,864,1728}`.
All divide 1728; the four corners are the size-1 orbits.

Practical consequence: `x*5` is a valid global bijection but is **not** a local walk —
it needs dead-end handling for the 4 corners before it can be used as an address walk.

---

## 8. Which 5184 / 1728 matches are real

`python tools/const_map.py 5184 1728`:

```
5184  -> SEEKER_K (tests/test_window_ladder.c:26), BOUND_START (tests/twin_seeker_test.c:24)
1728  -> 14 defines: FR_AXIS_STRIDE, MDIM_PIPES, GEO_FAST_PIPES, KIS_1728,
         GEO_MDIM_PIPES, TESS_AXIS_STRIDE, TH_PENTAGON_NODES, FS_PIPES,
         GP_PIPES, LAYER_SZ, AXIS_STRIDE, ...
```

`core/breathing_fs.h:30` states the derivation explicitly:

```c
#define BFS_SEEKER_K  ((8u*9u)*(8u*9u)) /* 5184 = 72^2 = 64*81
                                         * (interaction plane squared =
                                         * binary-square x ternary-square
                                         * = quarter field). Derived,
                                         * not magic. */
```

`(8*9)^2 = 72^2 = 2^6 * 3^4` — the same derivation reached independently. This is the
one match in the session that is real.

---

## 9. Reshape probes on real weights (MEASURED, negative)

Script: `pentagon_reshape.py`. Data: `qproj_L0.f32` (BF16-decoded q_proj L0), field 0
= first 20736 values.

| reshape | rows | cols | vertical adj r | horizontal adj r |
|---|---|---|---|---|
| 648x32 (Q8_0 block) | 648 | 32 | -0.005581 | -0.000533 |
| 324x64 | 324 | 64 | -0.014333 | -0.001591 |
| **288x72 (pentagon)** | 288 | 72 | **-0.017933** | **+0.000089** |
| 162x128 (quantum) | 162 | 128 | +0.012178 | -0.001447 |
| 144x144 | 144 | 144 | +0.013904 | +0.000948 |
| 128x162 | 128 | 162 | +0.012781 | -0.000635 |
| 72x288 | 72 | 288 | -0.005275 | +0.001778 |

Noise floor from a Gaussian control at the same sample size: **+/-0.0074 to +/-0.0082**
(`1/sqrt(n)`, n = 20,448 pairs).

Every reshape sits at or below the noise floor. **No reshape reveals structure.**
The pentagon view (288x72) is indistinguishable from noise.

The real signal lives at **stride 896** (the tensor's own row) — see
`KIS-ENTROPY-BASELINE-2026-10-30.md`. It is a property of the tensor, not of the field,
and no field reshape creates or destroys it.

Divisibility, for the record — no tensor divides 5184:

| tensor | cells | /5184 |
|---|---|---|
| q_proj / o_proj | 802,816 | 154.86 |
| v_proj | 114,688 | 22.12 |
| up / down_proj | 4,358,144 | 840.69 |
| embed_tokens | 136,134,656 | 26,260.54 |

Weights are `2^a * 7 * p` — 3-free and 5-free. The only shared factor with the field is
`2^7 = 128`.

---

## 10. The two containers and the 25:4 ratio

```
field     : 20736  = 4  * 72^2   (2^8 * 3^4)
geometry  : 129600 = 25 * 72^2   (2^6 * 3^4 * 5^2) = 360^2
ratio     : 129600 / 20736 = 25/4 = 6.25 = (360/144)^2
```

Both are perfect squares (all exponents even, §3). The ratio is the square of the ratio
of their containers. Closures:

```
lcm(20736, 129600)        = 518,400 = 25 fields = 4 * 360^2 = 720^2
lcm(..., 896)             = 3,628,800 = 175 fields
5 fields = 103,680 = 288 * 360   (pentagon closes at 5 fields, not 1)
40 quadrants = 207,360 = 10 fields (Z40 closes at 10 fields, not 1)
```

The only difference between the two containers is the prime set: `{2,3}` vs `{2,3,5}`.
Choosing 360^2 is a legitimate design choice; it is not derivable from the field.

---

## 11. Scope correction — the tensor is a fixture, not a design input

This is the most important section, and it was the owner's correction, not mine.

`core/geo_light_index.h:7-8`:

```
this header never touches payload bytes, only (offset, size) coordinates
```

The light index record is 40 bytes:

```
name_hash u64 | field_slot u32 | hj_cluster u32 | store_off u64
store_size u32 | access u32 | flags u32 | crc32 u32     = 40 B wire
```

```
20736 slots * 40 B = 829,440 B ~= 810 KB in RAM, independent of corpus size
```

So the real container of the vector-DB layer is **20736 anchor slots at 40 B each**, and
the payload (tensor weights, KV cache, embeddings — anything) lives in storage behind
`store_off`/`store_size`.

Consequences:

1. The tensor's shape (`896`, `4864`, `151936`) has **no bearing** on the container.
   `store_off` is u64, so payload size is unconstrained.
2. "Does this tensor divide 5184" was the wrong question from the start. Nothing needs
   the tensor to divide anything.
3. The tensor is a **borrowed immutable fixture**. The first bake does not overwrite it;
   there is no finetune step. It exists to prove the field mechanics (lossless,
   ratio=1, replay) across payload types — which it has.
4. Analysing a fixture offline is not a violation of value-blindness. The field still
   never reads values at runtime. What *would* be a violation is letting the fixture
   choose the container — that is overfitting to a borrowed blob.

The fan-out that memory #7505 describes (`each anchor branches into a 4x6 frustum
quadtree`) is **not in code**. What actually exists:

| header | structure | granularity |
|---|---|---|
| `core/bfs_quadtree.h` | quadtree, 12x12 grid padded to 16x16, depth 4, 341 nodes | over **144 blocks**, not over 20736 anchors |
| `core/anchor_metatron.h:82-86` | `96 pieces x 216 slots = 20736` (`216 = 6^3`) | 96-way fan-out, not 4x6 |
| `core/anchor_metatron.h:85` | `AM_GRID_CUBE_FACE = 24 = 4*6` | a named constant, not a fan-out |

So the `4x6` is a *face count*, not a routing fan-out. The nearest real fan-out is
`96 x 216`, and the only quadtree is over 144 blocks. How many vectors a frustum holds
is still undefined.

---

## 12. Corrections made during the session

Recorded so they are not repeated:

| claim | correction |
|---|---|
| `ord(5 mod 81) = 27` | **54** (`= 2 * 3^3`) |
| `ord(5)` matching `FS_PIPES` is a mechanism link | number only; both are `N/12`, mechanisms differ |
| `1728` was called a coincidence | wrong — `ord(5) = N/12` is forced (THEOREM, §6) |
| "we are modifying the inside" | over-correction — the light index never touches payload bytes (§11) |
| user's `129000` not flagged | product is `129600`; noted late |
| `3^2 * 4^2 = 5^2` | `3^2 * 4^2 = 144`; `3^2 + 4^2 = 25` |

Bug fixed in repo: `core/geo_crt_bridge.h` header comment said `81^-1 mod 256 = 33`
and `256^-1 mod 81 = 64`. The `#define`s were correct (`177` and `25`); only the
comment was wrong. Comments corrected, no code change, no rebuild needed.

---

## 13. Proven / coincidence / open
**PROVEN (theorem or measurement)**
- field is 5-free and 7-free; every 5-bearing quantity fails to divide it
- perfect square iff all exponents even; the "twin" is the borrowed factor fixing an odd exponent
- `20736 = 16^2 * 9^2 = (16*9)^2 = 144^2`; 144 is the geometric mean of 256 and 81
- 16:9 — 12 is the geometric mean of 16 and 9; 16:12:9 is a GP with ratio 4/3
- `ord(5 mod 20736) = 1728 = N/12`
- `x*5` fixes exactly `{0, 5184, 10368, 15552}` and does not preserve quadrants
- `5184 = 72^2 = field/4` and `BFS_SEEKER_K` derives it the same way
- no field reshape reveals structure on real q_proj data (all at noise floor)
- `tau(20736) = 45` factor pairs, 1 unified

**COINCIDENCE (do not build on)**
- `tau(20736) = 45` equalling the octagon number 45
- `1728` matching `FS_PIPES` by value
- `6912` matching the KIS axis by value
- `64` and `54` matching `GEO_FACE_UNITS` / `GEO_SIDE_FULL` by value

**OPEN**
- how many vectors a frustum holds
- whether the `96 pieces x 216 slots` grid in `anchor_metatron.h` is the intended
  replacement for the `4x6` fan-out described in memory #7505 (see §11)
- precision issue in memory #7160: it says stride-37 gives "36 disjoint orbits of
  length 576". At field granularity that is wrong — gcd(37,20736)=1 gives **one**
  orbit of 20736 (MEASURED, §14). The 36x576 figure is correct only at tower
  granularity (20736/576 = 36 towers, gcd(37,576)=1). Both statements are true at
  different levels; the memory mixes them.

---

## 14. Walk locality — x*5 is not a usable address walk (MEASURED)

Script: `walk_locality.py`. Question left open in §7: does `x*5` give locality, or is
it only a global bijection? Answer: it is only a bijection.

Metrics over every start point `s` in `[0, 20736)`. Cache line = 16 slots (4-byte
slots, 64-byte line). `sameQuad` = the step stays inside the same 5184-slot quadrant.

**Multiplicative walks** (`next = k*s mod N`):

| k | ord | orbits | orbit sizes | max | mean abs(d) | sameLine% | sameQuad% |
|---|---|---|---|---|---|---|---|
| **5** | 1728 | 128 | 25 distinct | 1728 | **6221.2** | **0.14** | 39.99 |
| 7 | 864 | 225 | 24 distinct | 864 | 6418.4 | 0.14 | 28.56 |
| 11 | 1728 | 127 | 25 distinct | 1728 | 6599.2 | 0.11 | 27.27 |
| 13 | 1728 | 144 | 28 distinct | 1728 | 6647.8 | 0.17 | 30.73 |
| 17 | 144 | 848 | 13 distinct | 144 | 6708.7 | 0.08 | 29.40 |
| 37 | 576 | 336 | 21 distinct | 576 | 6818.9 | 0.17 | 27.03 |
| 41 | 864 | 220 | 21 distinct | 864 | 6828.7 | 0.04 | 26.81 |
| 137 | 864 | 220 | 21 distinct | 864 | 6888.3 | 0.12 | 25.50 |

**Additive strides** (`next = s+k mod N`) — what GJ / spine already use:

| k | orbits | orbit size | mean abs(d) | sameLine% | sameQuad% |
|---|---|---|---|---|---|
| 1 | 1 | 20736 | 2.0 | 93.75 | 99.98 |
| 12 | 12 | 1728 | 24.0 | 25.00 | 99.77 |
| **37** | **1** | **20736** | **73.9** | 0.00 | **99.29** |
| 144 | 144 | 144 | 286.0 | 0.00 | 97.22 |
| 1728 | 1728 | 12 | 3168.0 | 0.00 | 66.67 |

**Verdict**: every multiplicative multiplier has `mean abs(d)` around 6,200-6,900
slots — 30-33% of the whole field — and `sameLine` at the noise level (~0.1%).
`x*5` is not better than the others; its 39.99% `sameQuad` comes from the four fixed
corners (§7), not from locality. Additive stride 37 keeps **99.29%** of steps inside
one quadrant and is the only walk of the set that is both a single full-field cycle
and quadrant-respecting.

So: `x*5` is a valid global bijection, is not a local walk, and should not replace
stride-37. The `72^2` lattice remains an address-space fact (§8), not a routing gain.

**Caveat on `sameLine%`**: the column assumes 4-byte slots (16 per 64-byte line).
The real index element is a `LIXEntry` of **40 B** (§11), so only `abs(d)=0` keeps two
visits on the same line — with the real record size `sameLine%` collapses to ~0 for
every walk, including stride-37. Only `mean abs(d)` and `sameQuad%` are load-bearing;
`sameLine%` is shown because it separates the additive walks from the multiplicative
ones, not as a prediction of cache behaviour.



---

## 15. Capacity (CLOSED) and the producer-schema blocker (OPEN)

### Capacity — MEASURED at full field size

`tests/test_light_index.c` had its sizes wrapped in `#ifndef`, so the same gate builds at
full field size. Built with `-DNENTRY=20736 -DKANCH=276` (276 keeps 75 entries per
bucket, the same ratio as the default 600/8):

```
capacity: NENTRY=20736  index=829440 B (810.0 KB)  log=829440 B
          payload=1327104 B  slots_filled=20736/20736
G1: full-bucket recall@1 200/200, top2 recall@1 200/200
    scored/query full=20736 top2=152 (ratio 136.73), 4.6 ms/q
G4: rank+fault 200 queries in 0.005s (25.0 us/q), bytes ok=1
RESULTS: 16 PASS, 0 FAIL
Receipt: lix{n=20736 wire=829440B recall_full=200/200 recall_top2=200/200}
```

The predicted 810 KB is exact (`20736 * 40 = 829440`). Filling the field does not
degrade recall; it costs 5x the query time of the 600-entry gate (5.0 -> 25.0 us/q).
The default gate still reports 16 PASS after the `#ifndef` change.

```
gcc -O2 -I. -Icore -o build/test_light_index_cap.exe tests/test_light_index.c -lm \
    -DNENTRY=20736 -DKANCH=276
```

### Producer schema — still OPEN, blocked by one discarded field

```
sid/adaptive_route_sid.h:35   (void)event->view;      <- view accepted, then dropped
sid/zone_card_sid.h:39-46     ZoneCardSID = 42 B packed, no view member
```

`AdaptiveRouteSIDEvent.view` is read nowhere else in the tree (`grep '->view'` finds only
`dramtile_container.h`, a different struct). So the corpus can hand the store a view and
the store silently keeps one route per SID — exactly memory #7819's blocker.

Two candidate fixes, both small:

| option | change | size impact | blast radius |
|---|---|---|---|
| **A. SID record** | add `uint16_t view` to `ZoneCardSID` | 42 -> 44 B, update `_Static_assert` | `adaptive_route_sid.h`, `kv_graph_probe.c`, `sid_kv_compact.c`, `tensor_memory.h` |
| **B. ANN flags** | view in `LIXEntry.flags` bits 1..5 (32 views) | none — `flags` is already "bit0 HOT; rest reserved" | `geo_light_index.h` + rank filter |

`42` is asserted only in `zone_card_sid.h`; nothing else persists a 42-byte record, so
option A does not corrupt stored data. Option B is smaller and touches the layer the
multi-view gate actually runs in (`core/anchor_route.h`), so it is preferred.

Neither option makes multi-view testable by itself: the corpus must emit **one entry per
(SID, view)** instead of one per SID. That is a producer change, not a code change, and
it is the real remaining blocker.

---

## 16. Files

| file | what |
|---|---|
| `pentagon_reshape.py` | reshape probes + divisibility, §9 |
| `walk_locality.py` | x*k vs stride-k walk comparison, §14 |
| `KIS-ENTROPY-BASELINE-2026-10-30.md` | weight structure baseline (stride 896, period-32) |
| `REPORT-2026-10-30.md` | ortho-push / stride / entropy report |
| `tools/const_map.py` | number -> derivation lookup, §2 |
| `core/geo_crt_bridge.h` | CRT 256x81 bridge, comment corrected |
| `core/breathing_fs.h:30` | `BFS_SEEKER_K = 72^2`, derivation in comment |
| `core/geo_light_index.h` | 40 B light index record, §11 |
| `tests/test_light_index.c` | light index gate; sizes `#ifndef`-overridable for the 20736 capacity build, §15 |
| `tests/test_kis_positional_gate.c` | address/value/structure preservation gate |
| `tests/test_kis_relocation_gate.c` | whole-tensor move vs intra-tensor reindex |
| `tests/test_kis_hyper_pythagoras.c` | existing Pythagoras-related KIS test |

The two gates stay in `tests/`, not here: they are listed in `Makefile:25-26`
(TIER1), so moving them would break the build. They are gated, not experimental.

The `.f32` tensor dumps in this folder (`qproj_L0.f32`, `qproj_L12.f32`,
`oproj_L0.f32`, `vproj_L0.f32`, `downproj_L0.f32`, `bf16_slice.npy`, ~30 MB total)
are regenerable by `dump_tensor.py` / `bf16_fetch.py` and are not needed to read
this document.

Reproduce §2 and §9:

```
python tools/const_map.py --audit
python tools/const_map.py 5184 1728 25 360
cd experiments/apex-hidden-2026-09-29 && python pentagon_reshape.py
python walk_locality.py
gcc -O2 -I. -Icore -o build/test_light_index_cap.exe tests/test_light_index.c -lm -DNENTRY=20736 -DKANCH=276
```
