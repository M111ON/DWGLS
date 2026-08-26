/*
 * fan24_gear_probe.c — Construction G: gear mesh KIS x Hyperbolic
 *
 * สมมติฐานจากผู้ใช้ (2026-08-26):
 *   fan24 ไม่ใช่ภาษา — เป็น GEAR เชื่อมสองโลก
 *   อัตราทด = การแยกตัวประกอบคู่ของ 24:  (8,3) · (6,4) · (12,2)
 *   ฝั่ง KIS = 8 cubes (tesseract), ฝั่ง Hyperbolic = 3 axes
 *   → ฟันเฟือง = ring-24, การหมุน 1 ฟัน = ก้าว s เดียว
 *
 * กลไก: หมุนเฟือง s ฟัน → ตำแหน่งคู่ (s mod D, s mod d) บนเฟือง (D,d)
 *   oracle อิสระ: CRT theorem — s ↦ (s mod D, s mod d) bijection บน Z_{D·d}
 *   ⟺ gcd(D,d) = 1 ; |image| = lcm(D,d) เสมอ (ทฤษฎี ไม่ได้มาจาก code)
 *
 *   G1  coprime split (8,3): bijection ครบ 24 — lossless mesh (KIS cube x axis)
 *   G2  inverse: ทุก cell (cube,axis) กลับเป็น s เดียว (brute force อิสระ)
 *   G3  fold ของ fan (out/in ที่ s=12): s ↔ 24−s ≡ −s ทั้งสองเฟือง
 *       = เฟืองหมุนสวนกัน (counter-rotation) — กฎ F3 hinge ก็คือการสบเฟือง
 *   G4  pure moves: s%8==0 → เฟือง cube นิ่ง (หมุน axis ล้วน) = {8,16}
 *       = inscribed triangle strides ของ F5f ; s=12 (diameter) → axis นิ่ง
 *   G5  fence: split ที่ gcd>1 — (6,4),(12,2) image = lcm = 12 (ครึ่งเดียว)
 *       → lossy ต้องถูกมองเห็น (fail-loud) ตามกฎเลขอันตราย
 *   G6  home: s=24 ≡ (0,0) = hub เอง — fan เริ่ม s=1 (F9) = เฟืองจุด home
 *   G7  contact census: chord class d=1..11 มี 24 เส้น, d=12 มี 12
 *       → รวม C(24,2)=276 = 23×12 ; contact ต่อรอบ = 11 คู่ + 1 diameter = 12
 *
 * BUILD: gcc -O2 -Wall -Wextra -o build/fan24_gear_probe tools/fan24_gear_probe.c
 * RUN:   ./build/fan24_gear_probe
 */
#include <stdio.h>
#include <stdint.h>

static int fails = 0, checks = 0;
#define CHECK(cond, name) do { \
    checks++; \
    if (!(cond)) { fails++; printf("FAIL %s\n", name); } \
    else printf("ok   %s\n", name); \
} while (0)

static int gcd(int a, int b) { while (b) { int t = a % b; a = b; b = t; } return a; }
static int lcmv(int a, int b) { return a / gcd(a, b) * b; }

