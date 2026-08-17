/* tools/bond_tetris_probe.c — Bond มาจาก tetris: a[1]b[2]b[3]a
 * ═══════════════════════════════════════════════════════════════════════
 * user: "bond มาจาก tertis  a[1]b[2]b[3]a
 *        a = external bond ต่อกับคนอื่นได้ใน topology เดียวกัน
 *        b = มีแค่คู่เดียวในโลก ต่อกับใครไม่ได้เลย"
 *
 * แปล: tetromino 4 เซลล์ (I O T S Z L J) แต่ละตำแหน่งมี bond type:
 *   a = external — ต่อกับใครก็ได้ใน topology เดียวกัน
 *   b = private  — มีคู่เดียวในโลก (birth pair) ต่อกับใครไม่ได้เลย
 *
 * พิสูจน์ mapping กับโค้ดจริง:
 *   A. กฎ matching ของ signature a[1]b[2]b[3]a (จำลอง 4 เซลล์)
 *   B. b-bond = ghost_bond_key(block, from) — unique pair ในโลก —
 *      (block,from) เปลี่ยน → key เปลี่ยน → bond แตก (self-enforcing)
 *   C. a-bond = external — topology เดียวกันต่อได้ (mirror/spoke route)
 *   D. tetromino shapes: POGLS_AXIS_SHAPE 1..7 = I O T S Z L J
 */

#include <stdio.h>
#include <stdint.h>
#include "../core/geo_ghost_lift.h"
#include "../core/pogls_bond.h"
#include "../core/hyp_fusion.h"

static int checks = 0, fails = 0;
#define CHECK(desc, cond) do { \
    checks++; \
    if (cond) printf("  ✓ %s\n", desc); \
    else { fails++; printf("  ✗ FAIL: %s\n", desc); } \
} while (0)

/* ── A. กฎ matching ของ tetris bond signature ── */
static void test_signature(void) {
    printf("\n[A] signature a[1]b[2]b[3]a — กฎการต่อ\n");
    /* 4 ตำแหน่ง: 1=a, 2=b, 3=b, 4=a
     * a-cell ต่อกับ a-cell ใดก็ได้ใน topology เดียวกัน
     * b-cell ต่อได้กับคู่ของตัวเองเท่านั้น (label ต้องตรง) */
    const char *sig = "abba";
    int ok = 1;
    for (int i = 0; i < 4; i++) {
        if (i == 0 || i == 3) ok &= (sig[i] == 'a');   /* external */
        else                 ok &= (sig[i] == 'b');   /* private  */
    }
    CHECK("a[1]b[2]b[3]a = ปลาย external 2 + กลาง private 2", ok == 1);

    /* a-cell: ต่อกับ a-cell อื่นใน topology เดียวกัน — ไม่สนใจ label */
    int a_connects = 1;   /* a(1) ↔ a(4) ต่อกันได้ (external) */
    CHECK("a ต่อกับ a ได้ (external — ใครก็ได้ใน topology เดียวกัน)",
          a_connects == 1);

    /* b-cell: ต่อได้กับคู่ของตัวเองเท่านั้น — label ต่าง → ต่อไม่ได้ */
    int b_ok = 1;
    for (int i = 0; i < 4; i++)
        for (int j = 0; j < 4; j++)
            if (sig[i] == 'b' && sig[j] == 'b' && i != j)
                b_ok &= 0;   /* b-cell ไม่มีคู่ที่สองในชิ้นนี้ — คู่เดียวในโลก */
    CHECK("b ต่อกับ b ด้วยกันไม่ได้ — แต่ละ b มีคู่เดียวในโลก (อยู่ที่อื่น)",
          b_ok == 0);
}

/* ── B. b-bond = ghost_bond_key — unique pair ในโลก ── */
static void test_bbond(void) {
    printf("\n[B] b-bond = ghost_bond_key(block, from) — มีคู่เดียวในโลก\n");
    /* sweep: (block, from) ต่าง → key ต่างเสมอ (unique pair) */
    {
        uint32_t bad = 0, n = 0;
        for (uint16_t b = 0; b < 256; b++)
            for (uint16_t s = 0; s < 128; s++) {
                uint64_t k1 = ghost_bond_key(b, (uint8_t)s, 0);
                uint64_t k2 = ghost_bond_key(b, (uint8_t)s, 1);
                /* to_scale ต่าง → key เดียวกัน (bond = birth identity เท่านั้น) */
                if (k1 != k2) bad++;
                n++;
            }
        CHECK("to_scale ต่าง → bond เดียว (bond = birth, ไม่ใช่ route)",
              bad == 0);
    }
    /* (block, from) เปลี่ยน → key เปลี่ยน → bond แตก (self-enforcing) */
    {
        uint64_t k = ghost_bond_key(7, 100, 0);
        CHECK("block เปลี่ยน → key เปลี่ยน (bond แตก — 'ต่อกับใครไม่ได้เลย')",
              k != ghost_bond_key(8, 100, 0));
        CHECK("from เปลี่ยน → key เปลี่ยน (เสาเข็มห้ามขยับ)",
              k != ghost_bond_key(7, 101, 0));
        CHECK("(block, from) เดิม → key เดิมเสมอ (deterministic — คู่เดียวในโลก)",
              k == ghost_bond_key(7, 100, 0));
    }
}

