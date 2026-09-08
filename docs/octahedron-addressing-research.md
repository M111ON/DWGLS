# Octahedron Addressing Research — Session Compilation (2026-09-07)

Source: sessions §1–§68 (Lo Shu n15 + octahedron edge-sum + letter cube + stella + DGGS).
Status: research compilation. Spec items marked PROVEN / REJECTED / OPEN.

---

## 1. Octahedron Edge-Sum Addressing (PROVEN)

- 6 vertices, labels 1–6. 12 edges (all pairs minus 3 opposite pairs).
- Address = ordered 12-vector of edge sums `e_uv = f(u)+f(v)`.
- Injective 720/720 — zero collisions (brute-force verified).
- Why: incidence matrix 12×6 has trivial nullspace (odd cycles force f=0). Theorem, not luck.
- Bit budget: 6! needs 9.5 bits; naive 12-vector costs 48 bits. NOT a short address — it is a **redundant / self-checking address** (1 corrupt vertex → 4 edge-sums wrong, free error detection).
- CRITICAL: vector must be ORDERED by fixed edge index. Sorted multiset collapses to 13 distinct. Canonical order `opp=(0,1),(2,3),(4,5)` frozen.
- vs 3×3 grid: grid is 9 unknowns / 8 equations (underdetermined, forced collisions); octahedron is 6 unknowns / 12 equations (overdetermined + nullspace zero).

## 2. Lo Shu Magic Square + n15 (PROVEN data)

- n15 = count of lines summing to 15 in a 3×3 permutation of {1..9}. Variable metric: `n15 = sum_all/3` per value set; changes with mode + set.
- Full distribution over 362,880 permutations:
  0:168,768 (46.5%) · 1:131,040 (36.1%) · 2:48,960 (13.5%) · 3:10,240 (2.8%) · 4:2,824 · 5:832 · 6:176 · 7:32 · 8:8 (0.0022%, the magic squares).
- Decay curve, not bell: each extra line cuts count ~3–6×. Near-magic states exponentially rare.
- Center=5 gap: n15 ∈ {0,1,2,3,4,5,6,8} — **no 7-line state exists with center=5**. Real decode constraint, cuts search space.
- Swap-distance vs n15 is NOT monotonic (n15=5 avg distance 4.00 > n15=4 avg 3.65; heavy overlap). n15 must NOT be used as BFS heuristic.
- Complement duality `x → 10-x`: preserves per-line pass/fail 100% (0 mismatches / 5,000 samples). Reason: line summing to 15 → 30−15=15; failing line stays failing. **Applies to ALL 362,880 permutations** — universal twin, not magic-square-only.
- REJECTED: "1↔7 recall via single swap" — 0 paths found in 2,000 n15=1 samples. Needs external key, not symmetry alone.
- Lo Shu role: toy model of constraint-based filtering (3×3 only). Octahedron edge-sum is the generalization that "finished cutting" by nature.

## 3. Letter Cube (PROVEN core, REJECTED extras)

- 6-face cube, 12 adjacent-face swaps (no opposite swaps). Intra-island = S6 on 720 states, BFS diameter = 7, distribution `1,12,70,197,271,150,18,1` (independently re-verified).
- Face-adjacency graph = octahedron graph (cube dual), NOT cube graph.
- REJECTED: v1 "God's = 1" (trivial generator definition), "loop closes at LCM 78/12" (LCM gives phase alignment, not a constructed walk), "surplus 28.5 bits for cycle tracking" (real use = membership check only).
- 24-pool: pool 24 = 4×6-windows or 3×8-windows, 20736/24=864 clean. C(24,6)=134,596 (17 bits) · C(24,8)=735,471 (19.5 bits). Two-level pipeline: choose group (combination) + arrange (permutation). Inter-island bound ≤13 (6 replacements + 7 permute) once move defined. Inter-island selector still OPEN.

## 4. Stella Octangula / Tetra Compound

- 8 cube corners (tetra A 4 + tetra B 4) + 6 octa midpoints = 14 distinct points (rhombic-dodecahedron vertex set). Midpoint pairs of tetra → octa vertices exactly.
- MODE_FREE (14 slots, 36.3 bits) vs MODE_CHECK (8 tetra free + 6 octa derived, 15.3 bits + sum_A==sum_B cross-check). Never mix in one window.
- 14 does NOT divide 20736. 24 does. 24 = rotation group of cube/octa = 4!.

## 5. DGGS Hex Grid (reference system)

- Images: basis vectors (v1,v2); LCS(RT)↔GCS↔LCS(ICO′) transform chain; two hex layouts.
- Their method: 2D coord + transform-matrix lookup. Ours: injective formula, zero lookup.
- 6 directions ω¹–ω⁶ ↔ 6 octahedron vertices 1:1. Octa hex cross-section (plane x+y+z=0, 3-fold axis) is the geometric link.
- Hexagon adds ZERO address capacity (linear derive of same 6 vertices) — it is a check/debug view, not a layer.

## 6. What DWGLS Can Use (assessment)

1. **Membership check** — edge-sum vector outside the 720 valid set = corrupt, ~100% detection, O(1). Fits MAP-not-COMPRESS + content-derived address.
2. **Complement halving** — store half vector + 1 polarity bit, reconstruct via duality. ~50% header storage cut where self-checking headers are used.
3. **n15-gap pruning** — no-7-at-center-5 class constraints skip dead branches at decode.
4. **BFS-7 reroute rule** — any intra-window state reachable ≤7 swaps; never use n15 as distance heuristic.
5. **24-pool tiling** — 24 divides 20736 cleanly (864); standard block size for 6/8-windows. Inter-island selector remains the open problem.
6. **NOT usable**: Lo Shu 3×3 rules at other sizes; LCM-as-walk-closure; sorted edge-sum vectors; n15-distance heuristic; 1-bit encoding saving alone (8.3%, ~2.5KB over full field — noise at GB scale).

## 7. Frozen Invariants

- I-1: canonical edge order `opp=(0,1),(2,3),(4,5)` shared by addressing + navigation.
- I-2: 12 generators = non-opposite transpositions; BFS diameter 7.
- I-3: ordered vector only; sorted form forbidden.
- I-4: complement duality universal over full permutation space.
- I-5: inter-island move must be defined before any closure/diameter claim.
- I-6: mode is per-container; never mix FREE/CHECK in one window.

## 8. Open Problems

- O-1: inter-island selector (which 6 of pool enter the window).
- O-2: n15 as variable metric — formalize (mode, value-set) → threshold mapping.
- O-3: unified address/navigate pipeline composing §6 items 1–5.
