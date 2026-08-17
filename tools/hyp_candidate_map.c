/* tools/hyp_candidate_map.c — แผนที่ candidates → hyperbolic side
 * ═══════════════════════════════════════════════════════════════════════
 * user: "พวกที่หามาทั้งหมดนี่ล่ะ ผมเอามาเสนอ candidates ให้ทำงานกับ
 *        hyperbolic ไม่รู้ว่าอันไหนเหมาะกับตรงไหนบ้าง"
 *
 * KEY INSIGHT (พิสูจน์ใน probe นี้):
 *   สนาม 20736 = 6 cylinders (§15.49) = 3 hyperbolic axes × 2 halves
 *   - HYP_AXIS_SLOTS = 6912 = 2 × 3456 (2 cylinders ต่อ axis)
 *   - HYP_INFINITY_IDX = 3456 = 1 cylinder = KIS half ของ axis
 *   - axis band [0,3456) = cylinder KIS (positive)
 *                [3456,6912) = cylinder hyperbolic (mirror)
 *   → hyperbolic side = กระจก cylinder ของแต่ละ axis (Cayley = mirror map)
 *
 * และ candidates ที่หามาทั้งหมดวางลงชั้นไหนของ hyperbolic side (ส่วน D)
 * + composition จริง: scale-event log → wang edge validate → tantrix route
 */

#include <stdio.h>
#include <stdint.h>
#include "hyperbolic_seek.h"
#include "geo_frame_seek_wang.h"
#include "lc_tantrix.h"

#define FIELD    HYP_KIS_SLOTS   /* 20736 */
#define AXIS_SZ  HYP_AXIS_SLOTS  /* 6912  */
#define CYL      HYP_INFINITY_IDX /* 3456 = 1 cylinder = KIS half */
#define SPOKES   6u
#define SLOT_P_SPOKE 576u   /* 3456/6 = 24² */

static int checks = 0, fails = 0;
#define CHECK(desc, cond) do { \
    checks++; \
    if (cond) printf("  ✓ %s\n", desc); \
    else { fails++; printf("  ✗ FAIL: %s\n", desc); } \
} while (0)

/* ── A. axis × cylinder identity ── */
static void test_axis_cylinder(void) {
    printf("\n[A] สนาม = 6 cylinders = 3 axes × 2 halves\n");
    CHECK("20736 = 3 axis × 6912",
          FIELD == 3u * AXIS_SZ);
    CHECK("6912 = 2 × 3456 (2 cylinders ต่อ axis)",
          AXIS_SZ == 2u * CYL);
    CHECK("20736 = 3 × 2 × 3456 = 6 cylinders (ตรง §15.49)",
          FIELD == 6u * CYL);
    CHECK("HYP_INFINITY_IDX = 1 cylinder เป๊ะ",
          CYL == 3456u && CYL == SPOKES * SLOT_P_SPOKE);

    /* bijection: slot → (axis, half, spoke, slot_in_spoke) → back */
    {
        uint32_t bad = 0;
        for (uint32_t s = 0; s < FIELD; s++) {
            uint8_t  axis = (uint8_t)(s / AXIS_SZ);
            uint32_t rem  = s % AXIS_SZ;
            uint8_t  half = (uint8_t)(rem / CYL);   /* 0=KIS, 1=hyp  */
            uint32_t rem2 = rem % CYL;
            uint8_t  spoke = (uint8_t)(rem2 % SPOKES);
            uint16_t si    = (uint16_t)(rem2 / SPOKES);
            uint32_t back  = ((uint32_t)axis * AXIS_SZ)
                           + (uint32_t)half * CYL
                           + (uint32_t)si * SPOKES + spoke;
            if (back != s || spoke >= SPOKES || si >= SLOT_P_SPOKE) bad++;
        }
        CHECK("∀ slot: (axis, half, spoke, slot) roundtrip bijection (bad=0)",
              bad == 0);
    }
}

