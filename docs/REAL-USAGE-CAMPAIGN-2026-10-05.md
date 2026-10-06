# REAL-USAGE CAMPAIGN — 2026-10-05

Ordered test plan closed: prove the geometry against **real artifacts**, not
hand-computed fixtures. Three items, each with a memcmp/hit oracle. All three
now have receipts on real data. This document is the campaign record; the
per-item code receipts live in the tools named below.

Everything below is **MAP not COMPRESS** territory: no compression claim is
made anywhere, and no result here depends on reading byte *meaning*. The oracles
are memcmp, hit-rate against ground truth, and byte equality.

---

## Item 1 — addresses resolve to source bytes ✅

**Question:** does a field address point at the bytes the source GGUF holds?

**Tool:** `tools/verify_field.c` (`make verify-field`).
Open the field, read `fpos[]`, read the bytes at the address, open the source
GGUF, fetch the same tensor by name, memcmp every tensor.

**Result (live bake then verify, `dual_lazy_serve` 23/23 PASS):**

| field | source GGUF | tensors | memcmp |
|---|---|---|---|
| `build/fieldA.bin` | Qwen2.5-0.5B-Instruct-Q8_0 | 291 | **291/291 PASS** (638.7 MB) |
| `build/fieldB.bin` | Qwen3-0.6B-Q8_0 | 310 | **310/310 PASS** (604.1 MB) |

**Finding:** a field v2 is **raw bytes placed at addresses** — there is no delta
encoding, so a direct memcmp is a legitimate oracle. The field is
self-describing: the address lives inside the field, and addressing needs no
source GGUF.

**Two bugs found and fixed (both in the probe, not the field):**

| # | bug | symptom | fix |
|---|---|---|---|
| 1 | `arr_ptr` was a pointer into the reused `infos[]` array, stored at parse time | every `fpos` read 0 | store an **offset**, add `base` at read time; `fk[96]` → `fk[128]` |
| 2 | msys-gcc silently rejects `%llu` | printf printed garbage (looked like 0) | `%I64u` |

**Limit:** `field.bin` (v0, 2.4 GB) has no `fpos[]` — it was baked before v2 and
cannot serve addresses. Re-bake required. v2 fields only.

---

## Item 2 — fog/pattern on real tensors ✅

**Question:** does the fog/pattern machinery run on real model bytes, not
synthetic hand-computed values?

**Tool:** `tools/frustum_real.c` (`make frustum-real`).
Each real tensor is an anchor; walk the 6 wang directions; sample real bytes at
6 canonical positions inside the tensor; form the 6-bit face mask (tombstone =
entry+exit); verify every sampled byte against the source GGUF.

**Result:**

| field | anchors | face bytes | differ | verdict |
|---|---|---|---|---|
| `fieldA.bin` | 64 | 384 | 0 | 6/6 PASS |
| `fieldB.bin` | 80 | 480 | 0 | 6/6 PASS |

- **R2:** the walk reads real bytes from the model — 864 samples, 0 differ.
- **R3:** two walkers diverge on real data — `ffn` family fog `0x39`, `attn`
  family `0x3f`, so distinct walk paths produce distinct signatures.

**Structural receipt (fell out for free):** `output.weight` and
`token_embd.weight` carry **byte-identical data** — the field really does hold
tied weights — yet they are separate anchors and still separate by fog.

**Bug found and fixed:** my own `build_order` sorted `fpos[]` by chain order.
`fpos[]` is stored in **file-index order**; the correct formula is
`address(t) = body_off + fpos[t]` with `t` = entry index, no sort anywhere.
`verify_field.c` (291/291) was the tie-breaker that settled it. Same fix landed
in `tools/field_address.c` (commit `70ec861`).

---

## Item 3 — SIFT1M forage loop on 1M real vectors ✅

**Question:** the probe's own note said "real-data SIFT leg is a follow-up";
does the forage loop actually run on 1M real vectors and beat brute?

**Assets (all present, contrary to an earlier assumption):**
`build/sift1m/sift/` holds `sift_base.fvecs` (1M×128), `sift_query.fvecs`
(10k×128), `sift_groundtruth.ivecs`; `build/sift1m_hier/` holds `C1.npy`
(256×25), `fine_cent.npz` (c000..c255, 10×25 each), `lab1.npy`, `pca_*`,
`meta.json`. Nothing had to be regenerated.

**Tool:** `tools/forage_real.c` (`make forage-real`), artifacts via
`tools/sift1m_prep.py`.

**The doctrine this implements.** The importer does **not** place a row into a
slot. Every DB vector **enters the entrance and walks top→down** — PCA-25 →
nearest coarse (256) → nearest fine (10) → nearest L3 sub-leaf (4) — and the
route it walked is what shuts the wang latch (footprint). This is the owner's
rule: it is the *same structure as before, but the data must walk in from the
entrance, not be dumped in place*. It is not a batch operation; batch had nothing
to do with it.

**Not using the pre-computed assignment.** A walk-derived coarse agrees with
`lab1` **100%**, but `fine_members` col1 agrees with a real walk only **9.96%** —
it is a different artifact. So the importer walks for itself and reads neither.
Verified on vector 0: walk says coarse 59 (= lab1), `fine_members` says 0.

**Receipts (real sift1m, 300 queries unless noted, GT = sift_groundtruth):**

