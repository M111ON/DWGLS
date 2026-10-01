# TOWER LADDER — 3×64 stack as a geometric address space

**Date:** 2026-09-30
**Artifacts ported:** `F:/Artifact/3d_stack_144.html` (V4.2), `F:/Artifact/pogls_2_n.html` (V4.0)
**Output:** `core/geo_tower_ladder.h`, `tests/test_tower_ladder.c`, `tools/tower_ladder_real.c`

---

## 1. What this is

The POGLS simulators were a **visual** artifact: 3 towers × 64 slots = 192, 144 active
nodes (48 per tower) + 48 residual crossing nodes, with a 4×4 space-filling path per
layer and a pointer that walks, shifts, and shuffles.

This is the same object as **header-only integer-only C**, where the *address* is the
deliverable and the drawing is dropped. The stack becomes a coordinate space, not a
picture.

| Layer | Meaning |
|-------|---------|
| 192 slots | 3 towers × 64 = the full address range |
| 144 active | 48 per tower = 3 layers × 16 cells |
| 48 residual | 16 per tower = crossing nodes between layers |
| walk | `tl_next_active` closes a 144-slot loop, skipping residual |
| pointer | `tl_shift` = modulo 192 (not a bitmask) |
| log | `TlLog`, 972-frame write-behind ring, LIFO rewind |

---

## 2. Three findings from reading the artifact (before any code)

These are all **defects in the original simulator**, verified against the source
tables. They are pinned as tests so they cannot silently return.

### 2.1 The three "space-filling layers" are one curve, not three

```
HL_L1 = rot90ccw(HL_L0)
HL_L2 = mirx(HL_L1)
```

and `HL_L0` is exactly the transpose of the standard d2n Hilbert curve on 4×4
(oracle in `tests/test_tower_ladder.c` test 1, recomputed from the recursion and
compared — the table is never checked against itself).

So the 48-cell ladder is **one 4×4 curve under three symmetries**. In plan view the
three layers overlay on the same 16 cells; the layer index is what separates them.
This is not a bug — it is what makes the stack a stack — but it does mean "3 layers
of space filling" is a misreading. There is one space-filling curve.

### 2.2 `& 0xBF` is the wrong operator for 192 slots

The artifact treats `0xBF` as a slot mask. 192 is not a power of two, and `0xBF`
clears bit 6:

| Claim | Measured |
|-------|----------|
| slots with bit 6 set → unreachable | **64 of 192** |
| shift-left disagrees with modulo | **128 of 192** |
| shift-left is non-invertible (right∘left ≠ id) | **191 of 192** |
| pointers that survive the round trip | **1** (`p = 1`) |

Half the address space is unreachable and 191/192 shift-lefts destroy information.
The module uses modulo. Tests 17–19 assert the defect, so the mask cannot be
reintroduced as a "simplification".

### 2.3 The residual zone was not an address space

The artifact places 48 residual nodes with:

```js
layerIdx = Math.floor(index / 5);   // reaches 3 — but only 3 layers exist (0,1,2)
rawX = (subIdx % 2 === 0) ? -0.8 : 3.8;
rawY = (subIdx < 2)     ? -0.8 : 3.8;
```

Measured: **39 distinct coordinates for 48 slots, 9 collisions.** Two causes —
`index/5` invents a fourth layer, and the `-0.8/3.8` map yields only 4 distinct
`(x,y)` pairs (`subIdx` 3 and 4 land on the same corner).

Replaced with pure integer arithmetic:

```
rung   = residual_local / 4     (0..3)   which layer boundary
corner = residual_local % 4     (0..3)   which corner of the 4x4
```

16 per tower × 3 towers = 48 unique slots, bijection, no floats.

### 2.4 Minor: the "Peano" table is not space-filling

Step 11→12 is `(3,0) → (3,3)`, a 3-cell jump. It is a boustrophedon. Kept as-is
because the engine switch selects it, but the test now **asserts** the
discontinuation rather than leaving it implied.

---

## 3. Real-data verification

The unit test proves the math. `tools/tower_ladder_real.c` proves it carries real
bytes: it mmaps a real GGUF, takes a real tensor, and moves its bytes through the
192-slot stack.

**Method (written so it can fail).** The trap avoided: placing
`stack[order[i]]` back onto itself reconstructs the stack for *any* order, so that
formulation is a tautology. Here the view order decides **which source chunk is
stored at which slot**, so a view with a duplicate or a gap loses real bytes and the
`memcmp` against the untouched tensor fails.

| View | Order |
|------|-------|
| V0 | slot order (identity) |
| V1 | `tl_next_active` walk: 144 active + 48 residual |
| V2 | coprime stride `(i*37) % 192` |
| V3 | reverse |

### Results

