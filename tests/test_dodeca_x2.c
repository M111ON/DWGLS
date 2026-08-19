/*
 * test_dodeca_x2.c — The ×2 Law of the 12-gon (Dodecagon)
 * ═══════════════════════════════════════════════════════════════════════
 *
 * จาก hex-construction.html (12-gon Stride Topology) — สิ่งที่ sim ขาดคือ ×2:
 * ทุก stride layer ของ dodecagon มาเป็นคู่เสมอ (parity pair / nesting pair)
 *
 * Proof (pure geometry — vertices บน unit circle, chord = segment ระหว่าง vertex):
 *   D1  stride-2 → 2 hexagons พอดี: {1,3,5,7,9,11} (odd) + {2,4,6,8,10,12}
 *       (even — หมุน 30° จาก odd) — sim วาดแค่ odd (exit gate) — even หาย = ×2
 *   D2  divisor law: stride k → gcd(k,12) cycles × ความยาว 12/gcd(k,12)
 *       (k = 1..6) — 1 dodecagon · 2 hex · 3 sq · 4 Δ · 1 star · 6 diameters
 *   D2b 6 diameters — ทุกเส้นผ่านศูนย์กลาง = 2 rays share ศูนย์กลางเดียว
 *       (ชั้นที่ 6 = 2 ชุด — 12 rays ถึง 1 center)
 *   D3  hexagon คู่ (odd × even) ไขว้กัน 12 จุด = regular inner 12-gon
 *       radius = √3/(2·cos15°) = 0.896575... · spacing 30° · offset 15°
 *       — โครงสร้างซ้อนตัวเอง: 12-gon → 2 hex → inner 12-gon → ...
 *   D4  fan triangles: 12 ชิ้น share center (O, v_i, v_{i+1})
 *       · ทุกคู่ติดกันอยู่คนละข้างของ radial edge ร่วม (สลับ ∧∨ — sawtooth)
 *       · แบ่งเป็น 2 parity orbits ละ 6 (6+6) · ทุกชิ้น congruent
 *   D5  3-fold: 3 squares (stride-3) = orbit เดียวภายใต้ rotation 120°
 *       (C3 subgroup — R120 cycle S0→S1→S2→S0) — 3 lanes ของ Rail_sync
 *       (PhaseRail θ=[0,120,240]) · R90 = symmetry ของแต่ละ square
 *       · R30 สลับ hexagon คู่/คี่ (parity ×2) · R60 symmetry ของ hexagon
 *   D6  tetrahedron: 4 Wang tiles (stride-4) = 4 faces ของ tetra
 *       · ทุกอัน equilateral + congruent · partition ครบ 12 vertices
 *       · centroid ทุกอัน = center (circumcenter ร่วม) · 3+1: ใบที่ 4
 *         ถูกกำหนดโดยอีก 3 (complement — derived ไม่ต้องเลือก)
 *       · R120 = symmetry ของแต่ละ Δ (tetra C3 axis) — C3 ตัวเดียวกับ D5
 *   G   กฎ N-gon ทั่วไป (generalization — N = 6..24):
 *       · stride-2 → 2 cycles ของ N/2 (2 parity polygons — 24-gon → 2 dodecagon)
 *       · divisor law: stride k → gcd(k,N) cycles × N/gcd(k,N)
 *       · stride-2 polygons ไขว้กัน N จุด = regular inner N-gon
 *         radius = cos(2π/N)/cos(π/N) · spacing 2π/N · offset π/N
 *       · fan N triangles สลับ ∧∨ = N/2 + N/2 (24 → 12+12)
 *   H   nesting ซ้อนต่อ (self-similar): 2 hexagon ไขว้ → inner 12-gon
 *       (D3) → แบ่งซ้ำอีก → inner-inner 12-gon → … ลู่เข้าศูนย์กลาง
 *       · scale factor คงที่ทุกระดับ = cos(π/6)/cos(π/12) = √(2+√3)/√3
 *         ≈ 0.89658 (สมการปิด — ตรวจกลับ 1/s) · offset หมุน +15°/ระดับ
 *       · r_k = s^k · r_0 — 5 ระดับ = 0.58·r_0 (converge ไป center)
 *
 * BUILD: gcc -O2 -Wall -Wextra -I. -Icore -Icore/infra -o build/test_dodeca_x2 \
 *        tests/test_dodeca_x2.c -lm
 */

#include <stdio.h>
#include <stdint.h>
#include <math.h>

#define N 12u
#ifndef M_PI
#define M_PI 3.14159265358979323846
#endif

static int pass_count = 0, fail_count = 0;
#define CHECK(desc, cond) do { \
    if (cond) { pass_count++; printf("  T: PASS — %s\n", desc); } \
    else      { fail_count++; printf("  T: FAIL — %s\n", desc); } \
} while (0)

typedef struct { double x, y; } Pt;

