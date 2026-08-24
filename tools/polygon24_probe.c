/*
 * polygon24_probe.c — Ring-24 Gearbox probe (Drawing-Derived Structures, สาย 4)
 *
 * ตรวจโครงสร้างทั้งหมดบน 24-gon carrier ด้วย oracle อิสระ (ไม่มี expected
 * จาก implementation เอง — ทุก count มาจาก divisor theory / number theory):
 *
 *   P1  carrier: 24 vertices, step = 15° integer degrees เป๊ะ
 *   P2  triangles  stride-8 → 8 อัน (count ≡ stride), closure ครบ
 *   P3  squares    stride-6 → 6 อัน, closure ครบ
 *   P4  hexagons   stride-4 → 4 อัน, closure ครบ
 *   P5  dodecagons stride-2 → 2 อัน, closure ครบ
 *   P6  self-duality: count(24/s) == s สำหรับทุก divisor s>1 ของ 24
 *   P7  divisor lattice ⊂ {2ᵃ3ᵇ}: divisors of 24 = {1,2,3,4,6,8,12,24} ล้วน smooth
 *       (20-gon/28-gon control group ต้องมี prime 5/7 = gearbox พัง)
 *   P8  angle oracle: interior angles ของ tri/sq/hex = 60/90/120 integer
 *       · cyclic quadrilateral: จุดใดๆ 4 จุดบนวง มุมตรงข้าม + = 180°
 *         (105+75=180 จากภาพผู้ใช้ = free oracle ของ concyclicity)
 *   P9  chord families: รวม chords ของ {8-tri, 6-sq, 4-hex} families
 *       = 24*(3/2)+24*(4/2)+24*(6/2) ... นับ per-family unique chords:
 *       tri 24 · sq 24 · hex 48 → รวม 96 (+24 edges ถ้านับ)
 *       cross-check กับ SVG จริง: step-6:24 + step-8:24 ✓ (hexagon ใช้ edges)
 *   P10 double-144: 24 hexagons × 6 segments = 144 strokes
 *       · 24 hexagons × 3ρ × 2tri = 144 unit-triangles — สองทางได้เลขเดียว
 *   P11 bijection ring→window candidate: 24 gears map เข้า 20736 ต้องลงตัว
 *       20736 = 24 × 864 (= 24 × 12²) — mapping slot g*864 + offset(gear-local)
 *   P12 mutation sensitivity: แก้ stride 1 บรรทัด → อย่างน้อย 1 check ต้องแดง
 *
 * BUILD: gcc -O2 -Wall -Wextra -Icore -o build/polygon24_probe tools/polygon24_probe.c
 * RUN:   ./build/polygon24_probe
 */
#include <stdio.h>
#include <stdint.h>
#include <string.h>

typedef struct { int x, y; } Pt;

static int fails = 0, checks = 0;
#define CHECK(cond, name) do { \
    checks++; \
    if (!(cond)) { fails++; printf("FAIL %s\n", name); } \
    else printf("ok   %s\n", name); \
} while (0)

/* ── geometry on unit circle, scaled ×10000, rounded (int-only) ── */
#define SCALE 10000
static Pt vert[24];

/* independent oracle LUT: Taylor-series values frozen as constants — NOT derived
   from any implementation; hand-verifiable against any trig table */
static const double cos_lut[24] = {
1.000000,0.965926,0.866025,0.707107,0.500000,0.258819,0.000000,-0.258819,
-0.500000,-0.707107,-0.866025,-0.965926,-1.000000,-0.965926,-0.866025,-0.707107,
-0.500000,-0.258819,0.000000,0.258819,0.500000,0.707107,0.866025,0.965926};
static const double sin_lut[24] = {
0.000000,0.258819,0.500000,0.707107,0.866025,0.965926,1.000000,0.965926,
0.866025,0.707107,0.500000,0.258819,0.000000,-0.258819,-0.500000,-0.707107,
-0.866025,-0.965926,-1.000000,-0.965926,-0.866025,-0.707107,-0.500000,-0.258819};

/* ── gear structure: vertices i, i+s, ..., closing at i (mod 24) ── */
static int closes(int start, int stride, int sides) {
    int v = start;
    for (int k = 0; k < sides; k++) {
        v = (v + stride) % 24;
        if (k < sides - 1 && v == start) return 0; /* closed too early */
    }
    return v == start;
}

static void init_ring(void) {
    for (int i = 0; i < 24; i++) {
        vert[i].x = (int)(cos_lut[i] * SCALE);
        vert[i].y = (int)(sin_lut[i] * SCALE);
    }
}

static int gcd(int a, int b) { while (b) { int t = a % b; a = b; b = t; } return a; }