| layers | config | scan | recall@1 | reference |
|---|---|---|---|---|
| 2 (coarse→fine) | topb=16 topk=8 | 52,703 | 99.0% | ceiling before L3 |
| 3 (+L3) | topb=16 topk=6 | 40,645 | **99.5%** | **ceiling lifted** |
| 3 | topb=8 topk=4 | 13,960 | 88.5% | baseline 8x32: 13,887 @ 89.1% |
| 3 | topb=8 topk=2 | 3,601 | **71.0%** | baseline 4x8: 3,535 @ **65.6%** (+5.4pt) |

`meta.json` baselines are sklearn KMeans assignments on the same data.

**Two findings that matter:**

1. **The extra fine layer pays in both directions** — higher recall at *lower*
   scan (99.5% @ 40.6k vs 99.0% @ 52.7k). The baseline tops out at 89.1%
   because it only has two layers; walking three goes past what it can reach.
2. **Walking from the entrance matches the structure it replaces.** At matched
   scan (~13.9k) the walk gets 88.5% against the baseline's 89.1% — 0.6 points,
   with no pre-computed assignment used at all.

**Import cost:** 1M vectors walk top→down in **16.5 s** (1M × (256+10+4) L2
evaluations). This is the price of walking rather than placing — the earlier
0.04 s figure was the direct-place version that the doctrine rejects.

---

## Honest status after this campaign

| piece | state |
|---|---|
| field address ↔ source bytes (v2) | **proven** (601/601 memcmp) |
| fog/pattern on real tensors | **proven** (6/6 ×2 models, 864 bytes) |
| forage loop on 1M real vectors | **proven** (recall/scan vs GT) |
| field v0 (`field.bin`) addressing | **impossible** — no `fpos[]`, re-bake needed |
| fog/pattern bound to a real `anchor_route` | **done** — forage routes through `anch_route()`; `anch_slot == lk_id37` (R9) |
| 3 layer shapes (frustum composite vs flat) | **done** — coarse anchor carries a 6-bit face mask from the fine directions walked (R10) |

## Follow-up (2026-10-05, same day) — routing + stride-37 in the memory DB

After the three items closed, two more things were bound and measured.

**Route through the real router.** `forage_real`'s query path no longer hand-sorts
the coarse centroids; it calls `anch_route()` from `core/anchor_route.h` (needs a
float mirror `C1f` of the double table). Recall is unchanged — **88.5% @ scan
13,960**, equal to the sklearn 8x32 baseline.

**fog warm-read (R8).** Train and query walk the *same* entrance rule, so a
re-visited route reads a latch that is already shut: **64/64 first-visit open,
64/64 second-visit shut**. This is the payoff of "enter by walking, do not
place" — the second visit is warm with no stored branch list.

**stride-37 in the memory DB (R7, R9).** The owner asked whether the tensor-field
`(i*37)%20736` helix is useful here. Measured answer: **yes, as identity, not as
storage order.**

| property | measurement |
|---|---|
| bijection over the real leaf count N=10240 | yes (37 coprime with 20736/10240/2560/1728/144) |
| inverse | O(1) per N via extended Euclid — 16813 is the inverse **mod 20736 only**, so it must be computed per N |
| id step | exactly +37 mod N → uniform pool walk, no clustering |
| `anch_slot(i,256) == lk_id37(i,256)` | 1 — one helix shared by the tensor field **and** the memory DB |
| locality | 64 consecutive indices span 2331 slots; 576 span the whole 20720 → **must not** be used for payload/postings read in sequence |

So a leaf/anchor id is a *rule*, derived from `(c,k,k3)`, needing no stored map and
identical every session — while the posting storage order stays local.

**L3 as a frustum composite (R10).** The mini-kmeans split is replaced by the
6-direction frustum: descending through a fine leaf clears *that fine's* face
(`fine%6`), so a coarse anchor's pattern is a 6-bit mask of the directions it was
entered through — the tombstone of who passed, not one slot. On the real 1M set:
**lit = 251/256 anchors, distinct_patterns = 24.**

## Bugs found across the campaign (all mine, all fixed)

1. `arr_ptr` dangling pointer into `infos[]` → every `fpos` read 0.
2. msys-gcc `%llu` → printf silently wrong.
3. `build_order` sorted `fpos[]` by chain order → `field_address` printed the
   wrong tensor's address.
4. `forage_real` used `mm[i]` (fine_members row *i*) where `lab[i]` indexed the
   same row — a mismatched-row read that produced recall 0.00.
5. `npy_load` read the header length from the wrong bytes (10–11 instead of
   8–9), yielding `hlen=10107`.
6. `lk_inv37` hard-coded 16813, which inverts 37 mod **20736**, not mod the leaf
   count — every inverse check failed until the inverse was computed per N.
7. The float-mirror bug: `anch_route` takes `const float *`, and passing the
   `double` centroid table produced recall 4% until `C1f` was added.

## Reproduce

```
make verify-field && ./build/verify_field build/fieldA.bin <Qwen2.5-0.5B.gguf>
make frustum-real && ./build/frustum_real build/fieldA.bin <Qwen2.5-0.5B.gguf>
python tools/sift1m_prep.py build/sift1m_hier build/sift1m/sift
make forage-real && ./build/forage_real build/sift1m_hier build/sift1m/sift 300 8 4
```