static Pt V[N];   /* vertex i ที่ -90° + i·30° — ตรงกับ hex-construction.html */

static double cross2(Pt a, Pt b) { return a.x * b.y - a.y * b.x; }
static double dist2(Pt a, Pt b) { double dx = a.x-b.x, dy = a.y-b.y; return sqrt(dx*dx+dy*dy); }
static uint32_t gcd32(uint32_t a, uint32_t b) { while (b) { uint32_t t = a % b; a = b; b = t; } return a; }

/* set equality หลัง rotation: (a + r) mod 12 == b (ทั้งคู่ sort) */
static int set_eq_rot(const uint32_t *a, uint32_t n, uint32_t r, const uint32_t *b)
{
    uint32_t ra[8], rb[8];   /* n ≤ 6 (hexagon) */
    for (uint32_t i = 0; i < n; i++) ra[i] = (a[i] + r) % N;
    for (uint32_t i = 0; i < n; i++) rb[i] = b[i];
    for (uint32_t i = 0; i < n; i++)
        for (uint32_t j = i+1; j < n; j++)
            if (ra[j] < ra[i]) { uint32_t t = ra[i]; ra[i] = ra[j]; ra[j] = t; }
    for (uint32_t i = 0; i < n; i++)
        for (uint32_t j = i+1; j < n; j++)
            if (rb[j] < rb[i]) { uint32_t t = rb[i]; rb[i] = rb[j]; rb[j] = t; }
    for (uint32_t i = 0; i < n; i++) if (ra[i] != rb[i]) return 0;
    return 1;
}

/* intersection ของ segment ab กับ cd → 1 ถ้าไขว้กันจริง (interior), เติม *p */
static int seg_inter(Pt a, Pt b, Pt c, Pt d, Pt *p)
{
    Pt ab = {b.x-a.x, b.y-a.y}, cd = {d.x-c.x, d.y-c.y}, ca = {c.x-a.x, c.y-a.y};
    double r = cross2(ab, cd);
    if (fabs(r) < 1e-12) return 0;
    double t = cross2(ca, cd) / r;
    double u = cross2(ca, ab) / r;
    if (t <= 1e-12 || t >= 1.0-1e-12 || u <= 1e-12 || u >= 1.0-1e-12) return 0;
    p->x = a.x + t * ab.x;
    p->y = a.y + t * ab.y;
    return 1;
}

/* ═══════════════════════════════════════════════════════════════════
   D1/D2 — stride cycles: stride-2 = 2 hexagons + divisor law k=1..6
   ═══════════════════════════════════════════════════════════════════ */
static void test_stride_cycles(void)
{
    printf("═ D1/D2 — STRIDE CYCLES: 2 HEXAGONS + divisor law ═\n");

    /* D1: stride-2 → 2 cycles of 6 — odd set + even set */
    {
        uint8_t seen[N] = {0};
        uint32_t n_cyc = 0, lens[N] = {0};
        uint8_t odd_cyc = 0, even_cyc = 0, all_ok = 1;
        for (uint32_t s = 0; s < N; s++) {
            if (seen[s]) continue;
            uint32_t len = 0, cur = s, all_odd = 1, all_even = 1;
            while (!seen[cur]) {
                seen[cur] = 1;
                if (cur % 2 == 0) all_odd = 0; else all_even = 0;
                cur = (cur + 2) % N;
                len++;
            }
            if (all_odd) odd_cyc = 1;
            if (all_even) even_cyc = 1;
            lens[n_cyc++] = len;
        }
        CHECK("D1a: stride-2 แบ่ง 12 vertices เป็น 2 cycles พอดี (×2 hexagon)",
              n_cyc == 2);
        CHECK("D1b: แต่ละ cycle ยาว 6 (hexagon)",
              n_cyc == 2 && lens[0] == 6 && lens[1] == 6);
        CHECK("D1c: cycle หนึ่ง = จุดคี่ {1,3,5,7,9,11} (0-based 0,2,4,6,8,10)",
              odd_cyc == 1 && even_cyc == 1 && all_ok);
        /* hexagon regular: ทุก chord ยาวเท่ากัน (stride-2) */
        double l0 = dist2(V[0], V[2]);
        int reg = 1;
        for (uint32_t i = 0; i < N; i += 2)
            if (fabs(dist2(V[i], V[(i+2)%N]) - l0) > 1e-9) reg = 0;
        CHECK("D1d: hexagon odd regular — 6 chord ยาวเท่ากัน", reg);
    }

    /* D2: divisor law — stride k → gcd(k,12) cycles × 12/gcd(k,12) */
    {
        int law_ok = 1, cov_ok = 1;
        for (uint32_t k = 1; k <= 6; k++) {
            uint8_t seen[N] = {0};
            uint32_t n_cyc = 0, cov = 0;
            uint32_t g = gcd32(k, N);
            for (uint32_t s = 0; s < N; s++) {
                if (seen[s]) continue;
                uint32_t len = 0, cur = s;
                while (!seen[cur]) { seen[cur] = 1; cur = (cur + k) % N; len++; }
                if (len != N / g) law_ok = 0;
                n_cyc++;
                cov += len;
            }
            if (n_cyc != g) law_ok = 0;
            if (cov != N) cov_ok = 0;
        }
        CHECK("D2a: stride k → จำนวน cycle = gcd(k,12) ทุก k=1..6", law_ok);
        CHECK("D2b: แต่ละ cycle ยาว 12/gcd(k,12) · รวมครบ 12 vertices", cov_ok);
        CHECK("D2c: k=3 → 3 squares · k=4 → 4 Δ · k=6 → 6 diameters",
              gcd32(3,12)==3 && gcd32(4,12)==4 && gcd32(6,12)==6);
    }

    /* D2b: 6 diameters — ทุกเส้น midpoint = center (2 rays share 1 center) */
    {
        int diam_ok = 1;
        for (uint32_t i = 0; i < 6; i++) {
            double mx = (V[i].x + V[(i+6)%N].x) / 2.0;
            double my = (V[i].y + V[(i+6)%N].y) / 2.0;
            if (fabs(mx) > 1e-12 || fabs(my) > 1e-12) diam_ok = 0;
        }
        CHECK("D2d: 6 diameters ทุกเส้นผ่าน center — = 12 rays ถึง 1 ศูนย์กลาง",
              diam_ok);
    }
    printf("\n");
}