```
$ make tower-ladder-real MODEL=F:/model/Qwen3-4B-Q4_K_M.gguf MIB=128
  tensor: token_embd.weight  dtype=14  319065600 bytes
  passes: 10922  real bytes exercised: 134209536 (128 MiB)
  ok   real tensor bytes roundtrip through 4 views            0 bytes differ
  ok   all 4 views are permutations of 0..191                 no duplicate, no gap
  ok   no two views are secretly the same order               tests are independent
  ok   address walk throughput                                1028 MB/s over 1.00 s
  ok   0xBF mask cannot reach 64 of 192 slots (artifact defect)
  ok   artifact residual zone loses 9 of 48 addresses         39 distinct, 9 collide
  ok   tl residual zone is a bijection                        16 x 3 = 48 unique slots
  tower_ladder_real: 8 passed, 0 failed
```

### Across model families (type-blindness)

| Model | dtype | Tensor | Result |
|-------|-------|--------|--------|
| Qwen3-Embedding-0.6B-Q8_0 | 8 (Q8_0) | token_embd | 8/8 |
| Qwen3-4B-Q4_K_M | 14 (Q6_K) | token_embd | 8/8 |
| MiniCPM5-1B-Q4_K_M | Q4_K_M | output | 8/8 |
| huihui-moe-1b-q4_k_m | Q4_K_M | token_embd (MoE) | 8/8 |
| LFM2.5-8B-A1B-Q4_K_M | Q4_K_M | token_embd (MoE 32 experts) | 8/8 |
| Qwen3.5-2B-Q8_0 | 8 (Q8_0) | token_embd | 8/8 |

**6/6 models, 8/8 each.** The field never inspects byte meaning — consistent with
the field contract (#6192).

### Mutation checks (a test that cannot fail proves nothing)

| Mutation | Result |
|----------|--------|
| reverse view gains a duplicate slot | **caught** — 0 passed, 1 failed |
| stride 37→36 (non-coprime) | **caught** — 0 passed, 1 failed |
| write indexes source by slot instead of stream position | **caught** — 0 passed, 1 failed |
| control: claim `0xBF` reaches all slots | **caught** — 7 passed, 1 failed |
| control: claim artifact zone is fine | **caught** — 7 passed, 1 failed |

Unit test separately: 6/6 mutations caught (duplicate cell, rung/corner swap, log
off-by-one, MOD-37→36, corner collapse, step overflow).

---

## 4. Two bugs found by testing, not by reading

Both were invisible to inspection and surfaced only when the code was run.

### 4.1 `tl_next_active` was stuck at slot 0

The function returned the current layer/step rather than the next one, so
`tl_next_active(0) == 0` and the 144-slot walk never moved. Caught by the
"visits all 144 active slots exactly once" test.

### 4.2 `tl_jump` is a bijection but 0 is a fixed point

`tl_jump(0) = 0 * 37 % 192 = 0`. The stride is invertible (gcd(37,192)=1) so it is a
valid *single* jump — but **iterating it never leaves slot 0**. The original view
V2 iterated the jump and was therefore a degenerate 192-fold constant. The
traversable form is indexed: `(i*37) % 192`. Both facts are now pinned in the unit
test (13b) — invertible ≠ traversable is a general trap, not specific to stride-37.

---

## 5. Regression status

`build/` held **86 stale Linux ELF binaries** with extensionless names. On this
Windows/MSYS shell gcc emits `build/test-<t>.exe`, but the extensionless ELF
shadowed it, so the group runner executed a Linux binary and reported RUN FAIL.
This was causing **45 of 53 GEO failures on a clean checkout** — zero code bugs.
After clearing them:

| Group | Result |
|-------|--------|
| GEO | 53/53 |
| ACTIVE | 68/68 |
| SMOKE | 15/15 |
| TESS | 30/30 |
| BFS | 16/16 |
| WALK | 17/17 |
| KIS | 10/10 |
| CAP / GHOST / KV / 6ICO / FIBO | 7 / 4 / 9 / 5 / 3, all green |

---

## 6. What is NOT proven

Stated plainly, per the theory/verification distinction:

- **No consumer.** Nothing in the inference path uses `geo_tower_ladder.h` yet. It
  is a proven, tested address space, not a wired subsystem.
- **192 is not 20736.** This is a local ladder structure, not the field itself. Any
  claim that it *is* the 20736 frame is unsupported.
- **No scaling claim.** Throughput (≈1 GB/s) is a memcpy-bound measurement of the
  harness, not of anything the module does. It shows the addressing adds no
  measurable overhead; it is not a performance result.
- **The Peano path is not space-filling** and is kept only for engine-switch parity.
- **Residual geometry is asserted, not drawn.** `(rung, corner)` is a bijection, but
  whether it is the *right* crossing layout has not been checked against a
  visualization.

---

## 7. Build and run

```bash
make test-geo                                  # 53/53, includes test_tower_ladder
make tower-ladder-real                         # default model, 256 MiB
make tower-ladder-real MODEL=<path.gguf> MIB=512
```

Direct:
```bash
gcc -O2 -I. -Icore -o build/tower_ladder_real tools/tower_ladder_real.c
./build/tower_ladder_real F:/model/Qwen3-4B-Q4_K_M.gguf 256
```
