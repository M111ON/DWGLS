# HANDOFF — 2026-10-23 (session 2) — ann sanitizer + hourglass pair (pointer)

Full handoff: `I:/tools/ann/docs/HANDOFF-2026-10-23-ann-sanitizer-hourglass.md`
Receipts appended to: `docs/ANN-UPGRADE-2026-10-23-PHASE0.md` (§ "Session 2")

One-line state: the sanitizer 503 class is closed with no-regression receipts
(ann `bd1ce04`, unpushed at handoff), and the hourglass/frustum-pair shift algebra
is pinned in `tests/test_addr_orbit.c` T12–T15 (35/35 ALL PASS) — opposite pair is
point reflection `v → −v` ⇒ crossing the shared centre = **1 XOR**, composes to
`d ^ (k&1)`.

Next (ordered):
1. push `bd1ce04` (`I:/tools/ann` main → origin/main).
2. relayout the ann latch, `src/ann_search.c:622` `bucket % 144` → `(pair << 1) | end`,
   gate = no recall change on the 300q oracle + bucket occupancy unchanged.
3. G2.5 receipt: closing `+X` must not close `−X` (`tools/pattern_probe.c:163`).
4. decide per-anchor ids 72 vs 144 and whether the 1-bit fingerprint stays
   (pool: 5,440 anchors; 6 faces & anchor = 32,640 > 10,368 pool; 1 bit & anchor = 5,440).

Do not re-open: the ladder `2^a × 3^b`, `face = d>>1 / orient = d&1`, and the
reserved apex ids `{0, 10367}` are already locked in the same oracle file.