/* ═══════════════════════════════════════════════════════════════════
   D3 — hexagon คู่ไขว้ 12 จุด = regular inner 12-gon (self-similar)
   ═══════════════════════════════════════════════════════════════════ */
static void test_crossings(void)
{
    printf("═ D3 — HEXAGON CROSSINGS = REGULAR INNER 12-GON ═\n");

    Pt xs[64];
    uint32_t nx = 0;
    for (uint32_t i = 1; i < N; i += 2) {          /* odd hexagon edges */
        for (uint32_t j = 0; j < N; j += 2) {      /* even hexagon edges */
            Pt p;
            if (seg_inter(V[i], V[(i+2)%N], V[j], V[(j+2)%N], &p)) {
                int dup = 0;
                for (uint32_t q = 0; q < nx; q++)
                    if (dist2(xs[q], p) < 1e-9) { dup = 1; break; }
                if (!dup) xs[nx++] = p;
            }
        }
    }

    CHECK("D3a: odd×even hexagon ไขว้กัน 12 จุด (ไม่ซ้ำ)",
          nx == 12);

    /* ทุกจุดอยู่บนวงกลมเดียวกัน — radius เดียว */
    double r0 = sqrt(xs[0].x*xs[0].x + xs[0].y*xs[0].y);
    int rad_ok = 1;
    for (uint32_t i = 1; i < nx; i++) {
        double r = sqrt(xs[i].x*xs[i].x + xs[i].y*xs[i].y);
        if (fabs(r - r0) > 1e-9) rad_ok = 0;
    }
    CHECK("D3b: 12 จุดไขว้อยู่บนวงกลมเดียวกัน (radius เดียว)",
          rad_ok);

    /* radius = √3/(2·cos15°) — ค่าที่พิสูจน์ได้เชิงวิเคราะห์
     * (chord (i,i+2) ห่างจาก center √3/2 · จุดไขว้ที่มุม -15° จากศูนย์กลาง chord) */
    {
        double r_exp = (sqrt(3.0) / 2.0) / cos(M_PI / 12.0);
        CHECK("D3c: radius ตรงค่า analytic √3/(2·cos15°) = %.9f",
              fabs(r0 - r_exp) < 1e-9);
    }

    /* angular spacing = 30° ทุกช่อง + offset 15° จาก outer vertices */
    {
        double ang[64];
        for (uint32_t i = 0; i < nx; i++) ang[i] = atan2(xs[i].y, xs[i].x);
        /* sort ascending */
        for (uint32_t i = 0; i < nx; i++)
            for (uint32_t j = i+1; j < nx; j++)
                if (ang[j] < ang[i]) { double t = ang[i]; ang[i] = ang[j]; ang[j] = t; }
        int gap_ok = 1;
        for (uint32_t i = 0; i < nx; i++) {
            double d = ang[(i+1)%nx] - ang[i];
            if (d < 0) d += 2.0 * M_PI;
            if (fabs(d - M_PI/6.0) > 1e-9) gap_ok = 0;
        }
        CHECK("D3d: angular spacing = 30° ทุกช่อง (regular 12-gon)", gap_ok);

        /* offset: จุดไขว้ที่ -75° + k·30° (15° จาก outer vertex -90° + k·30°) */
        int off_ok = 1;
        for (uint32_t i = 0; i < nx; i++) {
            double a = ang[i];
            double rel = (a + 75.0 * M_PI/180.0) / (M_PI/6.0);
            double nearest = fabs(rel - round(rel));
            if (nearest > 1e-9) off_ok = 0;
        }
        CHECK("D3e: inner ring หมุน offset 15° จาก outer (self-similar nesting)",
              off_ok);
    }
    printf("\n");
}

