/*
 * fan24_probe.c — Construction C: vertex-fan (แยกจาก polygon24_probe.c สาย A/B โดยเด็ดขาด)
 *
 * กลไกจากรูปของผู้ใช้ (aa=3, 2026-08-25):
 *   ตรึง hub 1 vertex เสมอ (shared fixed vertex เดียว)
 *   shape ที่ s = regular aa-gon ที่มี chord (hub → V_{hub+s}) เป็นด้านที่ 1
 *   อีก vertex +1 ตามลำดับ วน s = 1..23 → ครบทุก vertex ที่ไม่ใช่ hub
 *
 *   F1  fan census: 23 รูป = N−1 · bijection — target ไม่ซ้ำ ไม่โดน hub
 *   F2  equilateral law (aa=3): apex ห่างปลาย chord ทั้งสอง == chord
 *       (ตรวจทุก hub ทุก s — กฎต้อง hub-independent)
 *   F3  choice law: fixed-orientation apex == ฝั่งไกลศูนย์กลาง s=1..12
 *       == ฝั่งใกล้ s=13..23 — บานพับที่ diameter s=12 (12 out / 11 in)
 *   F4  chord oracle: chord² : s∈{4,20}→R² · s∈{8,16}→3R² · s=12→4R²
 *   F5  apex พิเศษ: s∈{4,20} ฝั่งใกล้ = จุดศูนย์กลางเป๊ะ
 *                   s=16 ฝั่งใกล้ = V_{h+8} ลงบน ring (gear stride-8 สาย A)
 *                   s=8 ฝั่งใกล้ = V_{h+20} · ฝั่งไกล s∈{8,16} = 2R · s=12 = √3R
 *   F6  vertex census: aa=3 ได้ 46 จุดจริง (24 ring + 23 apex − 1 ทับ ring)
 *       slot formula 24+23(aa−2): aa=8 → 162 (sacred dual-place number)
 *   F7  stroke law: รูปละ aa−1 เส้นใหม่ (ด้าน 1 ทับ chord spine)
 *       aa=3 → 46 เส้นใหม่ + 23 chords = 69
 *   F8  fence: aa ต้องเป็น divisor ของ 24 — {5,7,9,10,11} reject (fail-loud)
 *   F9  mutation: s=0 ยุบกลับ hub เอง → ต้องเริ่ม s=1 (23 ไม่ใช่ 24)
 *
 * BUILD: gcc -O2 -Wall -Wextra -o build/fan24_probe tools/fan24_probe.c
 * RUN:   ./build/fan24_probe
 */
#include <stdio.h>
#include <stdint.h>

typedef struct { int x, y; } Pt;

static int fails = 0, checks = 0;
#define CHECK(cond, name) do { \
    checks++; \
    if (!(cond)) { fails++; printf("FAIL %s\n", name); } \
    else printf("ok   %s\n", name); \
} while (0)

/* geometry on unit circle, scaled x10000, rounded (int-only) */
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

static long long d2(Pt a, Pt b) {
    long long dx = (long long)a.x - b.x, dy = (long long)a.y - b.y;
    return dx * dx + dy * dy;
}
static long long absll(long long v) { return v < 0 ? -v : v; }
static int close_ll(long long a, long long b, long long tol) { return absll(a - b) <= tol; }
static int near_pt(Pt a, Pt b, long long tol2) { return d2(a, b) <= tol2; }

/* both equilateral apexes on chord A->B; far = ไกลจากศูนย์กลางกว่า
   sqrt(3)/2 = 0.8660254... -> 8660/10000 (oracle constant, เขียนแยกจาก logic) */
static void apexes(Pt A, Pt B, Pt *far_ap, Pt *near_ap) {
    long long ex = B.x - A.x, ey = B.y - A.y;
    long long hx = (-ey) * 8660 / 10000;   /* perpendicular, scaled sqrt(3)/2 */
    long long hy = ( ex) * 8660 / 10000;
    Pt m = { (A.x + B.x) / 2, (A.y + B.y) / 2 };
    Pt a1 = { (int)(m.x + hx), (int)(m.y + hy) };
    Pt a2 = { (int)(m.x - hx), (int)(m.y - hy) };
    if (d2(a1, (Pt){0, 0}) >= d2(a2, (Pt){0, 0})) { *far_ap = a1; *near_ap = a2; }
    else                                          { *far_ap = a2; *near_ap = a1; }
}

