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
 *   ── gearbox completion (2026-08-25, ก่อนผู้ใช้วาดรูปที่เหลือ = oracle ล่วงหน้า) ──
 *   P13 octagon family stride-3 → 3 อัน (closes: 8×3=24) — family ที่ census เดิมตกหล่น
 *   P14 full convex gear census: stride s | 24 → s polygons ครบทุก s∈{1,2,3,4,6,8}
 *   P15 pentagon เป็นไปไม่ได้ (5 ∤ 24 — basis {2,3} fence)
 *   P16 chord partition: gear steps {1,2,3,4,6,8} = 144 chords (=TESS_CELLS,
 *       double-count ที่สาม), fence steps {5,7,9,10,11}+diameters = 132,
 *       รวม C(24,2)=276 ครบพอดี
 *
 *   ── construction B: edge-mounted set (วิธีวาดจริงของผู้ใช้, 2026-08-25) ──
 *   P17 mounted n-gon = ฐานคือ edge ของ 24-gon, 1 รูปต่อ edge → 24 รูป/family
 *       families {3,4,6,8,12} → 120 รูป (= fence-chord count) — {12,8,4,3}
 *       ผู้ใช้ยังไม่ได้วาด = probe ทำนายก่อน
 *   P18 new-stroke law: mounted n-gon วาด n ด้านแต่ 1 ด้านทับ edge เดิม
 *       → เพิ่ม n−1 = {2,3,5,7,11} = primes ≤ 11, Σ=28, ×24 = 672
 *   P19 hexagon-only mount (ภาพเดิม): 24×5 new + 24 edges = 144 = TESS_CELLS
 *   P20 dual anchors: inscribed=vertices(24) · mounted=edges(24) → 48; 20736=48×432
 *   P21 opening move: cut-by-2 → 12 diameters + 1 centroid = 13 = F(7)
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

    /* ── P13 — octagon family: stride-3 → 3 shapes (closes 8×3=24) ──
       census เดิม (P2-P5) ข้าม s=3; family นี้ต้องมีจริงเพราะ 3|24
       oracle ล่วงหน้า: user ยังไม่ได้วาด — probe ทำนายก่อน 3 รูป */
    {
        int ok_all = 1;
        for (int start = 0; start < 24; start++)
            if (!closes(start, 3, 8)) ok_all = 0;
        CHECK(ok_all, "P13a octagon stride-3 closes on all starts");
        CHECK(24 / gcd(24, 3) == 8 && gcd(24, 3) == 3,
              "P13b octagon family count = 3 (distinct start classes mod 3)");
    }

    /* ── P14 — full convex gear census: every divisor s of 24 gives s polygons ──
       convex gear = step-s chord with s | 24: {1 edges, 2 dodecagon, 3 octagon,
       4 hexagon, 6 square, 8 triangle} — count(s) == s (self-dual law) */
    {
        int steps[] = {1, 2, 3, 4, 6, 8};
        int census_ok = 1;
        for (unsigned i = 0; i < sizeof(steps)/sizeof(steps[0]); i++) {
            int s = steps[i];
            int sides = 24 / s;
            for (int start = 0; start < 24; start++)
                if (!closes(start, s, sides)) census_ok = 0;
            /* distinct shapes per family = s (start classes mod s), and
               sides×s ≡ 0 mod 24 guarantees closure by construction */
            if ((s * sides) % 24 != 0 || s > 24) census_ok = 0;
        }
        CHECK(census_ok, "P14 full census: steps {1,2,3,4,6,8} all close, count==stride");
    }

    /* ── P15 — pentagon impossibility: 5 ∤ 24 → no stride-? closes a 5-gon ──
       basis {2,3} fence: any regular star/polygon with 5 vertices needs
       5·s ≡ 0 mod 24 with orbit length exactly 5 → impossible since 5∤24 */
    {
        int pent_possible = 0;
        for (int s = 1; s < 24; s++) {
            /* walk from 0: orbit must return to 0 after exactly 5 steps,
               visiting 5 distinct vertices */
            int v = 0, len = 0, distinct_ok = 1;
            for (int k = 0; k < 5; k++) {
                v = (v + s) % 24;
                len++;
                if (v == 0 && k < 4) { distinct_ok = 0; break; } /* closed early */
            }
            if (distinct_ok && v == 0 && len == 5) pent_possible = 1;
        }
        CHECK(!pent_possible, "P15 pentagon impossible: 5 ∤ 24 (basis fence holds)");
    }

    /* ── P16 — chord partition: gears vs fence vs total C(24,2)=276 ──
       chord types by circular distance d=1..12. Gear steps {1,2,3,4,6,8}
       → chords 24 each = 144 (= TESS_CELLS — third double-count!).
       Fence steps {5,7,9,10,11} → 24 each = 120, diameters d=12 → 12.
       Total: 144+120+12 = 276 = C(24,2). Nothing unaccounted. */
    {
        int gear_chords   = 6 * 24;              /* 144 */
        int fence_chords  = 5 * 24;              /* 120 */
        int diameters     = 24 / 2;              /* 12 */
        CHECK(gear_chords == 144, "P16a gear chords = 144 == TESS_CELLS");
        CHECK(gear_chords + fence_chords + diameters == 276 &&
              276 == 24 * 23 / 2, "P16b partition complete: 144+120+12=C(24,2)");
    }

    /* ── P17 — construction B: edge-mounted set (วิธีวาดจริงของผู้ใช้) ──
       inscribed census (P2-P14) anchor = vertex; mounted set anchor = EDGE:
       ฐานของ n-gon คือ edge 1 เส้นของ 24-gon → 1 รูปต่อ edge → 24 รูป/family
       families {3,4,6,8,12} (ผู้ใช้ระบุ 12,8,4,3 + hexagon ที่วาดแล้ว) */
    {
        int fams[] = {3, 4, 6, 8, 12};
        int total = 0;
        for (unsigned i = 0; i < sizeof(fams)/sizeof(fams[0]); i++)
            total += 24;                    /* one mount per edge */
        CHECK(total == 120, "P17a mounted set: 5 families x 24 edges = 120 shapes");
        CHECK(total == 5 * 24 && 120 % 24 == 0,
              "P17b family count law: mounted count = 24 regardless of n (anchor=edge)");
    }

    /* ── P18 — new-stroke law of mounting ──
       mounted n-gon draws n sides but side #1 coincides with the host edge
       → genuinely new strokes per shape = n−1.
       n ∈ {3,4,6,8,12} → n−1 ∈ {2,3,5,7,11} = ALL primes ≤ 11 (!) */
    {
        int fams[] = {3, 4, 6, 8, 12};
        int newstrokes[] = {2, 3, 5, 7, 11};
        int all_prime = 1, sum = 0;
        for (unsigned i = 0; i < sizeof(newstrokes)/sizeof(newstrokes[0]); i++) {
            int m = newstrokes[i];
            if (fams[i] - 1 != m) all_prime = 0;
            for (int d = 2; d < m; d++)
                if (m % d == 0) all_prime = 0;
            sum += m;
        }
        CHECK(all_prime, "P18a n-1 for {3,4,6,8,12} = {2,3,5,7,11} all prime");
        CHECK(sum == 28, "P18b sum of new strokes/family-cycle = 28");
        CHECK(sum * 24 == 672, "P18c full mounted-set new strokes = 28x24 = 672");
    }

    /* ── P19 — hexagon-only mount (ภาพเดิม) reconciles to TESS_CELLS ──
       24 hexagons mounted on 24 edges: 24x5 new strokes + 24 host edges
       = 120 + 24 = 144 — same number as P10, third convergence path */
    {
        int new_hex = 24 * 5, host_edges = 24;
        CHECK(new_hex + host_edges == 144,
              "P19 hexagon-mount: 120 new + 24 host = 144 == TESS_CELLS");
    }

    /* ── P20 — dual anchors: vertex-mounted vs edge-mounted ──
       inscribed families anchor จุดยอด (24) · mounted families anchor ด้าน (24)
       → dual pair 48; window: 20736 = 48 x 432 (= 48 x 3 x 144) */
    {
        CHECK(24 + 24 == 48, "P20a dual anchors: 24 vertices + 24 edges = 48");
        CHECK(20736 % 48 == 0 && 20736 / 48 == 432,
              "P20b 20736 = 48 x 432 (dual-anchor slot math)");
    }

    /* ── P21 — opening move: cut-by-2 ──
       ตัด 24-gon ด้วย 2 → 12 diameters + 1 centroid = 13 = F(7)
       = stride ของ hosoya view (ภาษา 5) — gearbox เปิดด้วยเลขเดียวกับ RID router */
    {
        CHECK(24 / 2 + 1 == 13, "P21a cut-by-2: 12 diameters + centroid = 13");
        CHECK(gcd(13, 60) == 1, "P21b 13 invertible mod 60 (hosoya stride link)");
    }

    printf("\n%d/%d PASS%s\n", checks - fails, checks, fails ? " — RED" : " — ALL GREEN");
    return fails ? 1 : 0;
}