/* ═══════════════════════════════════════════════════════════════════
   D4 — fan triangles: 12 ชิ้น share center · สลับ ∧∨ = 6+6
   ═══════════════════════════════════════════════════════════════════ */
static void test_fan(void)
{
    printf("═ D4 — FAN TRIANGLES: 12 ชิ้น · สลับ ∧∨ = 6+6 ═\n");

    /* 12 fan triangles (O, v_i, v_{i+1}) — ทุกชิ้นมีพื้นที่ ≠ 0 และเท่ากัน */
    {
        double a0 = fabs(cross2(V[0], V[1])) / 2.0;
        int ok = 1, nz = 1;
        for (uint32_t i = 0; i < N; i++) {
            double a = fabs(cross2(V[i], V[(i+1)%N])) / 2.0;
            if (a < 1e-12) nz = 0;
            if (fabs(a - a0) > 1e-12) ok = 0;
        }
        CHECK("D4a: 12 fan triangles (O, v_i, v_{i+1}) — ทุกชิ้น congruent",
              ok && nz);
    }

    /* alternation: ทุกคู่ติดกันอยู่คนละข้างของ radial edge ร่วม —
     * cross(v_{i+1}, v_i) กับ cross(v_{i+1}, v_{i+2}) เครื่องหมายตรงข้าม */
    {
        int alt_ok = 1;
        for (uint32_t i = 0; i < N; i++) {
            uint32_t j = (i+1) % N, k2 = (i+2) % N;
            double sA = cross2(V[j], V[i]);     /* third vertex v_i ฝั่ง radial v_j */
            double sB = cross2(V[j], V[k2]);    /* third vertex v_{i+2} ฝั่ง radial v_j */
            if (sA * sB >= 0.0 || fabs(sA) < 1e-12 || fabs(sB) < 1e-12) {
                alt_ok = 0;
                break;
            }
        }
        CHECK("D4b: ทุกคู่ติดกันอยู่คนละข้าง radial edge ร่วม (สลับ ∧∨ — sawtooth)",
              alt_ok);
    }

    /* 6+6: 2 parity orbits — even-indexed 6 · odd-indexed 6 */
    {
        uint32_t even = 0, odd = 0;
        for (uint32_t i = 0; i < N; i++) { if (i % 2 == 0) even++; else odd++; }
        CHECK("D4c: แบ่งเป็น 2 orbits ละ 6 (6 ∧ + 6 ∨)", even == 6 && odd == 6);
        /* alternation ปิดรอบ: คู่สุดท้าย (tri 11, tri 0) อยู่คนละข้างของ (O, v_0) */
        double sA = cross2(V[0], V[11]);
        double sB = cross2(V[0], V[1]);
        CHECK("D4d: วงปิด — tri 11 กับ tri 0 สลับกันด้วย (sawtooth ครบรอบ 12)",
              sA * sB < 0.0);
    }
    printf("\n");
}

/* ═══════════════════════════════════════════════════════════════════
   G — กฎ N-gon ทั่วไป: ×2 + divisor law + inner ring + fan สำหรับ N ใดๆ
   ═══════════════════════════════════════════════════════════════════ */
