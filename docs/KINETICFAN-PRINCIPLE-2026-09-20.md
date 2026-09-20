# Kineticfan Principle — parametric gear cartridge (ตลับเกียร์)

Date: 2026-09-20 · Status: proven by test (32/32) · Test: `tests/test_poly11_oracle.c` (P11–P13)

## 1. The principle

One equilateral seed triangle (side `s = 2π`) + two integers `(a, b)` generate a whole
family of exact regular-polygon assemblies:

- Center **shaft**: `b`-gon erected on seed segment AB (slider `b ∈ [3,12]`).
- **Housing**: three `a`-gon rings erected outward on the three seed sides
  (slider `a ∈ [3,24]`). Always 3 petals regardless of `b` — they attach to the
  seed sides, not to the shaft sides.
- **Fan**: black chords from the two hubs (E top, G right) to the ring vertices,
  fan24-style: hub → chord → regular `a`-gon, with the outer/inner side rule
  picking one mirror (see §4).

`a=4, b=3` reproduces the distortcube start net (triangle + 3 quads).
`a=24, b=12` is the full state: 12-shaft + 24-rings (both sacred numbers at once).

## 2. Seed (oracle: kineticfan.ggb, 16 decimals)

```
A = (-h, 2π), B = (-h, 0), G = (0, π),  h = π√3,  s = 2π
|AB| = |AG| = |GB| = 2π  (exact to 1e-15 — equilateral, not assumed)
```

Seed axis `x = -h = -5.441398092702653` doubles as the poly11 centroid x:
the new machine is built on the previous session's constants.

## 3. Fit spec — shaft vs housing (proven, P11)

| b | class | mechanism |
|---|-------|-----------|
| 3 | **contact (seated)** | keystone: shaft == seed triangle, full edge contact, apex on hub G |
| 4 | **overlap** | east corners pierce rings, penetration `x = s − h ≈ 0.8418`, 2 crossings |
| 5 | **overlap** | deep interference |
| 6 | **overlap** | swallows hub G (depth `h`), both ring tips touched at 0.0 |
| 7, 8 | **overlap** | engulfs ring tips (one-sided tests go blind here — check both directions) |

Holds for housings `a = 3, 4, 5`. Control: west-erected shaft clears by exactly `s`.
Practical rule: **`b = 3` is the last clean fit; `b ≥ 4` interferes.**
(Clearance / contact / interference = สวมหลวม / พอดี / อัด — usable as a 2-bit gate verdict.)

## 4. Mirror side rule (oracle: docs/fan24_start.html)

Every chord carries two mirror polygons (cf. distortcube poly15/17 drawn doubled).
The fan24 demo resolves it deterministically: steps `s ≤ 12` erect outward,
`s > 12` inward, diameter tie-broken by chord direction (verified: 12/11/1,
regularity violations 0 for `aa ∈ [3,24]`). Distortcube's "distortion" was this
rule missing — both mirrors drawn at once.

## 5. Full 24/12 state (oracle: kineticfan (1).html, base64 XML)

- Center 12-gon: regular (side dev 6e-15), area **442.01**, E/W flat sides
  (span `2·apo`), sacred 12 (dodeca faces / ticks-per-pipe).
- Hubs K, G (free `PointIn` points) stacked exactly on ring vertex V2 (dist 0.0);
  label `C_2` was accidentally deleted, position preserved by the stack.
  Restore: type `C_2 = K` in GeoGebra (check `|B−C_2| = 6.28`).
- Petal sides: poly2/poly3 = `2π` (automatic: G=V2 adjacent to B), poly4 = `Rcirc`
  by the 2-step chord theorem (`chord(2/12) = R` exactly); poly4 area **6714.74**.
- Fan samples match file: 73.78, 46.50, 56.61, 24.07, 24.07.
- Sacred pair: `a = 24` (ring-24, COMPOUND_24) + `b = 12` in one state.

## 6. DWGLS mapping

| Principle part | Maps to | Status |
|---|---|---|
| `(a, b)` selectors | `geo_param_grid.h` GeoType family | proven via P13 (6 pairs through `geo_net_walk`: `V=b+3a−6, E=b+3a−3, disk=1`) |
| 24-ring fan + hub | `fan24_gear.h` (ring-24, 8×3) | principle match; 2-hub tie-break open |
| outer/inner rule | orientation bucket (`im_rhombus_param` 6→3), zero-sum filter | principle match |
| fit classes 0/1/2 | gate verdict (admission) | spec ready, **no consumer yet — not built (YAGNI)** |
| 6 incidences + ring LUT | anchor table | spec ready, **no consumer yet — not built** |
| chord=R, scale ladder | computation-over-storage (derive, don't store) | pattern reference |

Non-goals (explicit): no compression value (#898), Euclidean π-metric never enters
the int-only grid — only incidence / side-class / symmetry transfer.

## 7. Oracle files (independent ground truth, never from implementation)

- `distortcube.html` / `distortcube2.html` — poly11 ring + poly12–22 (10 dp table)
- `Downloads/kineticfan.ggb` — parametric seed, saved at (3,3) + a=4 anchors
- `Downloads/kineticfan (1).html` — full (24,12) state + base64 (16 dp XML)
- `docs/fan24_start.html` — hub + aa-slider + outer/inner rule demo
- Scratch proofs in `%TEMP%\opencode\` (`gearfit.py`, `kf2412*.py`, `fancheck.py`) —
  superseded by the test, kept for audit.

## 8. Open items

1. 2-hub diameter tie-break has no rule yet (demo proves 1 hub only).
2. Fit verdict / incidence LUT get core headers only when a consumer orders them.
3. `geo_octant.h:119` comment fixed 2026-09-20 (was stale pre-fix mapping).
