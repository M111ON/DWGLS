# HANDOFF 2026-10-10 — RDH → anchor → frustum → ann axis closed

**Repo:** `I:\DWGLS-native-fs` · branch `feat/geo-native-fs` · commit **`5e88dd2`**
**Session scope:** wire RDH capture into the anchor, the anchor into the frustum,
and the anchor into ann's centroid table. All three built, tested green.
**Live systems untouched:** ann `:8096` was NOT modified, no `kv_anchors.bin` written.

---

## 1. What landed (7 files, 1208 insertions)

| file | self-check | what it does |
|---|---|---|
| `include/anchor_weight.h` | **11/11** | RDH → 8 × 64-bit pattern, fold `64>16>4>1`, centre letter from the 52-pool, fixed weight |
| `include/anchor_frustum.h` | **16/16** | anchor = frustum apex, `1<4<16<64`, 6 faces, capo stable, TritAddr, major-anchor chain |
| `include/anchor_centroid.h` | **12/12** | anchor identity → `K×dim` centroid table in ann's exact format |
| `tests/test_anchor_weight.c` | — | oracle checks (fold, 6 distinct dirs, 52-bijection, weight range) |
| `tests/test_anchor_frustum.c` | — | oracle checks (4^level, branches `[0,6)`, replay, chain) |
| `tests/test_anchor_centroid.c` | — | oracle checks (determinism, save/load round-trip, `anch_assign`, gear seed) |
| `tools/tw_rdh_bench.c` | ⚠️ | **KNOWN BROKEN** — committed as-is, see §4 |

Build/run (all pass):
```sh
cd I:/DWGLS-native-fs
gcc -O2 -Wall -I. -Icore -Iinclude -o build/test_anchor_weight   tests/test_anchor_weight.c   -lm && ./build/test_anchor_weight
gcc -O2 -Wall -I. -Icore -Iinclude -o build/test_anchor_frustum  tests/test_anchor_frustum.c  -lm && ./build/test_anchor_frustum
gcc -O2 -Wall -I. -Icore -Iinclude -o build/test_anchor_centroid tests/test_anchor_centroid.c -lm && ./build/test_anchor_centroid
```

## 2. The chain (this is the picture)

```
        rdh_capture  (byte-identical in DWGLS core/ and ann include/)
              |
   include/anchor_weight.h        RDH -> pattern / letter / weight
              |                   (8 outer slots, 64>16>4>1, 6 dirs distinct,
              |                    52-letter no-repeat via stride-37)
              v  anchor = APEX
   include/anchor_frustum.h       1<4<16<64, 6 faces, capo, TritAddr,
              |                   level-16 -> next major anchor (chains)
              v  identity -> table
   include/anchor_centroid.h      anchor -> K x dim centroid (ann shape)
              |
              v
        ann :8096  anch_load / anch_assign / anch_route / anchor_letter
```

Reused, not duplicated: `rdh_capture`, `fr_route_produce` (`core/frustum_route.h`),
`trit_decompose` (`core/frustum_trit.h`), `anch_slot` MOD-37, `anch_save`/`anch_load`,
and ann's own `anchor_letter.h` (6 letters → 720 orbit) + `AL_WEIGHT` (the owner's
52 character weights, already present as **seed only**, not driving routing).

## 3. Key discoveries this session

- **`fan24_gear.h` already implements the 144²=20736 full field** (ring-24 = KIS 8-tooth
  × hyperbolic 3-tooth, CRT lossless; `fgx_*` full-field variants; `W_full = frame·144 + w_local`,
  gear identity is FRAME-INVARIANT). That is the gear layer the chain can hang from.
- **`core/frustum_route.h` + `test_frustum_descent.c` already prove the 1<4<16<64 descent**
  (D2 capo stable, D4 every branch ∈ `[0,6)`, D7 descent path = tombstone). The frustum
  was NOT rebuilt — `anchor_frustum.h` only seeds it from the RDH anchor.
- **ann already carries `AL_WEIGHT`** with the owner's character weights (a=176, b=69, c=212, …)
  described in-code as "CHARACTER WEIGHTS, 52 letters", exposed as *seed material only*.
  `anchor_weight.h` now derives those weights from RDH instead of a sheet.
- **ann's `anchor_route.h` / `rdh_capture.h` are byte-identical to DWGLS core/** (git hash
  compared) → the headers built here drop into ann without adaptation.

## 4. Honest caveats (do not let these become "facts")

- **`tools/tw_rdh_bench.c` is broken.** `data[i % len]` is an integer division per
  iteration; the `rdh_capture` number it prints (1796 cyc) is that bug, **not** RDH being
  slow (real measured RDH ≈ 5.09 cyc, memory #7544). A correct head-to-head must use a
  fixed `len` (or hoist `%`) and measure tw's parts (`_tw_find_sector`, slot pick, drain)
  separately.
- **The ×4–7 tw-vs-rdh figure is owner-recalled, not verified.** Do not upgrade it to
  measured.
- **RDH was created because tw_capture became the tensor-capture bottleneck on higher
  hardware**, and because single-core wins tiny batches but loses to the GPU at scale
  once `jet_bridge`/`gear_shift`/`gear_lock`/`dramtile` removed the GPU's overhead wall.
  RDH is "as far as this path goes," not a win over the GPU (memory #7551, #7552).
- **RDH was drawn, not designed:** the owner had no hash-function knowledge — he drew the
  Goldberg compound and converted the drawing into a walk (memory #7553). Read every RDH
  claim with that provenance.

## 5. What is NOT done (next session)

- **KV ↔ ann (cache section)** — memory #7527, **idea only, nothing built.** The one
  directly-connectable point is the KV cache *section* (KV = index carrier, ann consumes
  an index → no conversion). Explicitly NOT planned: wiring the DB straight into ann, and
  live-pull from ann during generation (tok/s cost, O(N) exact scan).
- **kineticfan `(a,b)` → anchor table for real** — `anchor_centroid.h` implements the
  converter in principle; it has not been run against a real `kv_anchors.bin` nor used to
  replace a trained table in a live ann query path.
- **Frustum chain end-to-end on real data** — `af_major_anchor_seed` chains anchors in
  unit tests only; no real corpus has been routed through the chain yet.
- **Level-16 major anchor as a real store** — still derived in O(1), never materialised.

## 6. Read these first next session

- `include/anchor_weight.h`, `include/anchor_frustum.h`, `include/anchor_centroid.h` (this session)
- `core/anchor_route.h`, `core/frustum_route.h`, `core/frustum_trit.h`, `core/fan24_gear.h`
- `I:\tools\ann\README.md`, `include/anchor_letter.h`, `GRAPH-ROUTE.md`, `WARM-PATH.md`
- memories **#7525–#7553** (the whole design arc: kineticfan, Quad Pomato, tw→rdh, the three inspirations)
