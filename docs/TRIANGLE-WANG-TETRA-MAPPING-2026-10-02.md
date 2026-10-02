# Triangle, Wang, and Two-Tetra Mapping

Date: 2026-10-02
Status: verified as an integer/incidence composition probe; not a production
addressing change.

## Purpose

This note records the experiment that started from the equal-triangle /
inward-semicircle construction and tested whether it composes cleanly with a
12x6 Wang-style route model and the existing cube/octant conventions.

The experiment is deliberately outside `core`. It does not replace the KIS
field, move payload bytes, create a persistent tree, or change the P0/P1
semantic route benchmark.

## Construction

The geometric observation is:

```text
equal triangle
  -> inward semicircle on each side
  -> inner intersection triangle
  -> repeat
```

The inner triangle is equilateral and has half the side length of its parent.
For routing, the construction is represented only as a ternary choice:

```text
3 branches per layer
3 layers
3^3 = 27 routes
```

No coordinates or payload are stored by the route probe.

## Wang Composition

The experimental Wang model gives each route six directions. The composition
therefore has:

```text
27 routes x 6 directions = 162 positions
27 routes x 3 branch groups = 81 positions
```

The three-layer triangle route was checked against complementary Wang edges.
All 27 routes snapped deterministically and joined across the three layers in
the probe.

This proves composition of the probe rules, not equivalence to every Wang
implementation in `core`.

## Field Factorizations

Both factors fit the existing field exactly:

```text
128 x 162 = 20,736
256 x 81  = 20,736
144 x 144  = 20,736
```

The `128x162` and `256x81` views are related by quotient/remainder splitting.
For `lo162 = q*81 + r`, where `q` is 0 or 1:

```text
hi256 = hi128*2 + q
lo81  = r
```

The inverse recovers `hi128`, `q`, and `r`. An exhaustive sweep covered all
20,736 field positions with no collision or missing position.

## Cube and Two-Tetra Incidence

A cube corner is a 3-bit state:

```text
000 001 010 011
100 101 110 111
```

The eight corners split by parity into two sets of four:

```text
Tetra A: 000, 011, 101, 110
Tetra B: 001, 010, 100, 111
```

Within each set, every pair differs in exactly two bits. This is the edge
incidence of an inscribed tetrahedron. Bitwise complement maps every corner to
the opposite cube corner and always crosses from one tetrahedron to the other:

```text
000 <-> 111
001 <-> 110
010 <-> 101
100 <-> 011
```

This agrees with the existing `geo_octant.h` convention at the level of eight
cube states, four tetra-active states, and complementary antipodal states.
The existing zero-sum active set is a coordinate contract; it is not silently
being renamed as the parity tetrahedron split.

## Verification Receipts

### Equal triangle and semicircle

Target: `mingw32-make equal-triangle-semicircle`

```text
result: 6 passed, 0 failed
outer side=2.000000000 inner side=1.000000000
```

### Triangle route

Target: `mingw32-make equal-triangle-route`

```text
triangle route roundtrip: PASS (19683 checks)
ternary depth=9 leaves=19683 field=20736 unused=1053
route steps/query=9; payload/storage bytes moved=0
```

### Triangle/Wang snap

Target: `mingw32-make triangle-wang-snap`

The probe passed all route snap, complementary-edge, centroid coverage, and
roundtrip checks for the experimental 12x6 model.

### Field equivalence

Target: `mingw32-make triangle-field-equivalence`

```text
triangle/Wang factors: PASS
27x6=162, 27x3=81, 128x162=20736, 256x81=20736
exhaustive checks: 20979, field coverage: 20736
```

### Cube/two-tetra incidence

Target: `mingw32-make cube-two-tetra-incidence`

```text
cube/two-tetra incidence: PASS
corners=8 tetrahedra=2 corners_per_tetra=4 antipodal_pairs=4
result: 5 passed, 0 failed
```

## What This Establishes

The experiment establishes these concrete facts:

1. The equal-triangle construction produces a repeatable ternary route rule.
2. Three route layers produce 27 route states.
3. The experimental Wang 12x6 composition produces 162 positions and a
   three-way grouping produces 81 positions.
4. `128x162`, `256x81`, and `144x144` address the same 20,736-position field.
5. Eight cube corners split into two four-corner tetrahedral incidence sets,
   with four antipodal pairs crossing the sets.
6. These checks use integer/address rules and do not require runtime geometry
   materialization.

## What This Does Not Establish

This is not yet:

- a replacement for `core/geo_octant.h`, `anchor_route.h`, or Wang code;
- proof that the drawn overlay is a full geometric embedding of a production
  Wang graph;
- proof of semantic recall, latency improvement, or reduced storage reads;
- proof that a 144^6 global space is required;
- a change to payload storage or the existing P0/P1 benchmark.

The next justified step, if pursued later, is an explicit incidence adapter:
triangle/Wang route states to cube-corner and tetra states, with forward,
reverse, edge, and antipode receipts. It should remain outside `core` until a
real consumer demonstrates value.
