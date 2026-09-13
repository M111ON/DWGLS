# Inner Field Spec — digit-extension nesting (2026-09-13)

Approach A (approved). Address math only — zero bytes stored.

## 1. Model (#807)

Each cube holds one FULL 20736-field, equal to the outside:
`16x16 (quadtree) x 9x9 (loshu) = 256x81 = 20736 = 12^4`.
Totals: `20736 x 8 cubes x 18 tess = 2,985,984 = 144^3`; one level deeper
`20736^3 = 12^12 = 8,916,100,448,256 (~8.9T)`. Arithmetic verified live.

## 2. Address format

- L0 (existing, untouched): `(tess, cube, slot)`, `slot = q*9+l`,
  `q in [0,16)` quadtree branch, `l in [0,9)` loshu cell
  (`144 = 16x9`; `tess_to_flat`/`flat_to_tess` unchanged).
- L1 (inner): extend coarse digits with fine digits
  `q2 in [0,16)`, `l2 in [0,9)`:
  `Q = q*16+q2 in [0,256)`, `L = l*9+l2 in [0,81)`,
  `inner = Q*81+L in [0,20736)` — same shape as `th_node(hi,lo)`.
  Outer slot is the coarse prefix; inner is the full field.
- L2: `(X,Y,Z)` triple of L1 addresses (one per KIS axis) = `20736^3`.

## 3. Code (`core/geo_inner_field.h`, header-only, int-only)

- `inner_from_outer(q,l,q2,l2) -> inner` (O(1), no lookup)
- `inner_split(inner, &Q, &L)` / `inner_coarse(inner, &q, &l)`
- `inner_parent(inner) -> slot` (drop fine digits; lossless inverse of prefix)
- No storage, no malloc, no float. Reuses `th_node` representation;
  does not modify `tri_hex_tess.h` or `geo_tess_wiring.h`.

## 4. Verification

- L1: bijection `20736/20736` exhaustive per cube; `parent(child)==prefix`
  roundtrip all; test oracle = independently written brute-force digit
  formula (never calls impl); mutation check (e.g. `16->15` must go red).
- L2: sampling + roundtrip (8.9T not exhaustible — stated, not hidden).
- Tautology guard: parent/child must move digits (identity impl rejected).

## 5. Scope (honest)

- No bytes stored at any level (hotel rule: materialization is caller's
  business). Demo cell stays the 20KB outer shell (`breathing_fs.h`,
  `hyper_delta.h` untouched). Closed threads (playbook) not reopened.