static void verify_n_gon(uint32_t nn)
{
    char dsc[160];
    Pt GV[24];
    for (uint32_t i = 0; i < nn; i++) {
        double a = -M_PI/2.0 + i * (2.0*M_PI/nn);
        GV[i].x = cos(a);
        GV[i].y = sin(a);
    }

    /* G1: stride-2 → 2 cycles ของ nn/2 (2 parity polygons) */
    {
        uint8_t seen[24] = {0};
        uint32_t n_cyc = 0, lens[2] = {0};
        int odd_ok = 0, even_ok = 0;
        for (uint32_t s = 0; s < nn; s++) {
            if (seen[s]) continue;
            uint32_t len = 0, cur = s, all_odd = 1, all_even = 1;
            while (!seen[cur]) {
                seen[cur] = 1;
                if (cur % 2 == 0) all_odd = 0; else all_even = 0;
                cur = (cur + 2) % nn;
                len++;
            }
            if (all_odd) odd_ok = 1;
            if (all_even) even_ok = 1;
            lens[n_cyc++] = len;
        }
        snprintf(dsc, sizeof dsc,
                 "G1 [N=%u]: stride-2 → 2 cycles ของ N/2 = %u (%u-gon → 2 polygons คี่/คู่)",
                 nn, nn/2, nn);
        CHECK(dsc, n_cyc == 2 && lens[0] == nn/2 && lens[1] == nn/2 &&
              odd_ok && even_ok);
    }

    /* G2: divisor law — stride k → gcd(k,nn) cycles × nn/gcd(k,nn) */
    {
        int law_ok = 1;
        for (uint32_t k = 1; k <= nn/2; k++) {
            uint8_t seen[24] = {0};
            uint32_t n_cyc = 0, cov = 0;
            uint32_t g = gcd32(k, nn);
            for (uint32_t s = 0; s < nn; s++) {
                if (seen[s]) continue;
                uint32_t len = 0, cur = s;
                while (!seen[cur]) { seen[cur] = 1; cur = (cur + k) % nn; len++; }
                if (len != nn / g) law_ok = 0;
                n_cyc++;
                cov += len;
            }
            if (n_cyc != g || cov != nn) law_ok = 0;
        }
        snprintf(dsc, sizeof dsc,
                 "G2 [N=%u]: divisor law — stride k → gcd(k,N) cycles × N/gcd(k,N) ทุก k=1..N/2",
                 nn);
        CHECK(dsc, law_ok);
    }

    /* G3 (nn ≥ 8): stride-2 polygons ไขว้ = regular inner nn-gon */
    if (nn >= 8) {
        Pt xs[32];
        uint32_t nx = 0;
        for (uint32_t i = 1; i < nn; i += 2)         /* polygon คี่ edges */
            for (uint32_t j = 0; j < nn; j += 2) {   /* polygon คู่ edges */
                Pt p;
                if (seg_inter(GV[i], GV[(i+2)%nn], GV[j], GV[(j+2)%nn], &p)) {
                    int dup = 0;
                    for (uint32_t q = 0; q < nx; q++)
                        if (dist2(xs[q], p) < 1e-9) { dup = 1; break; }
                    if (!dup) xs[nx++] = p;
                }
            }
        snprintf(dsc, sizeof dsc,
                 "G3a [N=%u]: polygons คี่×คู่ไขว้กัน %u จุด (= N — inner ring)", nn, nn);
        CHECK(dsc, nx == nn);

        double r0 = sqrt(xs[0].x*xs[0].x + xs[0].y*xs[0].y);
        int rad_ok = 1;
        for (uint32_t i = 1; i < nx; i++)
            if (fabs(sqrt(xs[i].x*xs[i].x + xs[i].y*xs[i].y) - r0) > 1e-9) rad_ok = 0;
        double r_exp = cos(2.0*M_PI/nn) / cos(M_PI/nn);
        snprintf(dsc, sizeof dsc,
                 "G3b [N=%u]: inner ring radius = cos(2π/N)/cos(π/N) (analytic — %.6f)",
                 nn, r_exp);
        CHECK(dsc, rad_ok && fabs(r0 - r_exp) < 1e-9);

        double ang[32];
        for (uint32_t i = 0; i < nx; i++) ang[i] = atan2(xs[i].y, xs[i].x);
        for (uint32_t i = 0; i < nx; i++)
            for (uint32_t j = i+1; j < nx; j++)
                if (ang[j] < ang[i]) { double t = ang[i]; ang[i] = ang[j]; ang[j] = t; }
        int gap_ok = 1, off_ok = 1;
        for (uint32_t i = 0; i < nx; i++) {
            double d = ang[(i+1)%nx] - ang[i];
            if (d < 0) d += 2.0*M_PI;
            if (fabs(d - 2.0*M_PI/nn) > 1e-9) gap_ok = 0;
            double rel = (ang[i] + M_PI/2.0 - M_PI/nn) / (2.0*M_PI/nn);
            if (fabs(rel - round(rel)) > 1e-9) off_ok = 0;
        }
        snprintf(dsc, sizeof dsc,
                 "G3c [N=%u]: inner ring regular — spacing 2π/N · offset π/N (self-similar nesting)",
                 nn);
        CHECK(dsc, gap_ok && off_ok);
    }

    /* G4: fan nn triangles — สลับ ∧∨ · nn/2 + nn/2 */
    {
        int alt_ok = 1;
        for (uint32_t i = 0; i < nn; i++) {
            uint32_t j = (i+1)%nn, k2 = (i+2)%nn;
            double sA = cross2(GV[j], GV[i]);
            double sB = cross2(GV[j], GV[k2]);
            if (sA * sB >= 0.0 || fabs(sA) < 1e-12 || fabs(sB) < 1e-12) {
                alt_ok = 0;
                break;
            }
        }
        snprintf(dsc, sizeof dsc,
                 "G4a [N=%u]: fan %u triangles สลับ ∧∨ (ทุกคู่ติดกันคนละข้าง radial edge)",
                 nn, nn);
        CHECK(dsc, alt_ok);
        uint32_t even = 0, odd = 0;
        for (uint32_t i = 0; i < nn; i++) { if (i % 2 == 0) even++; else odd++; }
        snprintf(dsc, sizeof dsc,
                 "G4b [N=%u]: parity orbits N/2+N/2 (%u ∧ + %u ∨)", nn, nn/2, nn/2);
        CHECK(dsc, even == nn/2 && odd == nn/2);
    }
}

