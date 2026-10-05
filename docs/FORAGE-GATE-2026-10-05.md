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
