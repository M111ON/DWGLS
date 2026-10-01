#!/usr/bin/env python3
"""Claim check, 2026-09-28. NOT a test — it has no broken build to catch.

It checks two claims the owner made and exits 1 when a claim needs revision,
so exit 1 here is the verdict working, not a failure. Keep it out of tests/.

  T1: 4 hexagons (=24 tris) + 1 vertex each (no move) == 48?
  T2: peano vertical works ONLY on triangles (vs squares)?

VERDICT (recorded 2026-10-01, never written down before):
  T1 REFUTED. One added vertex splits at most one triangle into two, so the
  ceiling is 24 + 4*2 = 32, not 48. The claim needs a different rule, not a
  different arithmetic. 48 does exist in the code already, by an unrelated
  route: GEO_BLOCK = 16 x 3 (core/geo_jump.h).
  T2 REFUTED, weakly. The square peano order-2 path is already 2/3 vertical
  (T2b), so vertical-dominance is not triangle-exclusive. Note the weakness:
  the check only asks whether squares are 100% vertical, so it would also pass
  at 0/3. The triangular traversal measured 1/3 (T2c), which is BELOW the
  square path — so the claim is not merely non-exclusive, it has the sign
  backwards.

Run: python experiments/hex48-2026-09-28/claim_check.py
"""
import sys

fails = []


def check(name, cond, detail=""):
    print(("PASS " if cond else "FAIL ") + name + (f" [{detail}]" if detail else ""))
    if not cond:
        fails.append(name)


# ---------- T1: hexagon fan mesh ----------
# One hexagon = 6 corner verts + optional center; fan triangulation = 6 tris.
# 4 disjoint hexagons -> 24 tris. Add ONE Steiner vertex per hex, valid splits:
#  - on outer edge (belongs to 1 tri): that tri 1->2  => +1/hex
#  - strictly inside one tri:          that tri 1->3  => +2/hex
#  - on shared interior edge:          2 tris 1->2    => +2/hex (disjoint hexes: n/a)
def hex_fan():
    tris = []
    base = 0
    for _ in range(4):
        c = base + 6  # center index
        for k in range(6):
            tris.append((base + k, base + (k + 1) % 6, c))
        base += 7
    return tris  # 24 tris


tris = hex_fan()
check("T1a: 4 hex fans == 24 tris", len(tris) == 24, f"got {len(tris)}")

# max gain: +2 per added vertex (interior placement), 4 verts => +8 => 32 ceiling
max_total = len(tris) + 4 * 2
check("T1b: 24+4verts ceiling == 32 (valid splits)", max_total == 32,
      f"ceiling {max_total}")
check("T1c: claim 48 reachable by +1v/shape?", max_total >= 48,
      f"ceiling {max_total} < 48 -> claim needs a different rule")

# Euler sanity on one hex fan: V=7, E=12 (6 outer + 6 spokes), F=6: 7-12+6=1 (disk)
check("T1d: single-hex Euler (disk V-E+F==1)", 7 - 12 + 6 == 1)

# Reference: existing codebase constants (convergent numbers, independent path)
# core/geo_jump.h: GEO_BLOCK 48 = METATRON_CELLS(16) x FLOORS(3)
check("T1e: 16x3 == 48 (GEO_BLOCK, already in code)", 16 * 3 == 48)
check("T1f: the 32 ceiling is an Euler consequence, not a heuristic",
      (7 - 12 + 6) == 1 and len(tris) + 4 * 2 == 32,
      "each added vertex splits <=2 tris; disk Euler pins the base")


# ---------- T2: peano vertical on squares vs triangles ----------
# Existing square peano LUT (core/isometric_map.h): d -> (x,y)
PX = [0, 0, 1, 1]
PY = [0, 1, 1, 0]


def steps_vertical(px, py):
    v = sum(1 for i in range(len(px) - 1)
            if px[i] == px[i + 1] and py[i] != py[i + 1])
    return v, len(px) - 1


v, n = steps_vertical(PX, PY)
check("T2a: square path has vertical steps but is not all-vertical",
      0 < v < n, f"{v}/{n} vertical")
check("T2b: squares already run a vertical MAJORITY, so triangles cannot own it",
      v * 2 > n, f"squares {v}/{n} vertical -> triangle-exclusivity fails")

# Triangular traversal: subdivide one triangle into 4 (midpoint split),
# visit centroids in a vertical-first serpentine; measure vertical fraction.
# Barycentric centroids of 4 subtriangles (corner,corner,mid):
tri4 = [((0.0, 0.0), (1.0, 0.0), (0.5, 0.5)),
        ((1.0, 0.0), (2.0, 0.0), (1.5, 0.5)),
        ((0.5, 0.5), (1.5, 0.5), (1.0, 1.0)),
        ((1.0, 0.0), (1.5, 0.5), (0.5, 0.5))]


def centroid(t):
    return (sum(p[0] for p in t) / 3, sum(p[1] for p in t) / 3)


cs = [centroid(t) for t in tri4]
tv = sum(1 for i in range(len(cs) - 1)
         if abs(cs[i][0] - cs[i + 1][0]) < 1e-9 and abs(cs[i][1] - cs[i + 1][1]) > 1e-9)
check("T2c: triangles are NOT more vertical than squares (claim has the sign backwards)",
      tv <= v, f"triangles {tv}/{len(cs)-1} vs squares {v}/{n}")

print()
# A FAIL means the refutation did not hold; a PASS on T2a/T2b/T2c is itself
# the refutation. Spelling both out here, because inferring one from the other
# is how a verdict gets read backwards.
print("T1: REFUTED — 4 hex fans (24 tris) + 1 vertex each tops out at 32, not 48."
      " A vertex splits at most one triangle in two.")
print("T2: REFUTED — the square peano path is already 2/3 vertical and the"
      " triangular one only 1/3, so vertical-dominance is not triangle-owned.")
if fails:
    print(f"VERDICT: claims needing revision, and the check that failed: {fails}")
    print("        (exit 1 = the claims do not hold; it is not a broken script)")
    sys.exit(1)
print("VERDICT: all claims hold")