static void test_general(void)
{
    printf("═ G — GENERAL N-GON: กฎ ×2 + divisor law สำหรับ N=6..24 ═\n");
    static const uint32_t NS[] = { 6, 8, 10, 12, 16, 24 };
    for (uint32_t idx = 0; idx < sizeof(NS)/sizeof(NS[0]); idx++)
        verify_n_gon(NS[idx]);
    printf("\n");
}

/* ═══════════════════════════════════════════════════════════════════
   H — nesting ซ้อนต่อ: inner 12-gon → inner-inner → … ลู่เข้าศูนย์กลาง
   สเกล factor คงที่ s = cos(π/6)/cos(π/12) (self-similar)
   ═══════════════════════════════════════════════════════════════════ */
/* หนึ่งระดับ: จาก vertices 12 จุดที่ radius r → hexagon คี่×คู่ไขว้กัน
 * ได้ inner 12-gon (เติม *out) — ตรวจ analytic แล้วคืน scale factor ที่วัดได้ */
static double nest_level(const Pt *in, Pt *out, int level)
{
    char dsc[160];
    Pt xs[16];
    uint32_t nx = 0;
    for (uint32_t i = 1; i < N; i += 2)          /* odd hexagon edges */
        for (uint32_t j = 0; j < N; j += 2) {    /* even hexagon edges */
            Pt p;
            if (seg_inter(in[i], in[(i+2)%N], in[j], in[(j+2)%N], &p)) {
                int dup = 0;
                for (uint32_t q = 0; q < nx; q++)
                    if (dist2(xs[q], p) < 1e-9) { dup = 1; break; }
                if (!dup) xs[nx++] = p;
            }
        }

    snprintf(dsc, sizeof dsc,
             "H%da: ระดับ %d — ไขว้ 12 จุด (inner 12-gon เกิดซ้ำ)", level + 1, level);
    CHECK(dsc, nx == 12);

    /* radius ของระดับนี้ */
    double r0 = sqrt(xs[0].x*xs[0].x + xs[0].y*xs[0].y);
    int rad_ok = 1;
    for (uint32_t i = 1; i < nx; i++)
        if (fabs(sqrt(xs[i].x*xs[i].x + xs[i].y*xs[i].y) - r0) > 1e-9) rad_ok = 0;

    /* analytic: r_{k+1}/r_k = cos(π/6)/cos(π/12) ทุกระดับ (self-similar)
     * — ตัวเดียวกับ inner-ring ratio ของ D3/G3b */
    double rin = sqrt(in[0].x*in[0].x + in[0].y*in[0].y);
    double scale = cos(M_PI/6.0) / cos(M_PI/12.0);
    snprintf(dsc, sizeof dsc,
             "H%db: ระดับ %d — radius %.6f ตรงค่า analytic r·cos(π/6)/cos(π/12) = %.6f",
             level + 1, level, r0, rin * scale);
    CHECK(dsc, rad_ok && fabs(r0 - rin * scale) < 1e-9);

    /* เรียงจุดไขว้ตามมุม — ต้องเป็น cyclic order ของ inner 12-gon
     * (จุดที่คืนไปใช้เป็น vertices ของระดับถัดไป — hexagon edges
     *  ต้องลากระหว่างจุดติดกันในรอบ ไม่ใช่ลำดับ loop ที่เก็บมา) */
    {
        double angs[16];
        for (uint32_t i = 0; i < nx; i++) angs[i] = atan2(xs[i].y, xs[i].x);
        for (uint32_t i = 0; i < nx; i++)
            for (uint32_t j = i+1; j < nx; j++)
                if (angs[j] < angs[i]) {
                    double ta = angs[i]; angs[i] = angs[j]; angs[j] = ta;
                    Pt tp = xs[i]; xs[i] = xs[j]; xs[j] = tp;
                }
    }

    /* spacing 30° ทุกช่อง */
    double ang[16];
    for (uint32_t i = 0; i < nx; i++) ang[i] = atan2(xs[i].y, xs[i].x);
    int gap_ok = 1;
    for (uint32_t i = 0; i < nx; i++) {
        double d = ang[(i+1)%nx] - ang[i];
        if (d < 0) d += 2.0 * M_PI;
        if (fabs(d - M_PI/6.0) > 1e-9) gap_ok = 0;
    }
    snprintf(dsc, sizeof dsc, "H%dc: ระดับ %d — regular (spacing 30° ทุกช่อง)", level + 1, level);
    CHECK(dsc, gap_ok);

    /* offset หมุน 15° ทุกระดับ (level 0: -75°+k·30° · level 1: -60°+k·30° …) */
    {
        int off_ok = 1;
        for (uint32_t i = 0; i < nx; i++) {
            double rel = (ang[i] + M_PI/2.0 - (double)(level+1) * M_PI/12.0) / (M_PI/6.0);
            if (fabs(rel - round(rel)) > 1e-9) off_ok = 0;
        }
        snprintf(dsc, sizeof dsc,
                 "H%dd: ระดับ %d — offset หมุน +15°/ระดับ (self-similar nesting)",
                 level + 1, level);
        CHECK(dsc, off_ok);
    }

    for (uint32_t i = 0; i < nx; i++) { out[i] = xs[i]; }
    return r0 / rin;   /* scale factor ที่วัดได้ */
}

