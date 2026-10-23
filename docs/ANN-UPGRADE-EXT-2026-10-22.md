# ANN upgrade — extension analysis (2026-10-22)

Companion to `docs/ANN-UPGRADE-PLAN-2026-10-22.md`.
Inputs analysed: `I:\dsk01.txt` (parsed chat), `experiments/beam-revival-2026-09-30/REPORT.md`,
`core/mm_wang6.h`, `core/geo_lblock6.h`, `core/isometric_map.h`, `core/frustum_trit.h`,
`I:/tools/ann/src/ann_search.c`, `I:/tools/ann/include/geo_id_route_log.h`.

Status: **analysis only, nothing implemented**. Topic = ANN search layer.

## Scope guard (read this first)

IN scope: `I:/tools/ann` route/latch/note layer + the DWGLS address primitives it vendors.

OUT of scope — do not mix in, they are separate and parked:

| Topic | Why out |
|---|---|
| model weight pipeline (GGUF / tess / breathing_fs) | different payload layer; ANN never touches weights |
| hourglass fold / signed gates / entropy probes | payload-side (weights); see `experiments/apex-hidden-2026-09-29/` |
| beam entropy / folding / compression campaign | payload-side; only its **address** findings port (Wang, lblock) |
| KV × ANN separation work | parked (#7587); ANN must not carry KV vocabulary |
| re-anchor centroid table onto 3 axes | phase 3, parked (bucket id = centroid index, no spatial meaning) |

## Where the parked work actually helps (and where it does not)

The beam report's own conclusion is the filter:

> "Value never lost; the LINK dies." — cross-boundary needs the same addressing rules,
> otherwise re-anchor at the wang gate with a **rule tag**; park, do not enter the wrong home.

> "Binding theorem": restoring an arbitrary order costs ~N·log2(N/e) bits.
> Folding buys organization + verifiability, never fewer bytes.

So: **nothing about folding/entropy/compression ports**. What ports is exactly the
*address/topology* half — and that half is the half the ANN upgrade needs.

The parsed chat contributes four ideas that are structurally address-layer:

1. "Same points, new lines; where the lines overlap is a **door** between the two shelves."
2. "Points have no fixed 2D/3D coordinate — they can cross dimensions without colliding."
3. "The original order is the expensive part; shuffle + regrow is the reverse of the randomness I started with."
4. "Human memory is associative: a word warps you back; AI is sequential."

Each maps to a candidate below.

## Candidates (ranked by decision value per cost)

### N1 — Second route: maze fallback so a dead link never kills the search  [HIGH / medium]

Two topologies already live on the same cell space:

| | topology | depends on | status |
|---|---|---|---|
| A | centroid → bucket → order/stamp (semantic) | table + sidecar + order | today's path; the one that dies silently (H1) |
| B | `cell = bucket % 144` → wang6 6-dir maze → lblock6 Hilbert walk (geometric) | cell id only | `core/mm_wang6.h` 2700/2700, `core/geo_lblock6.h` 511/511 green, but unused in ANN |

Today the latch uses topology B's cell space one-dimensionally and the search never walks it.
Because B does not read the stamp sidecar at all, it is an **independent fallback**: if A is
dead, B still yields candidate buckets → the search degrades, it does not silently miss.

This is the chat's "overlap of two line-sets = door": the portal set is
`{cell : semantic neighbours and wang6 neighbours agree}`, which is measurable.

Gate (`tests/` style, run on the real 62,683-row index):
- baseline: recall@10 0.9617, recall@5 0.910 (300 queries)
- corrupt/truncate the stamp sidecar exactly as in "dead route" + "append window"
- today: `routed:1` with collapsed recall (observed twice) — record the number
- new: recall ≥ baseline − 0.02, latency bounded, and `/health` reports `degraded` not `ok`

Cost: a cell→bucket neighbour index (derived, not stored). No index change, no retrain.

### N2 — Rule tag on cross-boundary writes (kills the silent seam)  [HIGH / low]

The beam finding names the remedy: at a boundary, carry the rule, and **park rather than
enter the wrong home**. The ANN stamp write is precisely an untagged cross-boundary write:
8 B/row `(anchor, anchor2)` positioned by row index, produced under a particular table.

Today's only guard is H2 (order must carry K/ntr/checksum of the table, `ann_search.c:1073`),
which does **not** catch a stale stamp whose ntr still matches.

Proposal: each stamp batch carries `(table_id, K, ntr, n_rows, crc)`. Reader checks; mismatch →
rows are *parked* (unmapped, reported), never routed by position.

Gate: build stamp under table v1, swap in table v2 with identical ntr →
today silently routes wrong subsets; new refuses and reports `unmapped: N` with the tag diff.

Cost: 8 B/row → 12 B/row (or a one-line batch header). No index change.

### N3 — Measure whether the stored order is actually derivable  [DECISIVE / very low]

The user's framing: "my shuffle and regrow is the reverse of the randomness the model started with."
Made precise for ANN: the global order is derivable **iff** the stored order is a pure function of
the assignment (`anch_perm`: bucket-major + MOD-37). If it preserves arrival order *inside* buckets,
that intra-bucket permutation is arbitrary and costs bits (binding theorem), so "regenerate" must
keep a small residual or accept a different-but-correct order.

Gate (pure measurement, decides phase-1 cost):
1. recompute order from `(rows, table)` and byte-compare to `ann_index.order` → exact / bucket-level / mismatch
2. measure intra-bucket arrival-order entropy in bits/row
3. verdict: `derivable_free` or `derivable_with_residual(bits/row)`

This is the number behind "เก็บการเดิน ไม่เก็บแผนที่". It either proves phase 1 free or prices it honestly.

### N4 — Tamper states on wang6 gates (localize instead of silent pass)  [MEDIUM / low]

Beam open thread: "tamper states for wang6". In ANN a corrupted cell colour passes when it
happens to match. Adding `TAMPER/BREAK` states makes the note record a *gate state change* as an
event, so corruption is localized to a cell/face and contained: unaffected buckets keep routing.

Gate: inject N corruptions into the walk log → detect all, name cell/face, prove untouched
buckets still route; no recall change on clean data.

### N5 — Pair-symmetric latch: store 6 of 12 faces, derive the rest  [MEDIUM / needs evidence]

`core/isometric_map.h` carries **two** involutions: sign flip inside a ring
(`IM_FACE_TABLE:88-99`, faces 0-5 vs 3-8) and the Metatron cross ring swap
(`IM_CROSS:106-109`, `im_face_partner`). If shut/open state is symmetric under either,
half the latch is derivable.

Do **not** assume it. Gate: replay the real route log, count `shut(partner(f)) != shut(f)`
violations. ~0 → halving safe; otherwise drop the idea.

### N6 — Scope→latch affinity ("one word warps back")  [MEDIUM / low]

Chat idea 4 in code: ANN already has scope (tag/zone, scoped path bypasses route).
Keep a per-scope bucket hit count (or per-scope latch) so a repeated scope pre-warms the buckets
it walked last time. Uses the existing `g_warm_flag` machinery (`ann_search.c:573`).

Why it fits "jump once on the second round": the first pass pays the walk; the second pass reads memory.

Gate: repeat-scope queries only → latency delta and warm-hit rate; recall must not move.
Caveat measured in the climate campaign: stale *reuse* is what dies (−0.136 recall on E5 stale-1),
so this must pre-warm, never skip scoring on the strength of the memory alone.

## Recommended order

1. **N3** — pure measurement, decides whether phase 1 is free. One day.
2. **N2** — small, kills the seam that produces the silent unroute.
3. **N1** — the real capability win (second independent route).
4. **N6** — cheap, visible, matches "jump on round two".
5. **N4**, then **N5** (both need evidence first).

Anything that needs the four open owner decisions (below) waits.

## Open owner decisions (asked 2026-10-22, answers pending)

1. If note and sidecar disagree → **regenerate** or **refuse**? (recommendation: refuse + alert, fail loud)
2. Note layout: one file for the whole index, or segmented per slot?
3. Is 32 B/record acceptable? (the fibo path used 5 B/route; stamp is 8 B/row)
4. Phase 2: geometry only, or also a live isometric view?

## Files this analysis touches (read-only)

- `I:/tools/ann/src/ann_search.c` (latch `:588-591`, warm flag `:573`, cache key `:1060-1063`, H2 `:1073`, scoped bypass)
- `I:/tools/ann/include/geo_id_route_log.h` (32 B record, CRC32, replay)
- `I:/tools/ann/docs/HANDOFF-2026-10-21-ann-tree-side.md` (3 layers, dead route / append window)
- `core/mm_wang6.h`, `core/geo_lblock6.h`, `core/isometric_map.h`, `core/frustum_trit.h`
- `experiments/beam-revival-2026-09-30/REPORT.md` (binding theorem, "value never lost; the LINK dies", tamper states open)

## 2026-10-22 late update — Option B ผ่านเงื่อนไข + audit ข้ออ้าง "กลืนกิน"

### เรื่องที่ cleared
- Owner: "10M rows ไม่จะเกิด เพราะจะโดนกลืนกินก่อน" → scale objection ของ Option B ถูกยกเลิกตามคำตัดสิน owner
- ทิศทางเลือก: **Option B** (ลบ sidecar/order ออก; boot = replay note + คำนวณ assignment ใหม่) — gate ที่เหลืออันเดียว: วัด **cold-boot time จริง** จาก Phase 0 canary

### Growth assumption (ต้องเขียนชัด — โค้ดวันนี้เป็น append-only)
- โค้ดวันนี้: ann_index.jsonl append-only, ไม่มี merge/dedup/compact ของ vector rows (owner doctrine #7565: closed clusters, never edit/delete/rewrite)
- Boot recompute = linear กับ rows (arithmetic, ยังไม่ได้วัด): 62k ≈ ~tens ms · 1M ≈ 0.3–1.5 s · 10M ≈ 5–30 s → วัดจริงที่ Phase 0 เป็น gate
- Repo-native "กลืนกิน" ที่ไม่ลบอะไร = #7166: same-category new data may LAND same point in different dims, lookup เห็นทั้งคู่, **scoring merge ทีหลัง, nothing deleted** → cap ที่ view ที่ถูกเสิร์ฟ ไม่ cap raw rows

### Claims จาก agent ตัวอื่นที่ตรวจแล้วไม่จริง (ห้ามสืบทอดเข้า session หน้า)
1. "Venom dedup: vector ใหม่คล้ายเก่ากลืนเข้า anchor เดียว" — ไม่มีในโค้ด ANN; ขัด #7565 (closed clusters)
2. "Ghost Lift ผลักข้อมูลไป Residual Space" — geo_ghost_lift.h = ฝั่ง KV/ghost placement; no ANN consumer
3. "จำกัดด้วย Capacity ของ Field (20736)" — 20736 = slots ของ DWGLS field ไม่ใช่ row cap (ANN มีอยู่แล้ว 62,683)
4. "ขับถ่าย cold → Ghost/Residual" — ไม่มี mechanism; kv_cold = parked split (#7587)
5. "V2 ไม่มีข้อเสียเลย" — ข้อเสียจริงที่เหลือ: boot linear กับ rows + ตัด hot cache

### สรุปสถานะ
Option B = selected (gated on boot-time measurement). เงื่อนไขผูกพัน 2 ข้อ: (1) boot-time gate จาก canary, (2) growth assumption ที่บันทึกไว้นี้ — ถ้า "กลืนกิน" จะเกิดจริง ต้องออกแบบตาม #7166 (score-merge, nothing deleted) คือห้าม merge vectors ใน storage