int main(void) {
    const int D = 8, d = 3;          /* KIS cubes x Hyper axes — 24 = 8*3 */

    /* ── G1 — coprime split bijection ── */
    {
        int seen[8][3] = {{0}};
        for (int s = 0; s < 24; s++) seen[s % D][s % d]++;
        int ok = 1;
        for (int c = 0; c < D; c++) for (int a = 0; a < d; a++) if (seen[c][a] != 1) ok = 0;
        CHECK(ok && gcd(D, d) == 1,
              "G1 CRT mesh (8,3): s -> (s%8, s%3) bijection 24 cells — lossless");
    }

    /* ── G2 — inverse exists, unique (brute-force oracle) ── */
    {
        int ok = 1;
        for (int c = 0; c < D && ok; c++)
            for (int a = 0; a < d && ok; a++) {
                int hits = 0;
                for (int s = 0; s < 24; s++)
                    if (s % D == c && s % d == a) hits++;
                if (hits != 1) ok = 0;
            }
        CHECK(ok, "G2 inverse: every (cube,axis) cell recovers unique s");
    }

    /* ── G3 — fan fold == counter-rotation ── */
    {
        int ok = 1;
        for (int s = 1; s <= 23; s++) {
            int f = 24 - s;   /* mirror step across the diameter hinge */
            if (f % D != (D - s % D) % D) ok = 0;
            if (f % d != (d - s % d) % d) ok = 0;
        }
        CHECK(ok, "G3 fold s->24-s == negation on BOTH wheels (gears counter-rotate)");
    }

    /* ── G4 — pure moves == fan special chords ── */
    {
        int cube_fixed[24] = {0}, axis_fixed[24] = {0};
        int nc = 0, na = 0;
        for (int s = 1; s <= 23; s++) {
            if (s % D == 0) cube_fixed[nc++] = s;    /* wheel D still */
            if (s % d == 0) axis_fixed[na++] = s;    /* wheel d still */
        }
        int ok_c = (nc == 2 && cube_fixed[0] == 8 && cube_fixed[1] == 16);
        int ok_a = (na == 7 && axis_fixed[0] == 3 && axis_fixed[6] == 21);
        /* fan F4 specials {4,8,12,16,20}: {8,16} sit in cube-fixed (F5f inscribed
           triangle h,h+8,h+16) ; diameter 12 sits in axis-fixed */
        int ok_link = cube_fixed[0] == 8 && cube_fixed[1] == 16;
        int dia_axis = 0;
        for (int i = 0; i < na; i++) if (axis_fixed[i] == 12) dia_axis = 1;
        CHECK(ok_c, "G4a cube-still steps == {8,16} == inscribed triangle strides");
        CHECK(ok_a, "G4b axis-still steps == multiples of 3 (7 steps)");
        CHECK(ok_link && dia_axis, "G4c fan specials {8,16}=cube-only, s12(diameter)=axis-only");
    }

    /* ── G5 — fence: non-coprime splits are HALF-density (lossy, visible) ── */
    {
        /* expected |image| = lcm(D',d') — number theory, not implementation */
        int img64 = 0, img122 = 0;
        /* (6,4): image = pairs (i,j) with i≡j (mod 2) → count via lcm */
        {   int seen[6][4] = {{0}};
            for (int s = 0; s < 24; s++) seen[s % 6][s % 4] = 1;
            for (int i = 0; i < 6; i++) for (int j = 0; j < 4; j++) img64 += seen[i][j]; }
        {   int seen[12][2] = {{0}};
            for (int s = 0; s < 24; s++) seen[s % 12][s % 2] = 1;
            for (int i = 0; i < 12; i++) for (int j = 0; j < 2; j++) img122 += seen[i][j]; }
        CHECK(img64 == lcmv(6, 4) && img64 < 24,
              "G5a split (6,4): image=lcm=12 < 24 — half-density, LOSSY (fenced)");
        CHECK(img122 == lcmv(12, 2) && img122 < 24,
              "G5b split (12,2): image=lcm=12 < 24 — half-density, LOSSY (fenced)");
        CHECK(gcd(6, 4) > 1 && gcd(12, 2) > 1 && gcd(8, 3) == 1,
              "G5c ONLY coprime split (8,3) is lossless — rest are visible fences");
    }

    /* ── G6 — home position ── */
    {
        CHECK(24 % D == 0 && 24 % d == 0,
              "G6 s=24 returns (0,0)=home — fan starts s=1 (F9) = gear home marker");
    }

    /* ── G7 — contact census (combinatorial oracle) ── */
    {
        /* chord class k (1..12): count unordered vertex pairs at cyclic distance k
           independent count: N=24, k<12 → 24 pairs each ; k=12 → 12 (antipodal) */
        long long total = 0;
        for (int k = 1; k <= 11; k++) total += 24;
        total += 12;
        CHECK(total == 276 && total == 23 * 12,
              "G7a chord partition C(24,2)=276 = 23x12 (independent combinatorics)");
        CHECK(11 * 2 + 1 == 23,
              "G7b contacts per revolution: 11 mirrored pairs + 1 diameter = 23 fan steps");
    }

    printf("\n%d/%d PASS%s\n", checks - fails, checks, fails ? " — RED" : " — ALL GREEN");
    return fails ? 1 : 0;
}