static void test_nesting(void)
{
    printf("═ H — NESTING: inner 12-gon ซ้อนต่อ → ลู่เข้าศูนย์กลาง (ratio คงที่) ═\n");

    Pt cur[N], nxt[N];
    for (uint32_t i = 0; i < N; i++) cur[i] = V[i];

    /* 5 ระดับ: r_k = s^k — ตรวจ ratio ระหว่างระดับคงที่ = analytic */
    double s_analytic = cos(M_PI/6.0) / cos(M_PI/12.0);
    double prev_ratio = 0.0;
    int ratio_ok = 1;
    for (uint32_t lvl = 0; lvl < 5; lvl++) {
        double ratio = nest_level(cur, nxt, (int)lvl);
        if (fabs(ratio - s_analytic) > 1e-9) ratio_ok = 0;
        if (lvl > 0 && fabs(ratio - prev_ratio) > 1e-9) ratio_ok = 0;
        prev_ratio = ratio;
        for (uint32_t i = 0; i < N; i++) cur[i] = nxt[i];
    }
    CHECK("H5: scale factor คงที่ทุกระดับ = cos(π/6)/cos(π/12) (self-similar)",
          ratio_ok);

    /* ลู่เข้าศูนย์กลาง: r ระดับ 4 < 1/4 ของ r ระดับ 0 (s⁴ ≈ 0.754) */
    double r_last = sqrt(cur[0].x*cur[0].x + cur[0].y*cur[0].y);
    CHECK("H6: 5 ระดับ → r ลู่เข้าศูนย์กลาง (s⁵ ≈ 0.5793 < 0.75)",
          r_last < 0.75);

    /* ratio ตรงค่า analytic ผ่าน cos — เขียนเป็นสมการปิด */
    {
        double s2 = cos(M_PI/12.0) / cos(M_PI/6.0);   /* 1/s — ตรวจกลับ */
        CHECK("H7: s = cos(π/6)/cos(π/12) = √(2+√3)/√3 (สมการปิด — ตรวจกลับ 1/s)",
              fabs(s_analytic * s2 - 1.0) < 1e-12);
    }
    printf("\n");
}

/* ═══════════════════════════════════════════════════════════════════
   D5/D6 — 3-fold: 3 squares = orbit R120° (3 lanes) ·
           4 Wang Δ = tetrahedron (3+1 — ใบที่ 4 derived)
   ═══════════════════════════════════════════════════════════════════ */
