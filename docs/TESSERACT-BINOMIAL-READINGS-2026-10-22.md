# Tesseract binomial readings — session audit record (2026-10-22)

**Status:** discussion + audit record. No code was changed; no measurement was run.
Provenance rule used throughout: every number is tagged **[article]** (the pasted
tesseract article), **[owner]** (owner statement), **[external]** (outside-model
claim under audit), or **[verified]** (checked by direct computation/repo read this
session). Interpretations without a count-level map are labeled per field-bridge
rule: *identity, not assembly*.

---

## 0. Starting artifact (owner)

Article: tesseract vertices {0,1}^4 grouped by Hamming weight → **1+4+6+4+1 = 16 = 2^4**.
Owner's question: can this be read as **apex + pyramid base (quadtree) + reshape
hexagonal ↔ cube + …**? Asked as playful analysis ("คิดเล่นดู"), not a build request.

## 1. Layer structure — VERIFIED

- Layers are the binomial coefficients C(4,k); the 5 hyperplanes sum=k are parallel,
  all ⊥ (1,1,1,1). **[verified]**
- k=1: 4 points, all pairwise distance √2 → **regular tetrahedron**. **[verified]**
- k=2: 6 points; each has 4 neighbors at √2 and its complement at 2 →
  **regular octahedron**. **[verified]**
- k=3: antipodal regular tetrahedron. k=0 / k=4: single apex. **[verified]**
- Reading ①: slicing the tesseract ⊥ main diagonal gives vertex counts 1,4,6,4,1 —
  a genuine section-stack pyramid. **Caveat:** the tesseract is NOT a polyhedral
  cone/pyramid (it is a prism over the cube; a cone apex needs degree 15, max degree
  in a 4-cube is 4). "Pyramid" = section-stack + graded poset only.
- Reading ② (parity): even sublattice {k=0,2,4} = 8 vertices = **16-cell**
  (graph = K8 minus perfect matching; edge-transitive; 32 triangular faces) =
  **bipyramid over the k=2 octahedron** (apex 0000/1111, equator = k=2).
  Odd sublattice {k=1,3} = the other 16-cell = two dual tetrahedra (stella-octangula
  pattern). **[verified]**
- Vertex figure (both, stated precisely):
  - of the **tesseract** at any vertex = tetrahedron (4 neighbors);
  - of the **inscribed 16-cell** at apex 0000 = octahedron (6 neighbors = the k=2
    layer, up to homothety half-scale around the apex).

## 2. Quadtree reading — set-true, tree-different

- 16 = 4² via bit regroup {0,1}^4 → {0..3}² (2+2 split): a bijection; the 16 leaves
  CAN be organized as a depth-2 quadtree. **[verified]**