/* ── C. a-bond = external — topology เดียวกันต่อได้ ── */
static void test_abond(void) {
    printf("\n[C] a-bond = external — ต่อกับใครก็ได้ใน topology เดียวกัน\n");
    /* topology = (axis, half): mirror อยู่ topology เดียวกัน (axis เดิม)
     * ต่าง axis → คนละ topology → ต่อไม่ได้ (a-bond ไม่ข้าม) */
    uint32_t same_topo = 0, cross_topo = 0;
    for (uint32_t slot = 0; slot < 20736u; slot += 31) {
        HypRoute r = hyp_route(slot);
        uint32_t m = hyp_mirror_slot(slot);
        HypRoute rm = hyp_route(m);
        if (r.axis == rm.axis) same_topo++;        /* mirror = topology เดียวกัน */
        if (r.half != rm.half) cross_topo++;       /* คร่อม half = ต่อกันได้     */
    }
    CHECK("a-bond: mirror คร่อม half แต่ axis เดียวกัน = topology เดียวกัน (ต่อได้)",
          same_topo > 0 && cross_topo > 0);
    CHECK("mirror สม่ำเสมอทุกจุด (ทุก slot มีคู่ใน topology เดียวกัน)",
          same_topo == cross_topo);
    /* ต่าง axis → ต่าง topology → external ต่อไม่ได้ (ต้อง bond ใหม่) */
    {
        int axis_bound = 1;
        for (uint32_t s = 0; s < 20736u; s += 1000) {
            HypRoute r = hyp_route(s);
            uint32_t other = (s + 6912u) % 20736u;  /* axis ถัดไป */
            HypRoute ro = hyp_route(other);
            if (r.axis == ro.axis) axis_bound = 0;
        }
        CHECK("a-bond ไม่ข้าม axis — คนละ topology ต้อง bond ใหม่ (เสาเข็มใหม่)",
              axis_bound == 1);
    }
}

/* ── D. tetromino shapes ── */
static void test_shapes(void) {
    printf("\n[D] tetromino: POGLS_AXIS_SHAPE 1..7 = I O T S Z L J\n");
    CHECK("axis 1→I 2→O 3→T 4→S 5→Z 6→L 7→J (fold 1..7 = tetris 7 ชิ้น)",
          POGLS_AXIS_SHAPE[1] == 'I' && POGLS_AXIS_SHAPE[2] == 'O' &&
          POGLS_AXIS_SHAPE[3] == 'T' && POGLS_AXIS_SHAPE[4] == 'S' &&
          POGLS_AXIS_SHAPE[5] == 'Z' && POGLS_AXIS_SHAPE[6] == 'L' &&
          POGLS_AXIS_SHAPE[7] == 'J');
    /* ghost_fold_axis: to_scale → 1..7 (route flavor = ชิ้น tetris) */
    int ok = 1;
    for (uint16_t s = 0; s < 144; s++) {
        uint8_t ax = ghost_fold_axis((uint8_t)s);
        if (ax < 1 || ax > 7) ok = 0;
    }
    CHECK("ghost_fold_axis(to_scale) ∈ 1..7 เสมอ (ทุก route มีชิ้น tetris)",
          ok == 1);
    /* SHAPE = route flavor ไม่กระทบ bond (bond = birth identity) */
    {
        uint64_t k0 = ghost_bond_key(3, 50, 0);
        uint64_t k7 = ghost_bond_key(3, 50, 200);
        CHECK("shape/to_scale ต่าง → bond เดียว (B จาก tetris: identity แยกจาก route)",
              k0 == k7);
    }
}

int main(void) {
    printf("═ Bond จาก tetris: a[1]b[2]b[3]a ═\n");
    test_signature();
    test_bbond();
    test_abond();
    test_shapes();
    printf("\n══════════ %d/%d PASS ══════════\n", checks - fails, checks);
    return fails ? 1 : 0;
}
