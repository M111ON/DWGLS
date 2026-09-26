# geo_jump — Origin, Residual, and Proof (2026-09-26)

## Origin (owner)

geo_jump was designed from **Metatron's cube** by this construction:

1. Project Metatron's cube onto **3 viewports**.
2. Mark every circle-center; **dedup overlaps** (count once).
3. Result: spheres in **4×4 stacked 3 layers = 48**, whose silhouette is a
   **hexagon**.
4. Top 4×4 uses **Hilbert as maze walls**; vertical 3 layers use **Peano
   woven with Hilbert** — the weave carries orientation + placement
   direction, so a placement never lands rotated wrong.

Code footprint: `sid/geo_jump.h` (`_jump_hilbert` + `_jump_peano`),
`collection/beam_addressing/beam_*.c` (`GEO_BLOCK=48`, `GEO_TOWER=144`).

## The hidden +16 (residual)

Spheres occluded behind others in the projection are **not visible but not
gone** — they are the residual: a hidden data store. This is why Metatron
can reshape into other forms without breaking: projection never deletes,
it only moves points between visible and hidden.

Number footprints in code (no single place states the equation — the
equation is the owner's synthesis, consistent everywhere):

| Where | Value |
|---|---|
| `GEO_BLOCK` (3 floors × 16 cells) | 48 visible |
| `GEO_METATRON_CELLS` per floor, `resid(16)` bytes in zone card | 16 |
| `GEO_DNA_MAX_STEPS`, MOD walk `lcm(64,54)` | 64 = 48 + 16 |
| `lcm(64,54)` = 1728, × 12 orbits = 20736 | closes the field |

Baked as doctrine in `core/geo_hyper_jump.h` (`_Static_assert`):
`HJ_METATRON_VIS(48) + HJ_METATRON_RESID(16) == HJ_METATRON_FULL(64)`.

## Proof: occlusion-residual conservation

`tests/test_occlusion_residual.c` — **19/19 PASS** (integer-only, no floats):

- Orthographic projection of the 4×4×3 stack (48 points) on 3 viewports:
  visible 12/12/16 + occluded 36/36/32 = **48 every angle**.
- After 90° rotations (reshape): still 48 — points only change side.
- 48 rotated points are pairwise distinct (no collision = no loss);
  inverse rotation restores the exact original (`memcmp == 0`).
- Every point is visible from ≥1 viewport — hidden is relative, never
  permanent.

Verdict: "reshape doesn't break" is a property of projection, not a belief.

## Continuation: hyper_jump (4D)

`core/geo_hyper_jump.h` + `tests/test_hyper_jump.c` — **47/47 PASS**.
Lifts the concept to 4D tes-compound towers (pole-to-pole gates):

- 48-tes × 3 towers = 144-tess (gidpith tower, mirrors stride-3)
- 36-tes × 4 towers = 144-tess (spic tower, mirrors stride-4)
- Jump = next tower + mirror local (same shape as `JUMP_INVERT`).
- Verified against Klitzing LUT: 48xtes/gidpith army
  (`2{;3;3;4;}[48{4,3,3}]`), 36xtes/spic army, 18xtes/cont army dual pair.
  Correction recorded: botapna = 24xhex (rico), not 18-hex.
  Open: 72-tes/144-tes (Gidac/Gidgicana) are forward construction,
  no tes/hex rows in Klitzing; 48-tes = 6ico equation unproven
  (see memory #6307, #6312).

## Related memories

- #6307 — tesseract-compound ladder verification
- #6312 — geo_jump Metatron origin + occlusion residual

## Addendum: weave, conservation, real data (same day)

- `core/gidpith_edges.inc` — 384 verts / 768 edges / degree 4, derived from
  Klitzing coordinates (all perms + sign flips), verified against the
  `384/768/464/80` face vector. Central symmetry + antipode involution hold.
- `core/geo_gidpith_weave.h` + `tests/test_gidpith_weave.c` — **9/9 PASS**.
  Key finding: the graph is **bipartite by BFS shell** (zero same-shell
  edges), so a single-shell wall is provably impossible; the wall walks a
  shell-pair band (minimal floor-like unit). 17 shells, spine = diameter 17.
- `tests/test_hyper_conserve.c` — **8/8 PASS**. hj3/hj4 permutations of 144
  with inverse roundtrips (hj3⁶=hj4⁴=id) + **shell[v]+shell[anti[v]]=16
  for all 384 verts** — nothing permanently hidden.
- `tests/test_hyper_jump_real.c` (GGUF group) — **6/6 PASS** on
  Qwen2.5-0.5B-Q8_0 (288 KB @+1MB, digest 6f1296b85cc949e5): hj3/hj4
  scatter roundtrips byte-identical, all 3 towers ridden, model untouched.
- Open remainder: full serve-path integration (MoE expert order / tess
  routing) and full-coverage wall traversal (greedy band walk covers 9/90).

## Addendum 2: independent isomorphism proof (Claude parallel + local cross-check)

Claude built the gidpith graph from pure math (Cayley graph of Coxeter
group B4: signed permutations of (1,2,3,4), 3 adjacent swaps + sign flip)
without touching `gidpith_edges.inc`, then proved **VF2 isomorphism** with
the shipped table. Local cross-check (independent script, same counter both
sides) confirms every invariant matches exactly:

- 384v / 768e, degree 4, bipartite, diameter 16, triangles 0
- BFS profile identical: 1,4,9,16,24,32,39,44,46,44,39,32,24,16,9,4,1
- 4-cycles equal both sides; B4 centrally symmetric (v→−v automorphism)
- HJ_ANTIPODE verified 384/384 against geometric negation via the
  isomorphism vertex→coord mapping (apex 0 = (1,2,3,4), anti = 383)

Status upgrade: `gidpith_edges.inc` is no longer "consistent with" gidpith —
it **is** the gidpith vertex graph.

Gate-map caveat (stands): layer numbering is hand-transcribed from the
Klitzing page, and the page publishes **no per-vertex layer membership**,
so a 1:1 vertex→layer proof against source is impossible from published
data. Strongest available: pole/antipode consistency (shell identity,
proven) + mechanical re-transcription check.