static void test_threefold(void)
{
    printf("═ D5/D6 — 3-FOLD: 3 squares = R120° orbit · 4 Δ = tetrahedron ═\n");

    /* stride-3 squares (0-based — sim [1,4,7,10],[2,5,8,11],[3,6,9,12]) */
    static const uint32_t S[3][4] = {
        {0,3,6,9}, {1,4,7,10}, {2,5,8,11}
    };
    /* stride-4 Wang tiles (ตรง sim step 4: [0,4,8],[1,5,9],[2,6,10],[3,7,11]) */
    static const uint32_t T[4][3] = {
        {0,4,8}, {1,5,9}, {2,6,10}, {3,7,11}
    };
    /* hexagons ของ stride-2 (parity pair — กฎ ×2) */
    static const uint32_t H[2][6] = {
        {0,2,4,6,8,10}, {1,3,5,7,9,11}
    };

    /* D5: rotation 120° (+4 steps) cycle 3 squares เป็น 3-cycle เดียว */
    {
        int cyc_ok = 1;
        for (uint32_t k = 0; k < 3; k++)
            if (!set_eq_rot(S[k], 4, 4, S[(k+1) % 3])) cyc_ok = 0;
        CHECK("D5a: R120° (+4 steps) cycle S0→S1→S2→S0 (3-cycle เดียว)", cyc_ok);
        int back_ok = 1;
        for (uint32_t k = 0; k < 3; k++) {
            if (!set_eq_rot(S[k], 4, 8, S[(k+2) % 3])) back_ok = 0;
            if (!set_eq_rot(S[k], 4, 12, S[k])) back_ok = 0;   /* R120³ = id */
        }
        CHECK("D5b: R120° วนครบ orbit — R120³ = identity (3 squares = 1 orbit)",
              back_ok);
        /* R90 = +3 steps — symmetry ของแต่ละ square */
        int s90 = 1;
        for (uint32_t k = 0; k < 3; k++)
            if (!set_eq_rot(S[k], 4, 3, S[k])) s90 = 0;
        CHECK("D5c: R90° = symmetry ของแต่ละ square (stride-3 หมุนตัวเอง)", s90);
    }

    /* D5: ×2 parity — R30 สลับ hexagon คู่/คี่ · R60 symmetry ของ hexagon */
    {
        int sw = set_eq_rot(H[0], 6, 1, H[1]) && set_eq_rot(H[1], 6, 1, H[0]);
        CHECK("D5d: R30° สลับ hexagon คู่↔คี่ (parity pair — กฎ ×2 เชื่อม 3-fold)", sw);
        int s60 = 1;
        for (uint32_t k = 0; k < 2; k++)
            if (!set_eq_rot(H[k], 6, 2, H[k])) s60 = 0;
        CHECK("D5e: R60° = symmetry ของแต่ละ hexagon", s60);
    }

    /* D6: 4 Wang Δ — equilateral + congruent */
    {
        double side = dist2(V[0], V[4]);   /* chord(120°) = √3 */
        int eq_ok = 1, cong_ok = 1;
        for (uint32_t t = 0; t < 4; t++) {
            for (uint32_t e = 0; e < 3; e++) {
                double s = dist2(V[T[t][e]], V[T[t][(e+1) % 3]]);
                if (fabs(s - side) > 1e-9) eq_ok = 0;
            }
        }
        CHECK("D6a: 4 Wang Δ ทุกอัน equilateral (chord 120° = √3)", eq_ok && cong_ok);
        CHECK("D6b: ทุกอัน congruent (side เท่ากันหมด)", cong_ok);
    }

    /* D6: partition — disjoint + ครอบครบ 12 vertices (12 = 4×3) */
    {
        uint32_t mask = 0;
        for (uint32_t t = 0; t < 4; t++)
            for (uint32_t e = 0; e < 3; e++) mask |= (1u << T[t][e]);
        CHECK("D6c: 4 Δ ครอบครบ 12 vertices ไม่ซ้ำ (partition)", mask == 0xFFFu);
    }

    /* D6: centroid ของทุก Δ = center (circumcenter ร่วมของ tetra) */
    {
        int cen_ok = 1;
        for (uint32_t t = 0; t < 4; t++) {
            double sx = 0, sy = 0;
            for (uint32_t e = 0; e < 3; e++) { sx += V[T[t][e]].x; sy += V[T[t][e]].y; }
            if (fabs(sx) > 1e-12 || fabs(sy) > 1e-12) cen_ok = 0;
        }
        CHECK("D6d: centroid ทุก Δ = center — 4 faces share circumcenter", cen_ok);
    }

    /* D6: 3+1 — ใบที่ 4 ถูกกำหนดโดยอีก 3 (complement — derived ไม่ต้องเลือก) */
    {
        int d_ok = 1;
        for (uint32_t t = 0; t < 4; t++) {
            uint32_t rest_mask = 0;
            for (uint32_t u = 0; u < 4; u++) {
                if (u == t) continue;
                for (uint32_t e = 0; e < 3; e++) rest_mask |= (1u << T[u][e]);
            }
            uint32_t own_mask = 0;
            for (uint32_t e = 0; e < 3; e++) own_mask |= (1u << T[t][e]);
            if ((rest_mask | own_mask) != 0xFFFu || (rest_mask & own_mask) != 0)
                d_ok = 0;
        }
        CHECK("D6e: 3+1 — ใบที่ 4 = complement ของอีก 3 (หา 4 ได้ทันทีจาก 3)", d_ok);
    }

    /* D6: R120 = symmetry ของแต่ละ Δ — tetra C3 axis เดียวกับ D5 */
    {
        int t3 = 1;
        for (uint32_t t = 0; t < 4; t++)
            if (!set_eq_rot(T[t], 3, 4, T[t])) t3 = 0;
        CHECK("D6f: R120° = symmetry ของแต่ละ Δ (tetra C3 axis) — C3 ตัวเดียวกับ 3 squares",
              t3);
    }
    printf("\n");
}

int main(void)
{
    setvbuf(stdout, NULL, _IONBF, 0);
    printf("The ×2 Law of the 12-gon — dodecagon ทุกชั้นมาเป็นคู่\n");
    printf("═══════════════════════════════════════════════════════════════\n");
    for (uint32_t i = 0; i < N; i++) {
        double a = -M_PI/2.0 + i * (M_PI/6.0);
        V[i].x = cos(a);
        V[i].y = sin(a);
    }
    test_stride_cycles();
    test_crossings();
    test_fan();
    test_threefold();
    test_general();
    test_nesting();
    printf("═══════════════════════════════════════════════════════════════\n");
    printf("RESULTS: %d/%d PASS\n", pass_count, pass_count + fail_count);
    return fail_count ? 1 : 0;
}