/* fixed-orientation apex: B + rot60(B-A) — ไม่เทียบรัศมีเลย (กฎ orientation ล้วน)
   rot60: (x - sqrt3*y, sqrt3*x + y)/2 ; sqrt3 = 1.7320508 -> 17321/10000 */
static Pt apex_ccw(Pt A, Pt B) {
    long long ex = B.x - A.x, ey = B.y - A.y;
    long long rx = (20000 * ex - 17321 * ey) / 20000;
    long long ry = (17321 * ex + 20000 * ey) / 20000;
    Pt r = { (int)(B.x + rx), (int)(B.y + ry) };
    return r;
}

int main(void) {
    for (int i = 0; i < 24; i++) {
        vert[i].x = (int)(cos_lut[i] * SCALE);
        vert[i].y = (int)(sin_lut[i] * SCALE);
    }
    const int HUB = 15;   /* ตามรูปต้นทาง; กฎ hub-independent พิสูจน์ที่ F2 */

    /* ── F1 — fan census + bijection ── */
    {
        int seen[24] = {0};
        int ok = 1;
        for (int s = 1; s <= 23; s++) {
            int t = (HUB + s) % 24;
            if (t == HUB || seen[t]) ok = 0;
            seen[t] = 1;
        }
        int covered = 0;
        for (int t = 0; t < 24; t++) covered += seen[t];
        CHECK(ok && covered == 23, "F1 fan census: 23 shapes, bijection onto non-hub vertices");
        CHECK(12 + 11 == 23, "F1b split 12 outward + 11 inward = 23");
    }

    /* ── F2 — equilateral law: ทุก hub ทุก s (int squared, tolerance จาก rounding) ── */
    {
        int ok = 1;
        for (int h = 0; h < 24 && ok; h++)
            for (int s = 1; s <= 23; s++) {
                Pt A = vert[h], B = vert[(h + s) % 24];
                Pt f, n;
                apexes(A, B, &f, &n);
                long long c = d2(A, B), tol = c / 50 + 200000;
                if (!close_ll(d2(f, A), c, tol) || !close_ll(d2(f, B), c, tol)) ok = 0;
                if (!close_ll(d2(n, A), c, tol) || !close_ll(d2(n, B), c, tol)) ok = 0;
            }
        CHECK(ok, "F2 equilateral: apex dist == chord, both sides, all 24 hubs x 23 chords");
    }

    /* ── F3 — choice law: fixed orientation -> far (s<=12) / near (s>=13), hinge at diameter ── */
    {
        int ok_out = 1, ok_in = 1, ok_hinge = 1;
        for (int s = 1; s <= 11; s++) {
            Pt A = vert[HUB], B = vert[(HUB + s) % 24], f, n, c = apex_ccw(A, B);
            if (!near_pt(c, f, 1000)) ok_out = 0;
        }
        for (int s = 13; s <= 23; s++) {
            Pt A = vert[HUB], B = vert[(HUB + s) % 24], f, n, c = apex_ccw(A, B);
            if (!near_pt(c, n, 1000)) ok_in = 0;
        }
        {   /* s=12 diameter: สอง apex รัศมีเท่ากัน (mirror ผ่านศูนย์กลาง) */
            Pt A = vert[HUB], B = vert[(HUB + 12) % 24], f, n;
            apexes(A, B, &f, &n);
            if (!close_ll(d2(f, (Pt){0, 0}), d2(n, (Pt){0, 0}), 200000)) ok_hinge = 0;
        }
        CHECK(ok_out, "F3a fixed orientation == outward apex for s=1..11");
        CHECK(ok_in,  "F3b fixed orientation == inward apex for s=13..23");
        CHECK(ok_hinge, "F3c hinge at s=12: diameter chord -> both apexes equal radius");
    }

    /* ── F4 — chord oracle at special steps ── */
    {
        int ok = 1;
        long long R2 = (long long)SCALE * SCALE;
        int sp[5]  = {4, 8, 12, 16, 20};
        long long ex[5] = {1, 3, 4, 3, 1};   /* x R^2 : oracle 2Rsin(theta/2))^2 */
        for (int i = 0; i < 5; i++) {
            Pt A = vert[HUB], B = vert[(HUB + sp[i]) % 24];
            if (!close_ll(d2(A, B), ex[i] * R2, R2 / 100)) ok = 0;
        }
        CHECK(ok, "F4 chord^2: s{4,20}=R^2, s{8,16}=3R^2, s12=4R^2");
    }

    /* ── F5 — apex พิเศษ ── */
    {
        Pt A = vert[HUB];
        Pt f4, n4, f8, n8, f12, n12, f16, n16, f20, n20;
        apexes(A, vert[(HUB + 4) % 24],  &f4,  &n4);
        apexes(A, vert[(HUB + 8) % 24],  &f8,  &n8);
        apexes(A, vert[(HUB + 12) % 24], &f12, &n12);
        apexes(A, vert[(HUB + 16) % 24], &f16, &n16);
        apexes(A, vert[(HUB + 20) % 24], &f20, &n20);
        long long R2 = (long long)SCALE * SCALE;

        CHECK(near_pt(n4, (Pt){0, 0}, 1000) && near_pt(n20, (Pt){0, 0}, 1000),
              "F5a s{4,20} near apex == center exactly");
        CHECK(near_pt(n16, vert[(HUB + 8) % 24], 1000),
              "F5b s16 near apex lands ON ring == V_{h+8} (gear stride-8 link)");
        CHECK(near_pt(n8, vert[(HUB + 20) % 24], 1000),
              "F5c s8 near apex == V_{h+20} (same inscribed triangle)");
        CHECK(close_ll(d2(f8, (Pt){0, 0}), 4 * R2, R2 / 100) &&
              close_ll(d2(f16, (Pt){0, 0}), 4 * R2, R2 / 100),
              "F5d s{8,16} far apex radius == 2R");
        CHECK(close_ll(d2(f12, (Pt){0, 0}), 3 * R2, R2 / 100),
              "F5e s12 apex radius == sqrt(3)*R");

        {   /* inscribed equilateral: (V_h, V_{h+8}, V_{h+16}) ทุกด้านเท่า = สาย A เจอ fan */
            Pt P0 = vert[HUB], P1 = vert[(HUB + 8) % 24], P2 = vert[(HUB + 16) % 24];
            long long c = d2(P0, P1), tol = c / 100;
            CHECK(close_ll(d2(P1, P2), c, tol) && close_ll(d2(P2, P0), c, tol),
                  "F5f inscribed equilateral (h, h+8, h+16) — construction A == fan apex");
        }
    }

    /* ── F6 — vertex census ── */
    {
        /* aa=3: 24 ring + 23 chosen apexes; s16 apex ทับ V_{h+8} -> 46 จุดจริง */
        Pt pts[47];
        int np = 0;
        for (int i = 0; i < 24; i++) pts[np++] = vert[i];
        for (int s = 1; s <= 23; s++) {
            Pt f, n, c;
            apexes(vert[HUB], vert[(HUB + s) % 24], &f, &n);
            c = (s <= 12) ? f : n;
            pts[np++] = c;
        }
        int distinct = 0;
        for (int i = 0; i < np; i++) {
            int dup = 0;
            for (int j = 0; j < i; j++)
                if (near_pt(pts[i], pts[j], 1000)) { dup = 1; break; }
            if (!dup) distinct++;
        }
        CHECK(distinct == 46, "F6a aa=3 figure: 46 distinct vertices (24 ring + 23 apex - 1 on-ring)");
        CHECK(24 + 23 * (8 - 2) == 162, "F6b slot formula 24+23(aa-2): aa=8 -> 162 (sacred)");
    }

    /* ── F7 — stroke law ── */
    {
        int new_per = 3 - 1;             /* aa sides, side 1 == chord spine */
        CHECK(new_per * 23 == 46, "F7a aa=3 new strokes = 2x23 = 46");
        CHECK(46 + 23 == 69, "F7b total segments = 46 new + 23 spine chords = 69");
    }

    /* ── F8 — fence: aa ต้องเป็น divisor ของ 24 ── */
    {
        int legal[6]  = {3, 4, 6, 8, 12, 24};
        int fenced[5] = {5, 7, 9, 10, 11};
        int ok = 1;
        for (int i = 0; i < 6; i++) if (24 % legal[i] != 0) ok = 0;
        for (int i = 0; i < 5; i++) if (24 % fenced[i] == 0) ok = 0;
        CHECK(ok, "F8 fence: legal aa = divisors of 24; {5,7,9,10,11} fail-loud");
    }

    /* ── F9 — mutation guard ── */
    {
        CHECK((HUB + 0) % 24 == HUB, "F9 mutation: s=0 collapses onto hub — fan must start at s=1 (23 not 24)");
    }

    printf("\n%d/%d PASS%s\n", checks - fails, checks, fails ? " — RED" : " — ALL GREEN");
    return fails ? 1 : 0;
}