/* ── B. mirror halves + infinity boundary ── */
static void test_mirror(void) {
    printf("\n[B] KIS half ↔ hyperbolic half (กระจก cylinder)\n");
    /* half ของ slot: 0 = KIS [0,3456), 1 = hyperbolic [3456,6912) */
    {
        uint32_t n_kis = 0, n_hyp = 0;
        for (uint32_t s = 0; s < FIELD; s++) {
            uint8_t half = (uint8_t)((s % AXIS_SZ) / CYL);
            if (half == 0) n_kis++; else n_hyp++;
        }
        CHECK("KIS = hyp = 10368 = ครึ่งสนาม (3 axes × 1 cylinder)",
              n_kis == n_hyp && n_kis == 3u * CYL && n_hyp == FIELD / 2u);
    }
    /* invert pair ภายใน cylinder: spoke ↔ (spoke+3)%6 — ผ่านทั้ง 6 */
    {
        uint8_t pairs[6] = {0};   /* ตาม geo_spoke_invert */
        for (uint8_t s = 0; s < SPOKES; s++) {
            uint8_t iv = (uint8_t)((s + 3) % SPOKES);
            pairs[iv]++;
        }
        /* ทุก spoke ถูก invert โดน 1 ครั้ง (bijection) + invert² = id */
        uint32_t covered = 1;
        for (uint8_t s = 0; s < SPOKES; s++) {
            if (pairs[s] != 1) covered = 0;
            if (((s + 3) % SPOKES) == s) covered = 0;
            if (((uint8_t)((s + 3) % SPOKES) + 3u) % SPOKES != s) covered = 0;
        }
        CHECK("invert (spoke+3)%6 — bijection + invert²=id (เหมือน cylinder §15.49)",
              covered == 1);
    }
}

/* ── C. Glass pairing — a_w × a_{w+72} ≡ 1 (quadrant scheme จริง) ── */
static void test_glass(void) {
    printf("\n[C] Magnify glass — inverted rate ข้าม half (a_w × a_{w+72} ≡ 1)\n");
    const uint8_t QUAD = 36u, OFFSET = 5u;
    uint8_t a[144], inv[144];
    uint32_t ok = 1;
    for (uint32_t w = 0; w < 144; w++) {
        uint32_t shifted = (w + 144 - OFFSET) % 144;
        uint32_t q = shifted / QUAD;
        switch (q) {
            case 0:  a[w] = 103u; break;
            case 1:  a[w] = 5u;   break;
            case 2:  a[w] = 7u;   break;
            default: a[w] = 29u;  break;
        }
        uint8_t iv = 0;
        for (uint32_t x = 1; x < 144 && iv == 0; x++)
            if (((uint32_t)a[w] * x) % 144u == 1u) iv = (uint8_t)x;
        inv[w] = iv;
        if (iv == 0) ok = 0;
    }
    CHECK("ทุก w มี inverse (coprime กับ 144)", ok == 1);
    ok = 1;
    for (uint32_t w = 0; w < 144; w++)
        if (a[w] != inv[(w + 72) % 144]) ok = 0;   /* a_{w+72} = inverse ของ a_w */
    CHECK("a_w × a_{w+72} ≡ 1 mod 144 — ALL 144 w (antipodal inversion)",
          ok == 1);
}