- But the two trees over the same 16 leaves differ: binomial pyramid branches
  (4,3,2,1), quadtree branches (4,4). Not the same structure — groupings only.
  Digit regroup reverses with zero stored state (quotient/low-digit pattern,
  cf. #7078); it is NOT an affine-modular chain (#7068) — do not merge the theorems.
- Repo quadtree ladder is owner's own: 1 < 4 < 16 < 64 = 4^0..4^3, 2×2 subdivision
  per layer (#7535).

## 3. Hexagonal ↔ cube reshape — classical + repo bridges

- Classical: cube projected along body diagonal → regular hexagon (this is the
  isometric projection); octahedron along its 3-fold axis → hexagon; cube ↔
  octahedron dual. Chain: hexagon ↔ cube ↔ octahedron(k=2) ↔ 16-cell(1+6+1).
  **[verified]**
- Repo bridges already exist:
  - `core/isometric_map.h:268` `im_project` cube(x,y,z) → isometric screen;
  - `core/geo_a2_symmetry.h:31` "144 is the intersection of hexagonal and cubic
    symmetry"; A2 = 6 roots, base mod 12 (`A2_ORDER=6`, `A2_BASE=12`);
  - `core/geo_octant.h:10` zero-sum `i+j+k ∈ {0,1}` binary fold →
    `core/geo_tesseract_dense.h:21` `TD_VALID_CUBES=4` (store 4 valid cubes,
    derive 4 antipodal — same fold/unbake pattern as the parity split, but at
    **cube level 4/8, not vertex level 8/16** — different partitions, do not merge);
  - `core/tri_hex_tess.h:19` hexagon ring; 12-gon stride classes (dodecagon →
    hexagon → 3 squares → triangles).
- Numerical observation **[interpretation, no consumer]**: 20736 = 12^4 = (2·6)^4
  — the "2" (binary/parity fold) and "6" (hexagonal A2 roots / 6 directions) of the
  binomial middle coincide with field factors. Candidate only (#7129).

## 4. External model answer #1 audit (5+6+5)

Claim: {k≤1} ∪ {k=2} ∪ {k≥3} = 5 + 6 + 5 = "two 4D simplices bonded by an
octahedron".

| Part | Verdict |
|---|---|
| {0000} ∪ {k=1} is affinely independent → genuine 4-simplex (both sides) | **true** [verified] |
| shapes (tetra / octa / antipodal tetra), parallel hyperplanes | **true** |
| "valid ในแง่ vertex set" hedge | technically true but **vacuous** — any grouping is |
| "สอง simplex ประกบกัน" (tiling reading) | **rejected**: hulls disjoint (sum ≤ 1 / = 2 / ≥ 3), together ≈ 8.3% of the tesseract volume (2×1/24 + a measure-zero slice); 91.7% empty |
| Δ0 is a face/subcomplex of the boundary | **rejected**: Δ0 edge (1000)–(0100) has Hamming 2 = diagonal of square face {0000,1000,0100,1100}, not a tesseract edge (Hamming 1) |
| 8+8 "16-cell + 16-cell" | valid as **vertex partition only**; hull(even 16-cell) ≠ tesseract — proof: (1,0,0,0) ∉ conv(even) (coefficients of every even point with coord2=1 must vanish → only 0000 remains). hull(both) = tesseract. |
| "octahedron = vertex figure of the inscribed 16-cell" | **true and sharp** — neighbors of 0000 in the even 16-cell = exactly the 6 weight-2 vertices |

Terminology decided: say **vertex partition / vertex grouping**, never
"decomposition", unless volumetric tiling is meant (it is not).

## 5. The layer-2 question (binomial 6 vs quadtree 16)

- Counting fact: binomial level 2 = C(4,2) = 6 = unordered distinct pairs;
  quadtree level 2 = 4² = 16 = ordered pairs with repetition. **[verified]**
- First framing (rejected, over-claim, owned this session): "same plane, same
  level, two kinds of residents" — binomial layers are hyperplanes; quadtree levels
  are node counts; they are not one plane, and the repo never has a 6-population at
  a tree level.
- Settled framing per `experiments/field-bridge-2026-10-08/README.md:57-59`
  (`cube_cover_probe.c`): "`1<4<16<64` is a 4-ary (quadtree) ladder; frustum =
  **same tree entered by 6 directions** (rule identity, no assembly); one field has
  more than one nested decomposition."
  - **16 = node count** of the 4-ary ladder at level 2 (storage/space side);
  - **6 = entry/walk direction rule** — Wang 6, face digit `%6` (#7348), A2 roots
    — an **edge/step label, not a node population** (routing side);
  - both live on the same 20736 field; **no code joins them**:
    `wl_id(cell,choice)` and `tess_flat(tess,cell,slot)` are observation-only
    (README:47-49). Doctrine #6365 (routing emits paths, storage owns bytes)
    holds as **rule vs node / observation vs assembly**, not level vs level.

## 6. External model answer #2 audit (6×8=48, B_4, 128×162)

### 6.1 True (verified)

- All arithmetic: 6×8=48, 48=12×4, 48=16×3, 16=2⁴=8×2, 12⁴=9·48²=(3·48)²=144²,
  128×162=20736=144², 64×81=72×72=8²·9²=5184, 16×9=12×12=4·3·4·3=144, 4×3=12,
  128=2·4³, 162=2·3⁴, |B_4|=2⁴·4!=384=8×48, 192=4×48. Owner's reduction table
  `[external]` correction of `12=2×2×3×4` (→48) is right.
- **Flag count**: cube flags (v<e<f) = 8×3×2 = 48 = |O_h| via simple transitivity
  — the one genuine theorem-grade statement of that round.
- Geometric-mean identity b²=ac is trivial for any triple; the specialness is only
  in the value: 144/128 = 162/144 = 9/8.

### 6.2 Overclaim / wrong (external) — with proofs

- **"6×8 = 48 = |O_h| is a duality product, not coincidence"** — false as a
  theorem: faces×vertices of the n-cube = 2n·2ⁿ vs |B_n| = n!·2ⁿ; equal iff
  2n = n! iff **n = 3 only** (counterexample n=4: 8×16 = 128 ≠ 384). The equality
  is the accident 2·3 = 3!, one dimension wide. The flag-count version stands.
- **"48 = 24 × 2 (24 face × 2 sides)"** — cube has 6 faces (24 = 4!, which the
  same line admits). Salvage exists: 24 = the tesseract's square faces × 2 cells
  per face = 48 = cell-face incidences = 8×6 **[verified]** — a real count, if that
  was the intent; "two sides of a face" has no referent.
- **Flip on 3** (first "ตัวหลุดวง", then "cell dimension"): both recoverable —
  every 3 in this story traces to one fact (the cube is 3-dimensional: edges/vertex
  = 3, cell dim = 3, 3! axes, zero-sum over 3 axes #6511) → 3 HAS a referent; but
  referent ≠ assembly for any given product: `48 = 16×3` and `6×8×3 = 144` have no
  natural family in the tesseract (natural sizes: 8, 16, 24, 32, 48, 64, 96 — 144
  is not among them) → per the field-bridge README rule: identity, not assembly.
- **"128 = 2×4³ = 2×(ambient dim)³, 162 = 2×(cell dim)⁴"** — arithmetic true,
  but it is a NEW interpretation colliding with owner-established provenance
  #6497: 128 = base-2 (compute side, 2⁷), 162 = base-3 (ico vertices, 2·3⁴).
  Label as candidate (#7129). Cleaner alternative already grounded in #6497:
  **128 = 2·8², 162 = 2·9² → √(128·162) = 2·8·9 = 144 with ratio 8/9 = the
  dual-lane constant of #6497** (Lane A 144×128 = 8/9) — no ambient-cubed needed.
- **9/8 as a musical tie** — disproof in this system's own temperament:
  scale_bridge (1 tooth = ×2^(1/12), ring 144, W=12) has whole tone
  **2^(1/6) = 1.122462 ≠ 9/8 = 1.125**; 12·log2(9/8) = 2.0391 semitones → 9/8 sits
  **0.039 semitone** off the 2-tooth — it never lands on a gear tooth.
- **"ทุกตัวเป็น invariant ของ B_4"** — 6 and 8 are cube/B_3-level counts; the
  |O_h| tie is the n=3 accident above. The B_4 list itself (16/24/32/384/192) ✓.

### 6.3 Proposed test (external) — weak + already run

Proposed: "bijective map tag-space ↔ bucket-space on a subset would prove B_4
structure (not just 79.1% random conflict)".

1. Partial bijections are nearly always possible → not decisive;
2. even a full bijection measures label correlation, not group structure
   (#949: labels = semantics, MAP not semantics);
3. **the measurement exists**: `I:\tools\ann\docs\HANDOFF-2026-10-22-ann-scope-loader.md:80`
   — "the semantic tag is **orthogonal** to the geometric anchor buckets — a tag's
   rows spread over all 8 buckets (top-bucket share 0.22–0.50), the baked tag→bucket
   accuracy is **0.479**, and it conflicts with the row's own stamp **79.1%** of the
   time. So a tag LUT must **not** claim an anchor bucket as its zone."

   → 79.1% is the conflict rate that produced a **negative verdict** (keep tag and
   bucket separate), not a random baseline to beat. The external text used the
   number with inverted polarity and framed an already-closed test as open.
4. The criterion worth measuring is the external text's own better one —
   *"shared address, walk once, no translation"* = commutation of tag→address and
   bucket→address paths on shared sids — which is the field-bridge task, a
   different job from tag/bucket alignment.

## 7. Field bridge — status of the whole numeric family

`experiments/field-bridge-2026-10-08/README.md` (read this session):

- Status: **NOT WIRED. Concept only.** Parked; not in `make test`.
- Equation: `12 × 6 × 8 × 6 × 6 = 20736 = 12^4`, reconciled with `18 × 8 × 144`
  because 18 = 3×6 and 144 = 4×6×6; every factor is a real header constant
  (12 = `GEO_PENTAGONS`, 6 = `FACE_COUNT` = `CUBE_HALF_AXES`, 8 = `OCT_CUBES`,
  last two 6s = `tess_adjacent` degree).
- Boundaries (README:42-50): not cube assembly (`6 == 6` is a direction/face
  **rule identity**, not an assembly step); not a join (`wl_id` vs `tess_flat` —
  no code joins them); not compression (MAP, not COMPRESS).
- Revisit question (README:64-69): does making the identity explicit buy a shared
  address / one walk / no translation, or is it arithmetic coincidence? —
  "Answer with a measurement, not a diagram." Open as task #4:
  `docs/ANN-SYSTEM-REPORT-2026-10-08.md:158`,
  `docs/HANDOFF-2026-10-08-ann-tombstone.md:81`.

Consequence: most of the 6×8=48 / 12⁴ round was a re-derivation of this README;
the genuinely new claims were the |O_h|/B_4 framing, the 128 ambient³ reading, and
the 9/8 tone — each disposed in §6.

## 8. Errors owned this session (self-audit)

1. Example edge (0000,(1100)) is not an edge of Δ0 (weight-2 vertex not in Δ0).
   Correct example: (1000)–(0100), both in Δ0, Hamming 2 = face diagonal →
   conclusion (Δ0 not a subcomplex) unchanged.
2. "ระนาบเดียวกัน/ชั้นเดียวกัน" over-claim: binomial layers are hyperplanes,
   quadtree levels are node counts — not one plane.
3. Vertex-figure footnote omitted which polytope — both cases now stated (§1).
4. (self-found while answering §5) "two residents of one level" over-claim —
   6 is the walk direction rule, 16 the node count; corrected to rule-vs-node per
   the field-bridge README.

## 9. Status board

| Statement | Status |
|---|---|
| 1+4+6+4+1 layers, shapes, parallel hyperplanes | **proved** (computed) |
| parity split 16-cell bipyramid / two tetrahedra | **proved** |
| 5+6+5 as vertex grouping | true; as tiling — **rejected** (8.3%) |
| Δ0/Δ1 subcomplex of boundary | **rejected** (face-diagonal edges) |
| quadtree regroup of the 16 leaves | true (set-level), tree differs |
| hexagon↔cube↔octahedron chain + repo bridges | classical true; anchors cited |
| layer-2: 16 = nodes, 6 = entry direction rule | **settled** (README:57-59), no join |
| 6×8 = |O_h| as duality theorem | **rejected** (n=3 accident 2·3=3!) |
| flag count 48 = |O_h| | **proved** (flag-transitivity) |
| 3 = cell dimension | referent true; products `48=16×3`, `6×8×3=144` = identity, not assembly |
| 128 = 2·4³ / ambient³ reading | candidate; conflicts with #6497 provenance (2⁷ / 8·9 dual-lane) |
| 9/8 on the gear ring | **rejected** (0.039 semitone off 2^(1/6) tooth) |
| tag ↔ bucket shared structure | **closed-negative** (handoff 2026-10-22) |
| field bridge "one walk, no translation" | **open** — measurement, not diagram (task #4) |

## 10. If work continues

- The only measurement-grade open question from this session: field-bridge task #4
  — shared-address commutation ("walk once, no translation"), on sid pairs, as a
  path-equivalence check. Not label-set bijection.
- Everything else above stays at reading/audit level. No numbers from this session
  may enter a header constant or formula without the #7129 provenance statement.