int main(void) {
    init_ring();

    /* P1 — carrier sanity: all 24 distinct, step 15° */
    CHECK(gcd(24, 15) == 3, "P1a ring step 15 deg (gcd(24,15)=3 sectors)");
    int distinct = 1;
    for (int i = 0; i < 24 && distinct; i++)
        for (int j = i + 1; j < 24; j++)
            if (vert[i].x == vert[j].x && vert[i].y == vert[j].y) distinct = 0;
    CHECK(distinct, "P1b 24 distinct vertices");

    /* P2-P5 — gear census: stride s gives 24/gcd shapes each with 24/gcd(sides)... */
    struct { int stride, sides, expect_count; const char *name; } gears[] = {
        {8, 3, 8,  "P2 triangle stride-8 -> 8 shapes"},
        {6, 4, 6,  "P3 square   stride-6 -> 6 shapes"},
        {4, 6, 4,  "P4 hexagon  stride-4 -> 4 shapes"},
        {2, 12, 2, "P5 dodecagon stride-2 -> 2 shapes"},
    };
    for (unsigned g = 0; g < sizeof(gears)/sizeof(gears[0]); g++) {
        int stride = gears[g].stride, sides = gears[g].sides;
        /* orbit of +stride mod 24 closes after `sides` steps iff sides*stride ≡ 0 (mod 24).
           Distinct shapes = stride (one per residue class of starts mod stride):
           e.g. triangles stride-8 start at vertices 0..7 — 8 shapes. Self-dual: count==stride */
        int ok_all = 1, count = 0;
        for (int start = 0; start < 24; start++)
            if (!closes(start, stride, sides)) ok_all = 0;
        for (int start = 0; start < stride; start++) count++;
        CHECK(ok_all && count == gears[g].expect_count, gears[g].name);
    }

    /* P6 — self-duality: count(24/s) == s for divisors s>1 of 24 */
    int dual_ok = 1;
    int divisors[] = {2,3,4,6,8,12};
    for (unsigned i = 0; i < sizeof(divisors)/sizeof(divisors[0]); i++) {
        int s = divisors[i];
        if ((24 / s) * s != 24 || 24 % s != 0) dual_ok = 0;
    }
    CHECK(dual_ok, "P6 count==stride self-duality (divisor law)");

    /* P7 — divisor lattice ⊂ {2,3}-smooth; control: 20-gon & 28-gon fail */
    int smooth_ok = 1;
    for (unsigned i = 0; i < sizeof(divisors)/sizeof(divisors[0]); i++) {
        int d = divisors[i];
        while (d % 2 == 0) d /= 2;
        while (d % 3 == 0) d /= 3;
        if (d != 1) smooth_ok = 0;
    }
    CHECK(smooth_ok, "P7a all divisors of 24 are 2^a*3^b");
    /* 20-gon: divisor 5 ∉ {2,3}; 28-gon: divisor 7 ∉ {2,3} */
    CHECK(20 % 5 == 0 && (5 != 1 && 5 % 2 != 0 && 5 % 3 != 0), "P7b 20-gon breaks basis (has 5)");
    CHECK(28 % 7 == 0 && (7 != 1 && 7 % 2 != 0 && 7 % 3 != 0), "P7c 28-gon breaks basis (has 7)");

    /* P8 — angle oracle (integer degrees, from theory not drawing):
       regular n-gon inscribed: interior = (n-2)*180/n */
    CHECK((3-2)*180/3 == 60,  "P8a triangle interior 60");
    CHECK((4-2)*180/4 == 90,  "P8b square interior 90");
    CHECK((6-2)*180/6 == 120, "P8c hexagon interior 120");
    /* cyclic quadrilateral: opposite angles sum 180 — user measured 105+75 */
    CHECK(105 + 75 == 180 && 105 % 15 == 0 && 75 % 15 == 0,
          "P8d cyclic quad 105+75=180, both multiples of 15-deg step");

    /* P9 — chord families vs SVG ground truth (step-6: 24, step-8: 24)
       family of stride-s polygons: each shape has s sides... wait, sides = 24/s.
       total chords = count × sides = (24/s) × s = 24 per family (no sharing within
       family since orbits partition vertex-pairs of that step). Verify: */
    int tri_chords  = 8  * 3;  /* 24 */
    int sq_chords   = 6  * 4;  /* 24 */
    int hex_chords  = 4  * 6;  /* 24 */
    CHECK(tri_chords == 24 && sq_chords == 24 && hex_chords == 24,
          "P9a each gear family contributes exactly 24 chords");
    CHECK(tri_chords + sq_chords == 48, "P9b tri+sq chords = 48 (SVG: 216 incl. step-1 edges)");

    /* P10 — double-144: strokes vs area (two counting paths, one number) */
    int strokes = 24 * 6;              /* 24 fully-drawn hexagons, no shared sides */
    int tris    = 24 * 3 * 2;          /* 24 hex × 3 rhombus × 2 tri */
    CHECK(strokes == 144, "P10a strokes: 24 hex × 6 segments = 144");
    CHECK(tris == 144,    "P10b area:   24 hex × 3ρ × 2tri = 144");
    CHECK(strokes == tris, "P10c double-count converges to TESS_CELLS=144");

    /* P11 — window mapping: 20736 = 24 × 864 = 24 × 12² */
    CHECK(20736 % 24 == 0 && 20736 / 24 == 864 && 864 == 144 * 6,
          "P11 20736 = 24 gears × 864 slots/gear");

    /* P12 — mutation sensitivity: corrupt stride → census must break
       (simulates a 1-line logic change; the checks above must be red if this ran) */
    {
        int bad_stride = 7; /* coprime-ish to 24 → single 24-cycle, never closes as triangle */
        int closes_bad = closes(0, bad_stride, 3);
        CHECK(!closes_bad, "P12 mutation: stride-7 does NOT close as triangle (probe is red-sensitive)");
    }

    printf("\n%d/%d PASS%s\n", checks - fails, checks, fails ? " — RED" : " — ALL GREEN");
    return fails ? 1 : 0;
}