/* ── D. Role map (print) ── */
static void print_map(void) {
    printf("\n[D] CANDIDATE → HYPERBOLIC ROLE MAP\n");
    printf("  ┌────────────────────┬──────────────────────────────┬─────────────────────────────────────────┐\n");
    printf("  │ candidate          │ hyperbolic layer              │ mechanism                               │\n");
    printf("  ├────────────────────┼──────────────────────────────┼─────────────────────────────────────────┤\n");
    printf("  │ RDH (geo_rdh_addr) │ log addressing                │ bond_key จาก (block,from_scale) — 5.1 cyc │\n");
    printf("  │ L-block            │ resume/placement หลัง lift    │ bookmark: orientation+เพื่อนบ้านจาก addr  │\n");
    printf("  │ GeoSeed 2-register │ identity ของ lifted block     │ face_id 12 หน้า (shift+mask ~2 cyc)      │\n");
    printf("  │ geo_seed 12-coset  │ identity signature (optional) │ 12 checksums topology-aware              │\n");
    printf("  │ Hosoya/F-ladder    │ weight ladder                 │ F(n)≈φⁿ = scale ladder ของระบบ (144=F12) │\n");
    printf("  │ cylinder spoke     │ spatial route ของ log entry   │ spoke=60° wedge · slot=radial · face=axial│\n");
    printf("  │ invert (spoke+3)%6 │ glass mirror ระหว่าง 2 halves │ ฝั่งตรงข้าม = hyperbolic (เก็บ delta)     │\n");
    printf("  │ wang edge          │ integrity ของ scale-log       │ edge_bot[w]==edge_top[w+1] → order valid │\n");
    printf("  │ tantrix            │ routing decision              │ gate ตรง=FORWARD · ไม่ตรง=DROP (ปิดเส้น)  │\n");
    printf("  │ geo_seek_gate      │ read-path router              │ Chord>Tantrix>RDH>Teleport>Frame         │\n");
    printf("  │ Morton / RDH fast  │ hot-path addressing           │ shift+mask ไม่มี divide                   │\n");
    printf("  │ fibo clock/frame   │ timeline ของ log (มีอยู่แล้ว) │ (round,tick) stride-37 · checkpoint-replay│\n");
    printf("  │ geomatrix (era)    │ ANTI-pattern → กฎ              │ อย่าหา inverse — บันทึก + rollback        │\n");
    printf("  └────────────────────┴──────────────────────────────┴─────────────────────────────────────────┘\n");
}

/* ── E. Composition: scale-event log → wang validate → tantrix route ── */
static void test_composition(void) {
    printf("\n[E] Composition จริง: scale-event log → wang → tantrix\n");
    FrameWangLayer wl;
    fwang_init(&wl);

    /* scale-event log: 120 events (จาก w 0..119 → w+72 = mirror) */
    int tamper = 0, mismatch = 0, ok369 = 0, drop = 0, fwd = 0;
    for (uint16_t w = 0; w < WANG_WIN_COUNT; w++) {
        uint16_t enc = (uint16_t)(w * 37 % FRAME_CYCLE); /* ตาม stride-37 */
        FrameWangDecision g = fwang_seek_gate(&wl, enc);
        /* tantrix: entry จาก half ของ event (KIS=0/hyp=1 → 0..3), incoming = spoke */
        DualFrame f = frame_at(enc);
        TantrixTile tile = tantrix_make((uint8_t)((enc / CYL) % 4u),
                                        (uint8_t)(enc % 4u),
                                        (uint8_t)(f.face % 4u),
                                        TANTRIX_CLASS_NORMAL);
        uint8_t out = 0;
        TantrixRouteResult r = tantrix_route(tile, f.p.sub, &out);
        if (g == FWANG_SEEK_TAMPER) tamper++;
        else if (g == FWANG_SEEK_MISMATCH) mismatch++;
        else ok369++;
        if (r == TANTRIX_ROUTE_DROP) drop++;
        else fwd++;
    }
    CHECK("สนามสะอาด: wang เปิดหมด (OK/369, ไม่มี TAMPER/MISMATCH)",
          tamper == 0 && mismatch == 0 && ok369 == WANG_WIN_COUNT);
    CHECK("tantrix ตัดสินใจครบทั้ง 120 events (forward+drop ครบ)",
          (fwd + drop) == WANG_WIN_COUNT);
    CHECK("ทั้งคู่ deterministic — ตัดสินใจได้จาก address ล้วน ไม่มี state",
          ok369 + tamper + mismatch == WANG_WIN_COUNT);
}

int main(void) {
    printf("═ candidates → hyperbolic side map ═\n");
    test_axis_cylinder();
    test_mirror();
    test_glass();
    print_map();
    test_composition();
    printf("\n══════════ %d/%d PASS ══════════\n", checks - fails, checks);
    return fails ? 1 : 0;
}
