# Field bridge — parked concept (2026-10-08)

**Status: NOT WIRED. Concept only.** Nothing here is in `make test`, no Makefile
target, no runtime path. Parked so the plain/normal system stays stable first.
Do not build these into anything until the current pipeline is settled.

## The idea

The storage side and the tensor (BFS) side are the **same 20736 object** seen
through two different factorizations. The equation that bridges them:

```
12  x  6  x  8  x  6  x  6   =  20736  =  12^4
```

and it reconciles with the tesseract ladder:

```
18  x  8  x  144             =  20736        (tess x cell x slot)
```

because `18 = 3x6` and `144 = 4x6x6` — the two ladders are the same product.

The tensor side is the **Tesseract 4D** line (`core/geo_tesseract_addr.h`,
`geo_tesseract_dense.h`). The storage side is the tombstone/latch + frustum
field. The equation is the bridge; it has not been built.

## What the running product hits (all real named constants)

| product | value | constant | owner header |
|---|---|---|---|
| 12 | 12 | `GEO_PENTAGONS` | `tri_hex_tess.h` |
| 12x6 | 72 | wl choice space (`wl_id = cell*72+choice`) | `geo_wang_latch.h` |
| 12x6x8 | 576 | `TD_HALF` (tesseract half) / stride-37 orbit | `geo_tesseract_dense.h` |
| 12x6x8x6 | 3456 | `FRUSTUM_DATA_SZ` (= 54 x 64) | `frustum_slot64.h` |
| 12x6x8x6x6 | 20736 | `GEO_FULL` = `OCT_FULL` = `TESS_GEO_FULL` | many |

The factors are the code's own constants: 12 = pentagons, 6 = `FACE_COUNT` =
`CUBE_HALF_AXES` (3 axes x 2 signs), 8 = `OCT_CUBES` = `TESS_3D_CELLS`, and the
last two 6s = the tesseract cell adjacency degree (`tess_adjacent` returns 6).

## What this is NOT (boundaries, learned the hard way)

- **Not cube assembly.** There is no code that composes frustum faces into a
  cube, and none is required. `6 == 6` is a direction/face **rule identity**,
  not an assembly step. (See `cube_cover_probe.c` L2.)
- **Not a join yet.** `wl_id(cell,choice)` and `tess_flat(tess,cell,slot)` both
  live in the same 20736 field, but **no code joins them**. The bridge is an
  observation, not an implementation.
- **Not compression.** MAP, not COMPRESS — same doctrine as always.

## Supporting probes (parked, reproducible by hand)

Both compile standalone with `gcc -O2 -Wall -I. -Icore <file>.c -lm` from the
repo root. Neither is in the build.

- `cube_cover_probe.c` — `1<4<16<64` is a 4-ary (quadtree) ladder; frustum =
  same tree entered by 6 directions (rule identity, no assembly); one field has
  more than one nested decomposition.
- `field_ladder_probe.c` — the `12 x 6 x 8 x 6 x 6` ladder above, every factor
  read from its owning header, both ladders reconciled, and the boundary stated
  in the output.

## When to revisit

Only after the plain pipeline is stable. Then the question is: does making the
storage<->tensor identity explicit buy anything real (a shared address, one
walk, no translation), or is it just a pretty coincidence of arithmetic?
Answer that with a measurement, not a diagram.
