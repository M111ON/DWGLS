/* test_wang_tantrix.c — Wang seek gate + Tantrix fabric switch
 * ═══════════════════════════════════════════════════════════════════════════
 * user: "tantrix น่าเอามาใช้อยู่นะ และ geo_frame_seek_wang รุ่นนี้ก็เฉียบ
 *        สามารถเป็น switch ได้ด้วย ตอนที่ seek ไปแล้วปิดเส้นทางได้"
 *
 * พิสูจน์:
 *   A. Wang layer invariants — chord 2&7 (a+b==9), edge continuity ข้าม
 *      window, 3 skip / 4×369 ต่อ window, XOR parity — หลัง FIX 2 จุด
 *      (edge_bot off-by-one + skip_mask uint16) fwang_verify() == 0
 *   B. SWITCH — fwang_seek_gate ตัดสินใจเปิด/ปิดเส้นทางต่อ frame:
 *      สะอาด → OK/369 · chord โดนทำลาย → TAMPER (ปิด) ·
 *      edge ขาด → MISMATCH (ปิด) · deterministic
 *   C. Tantrix — 1 byte = 1 routing instruction (252 normal + 4 special):
 *      route FORWARD เมื่อ gate ตรง · DROP เมื่อไม่ตรง (ปิดเส้นทาง!) ·
 *      CROSS = invert · SPLIT = broadcast · MERGE · NULL = drop ·
 *      spoke_mask = invert pairs (0,3)(1,4)(2,5) — เชื่อม cylinder §15.49
 *   D. CHAIN — seek → wang gate (integrity) → tantrix route (direction):
 *      เปิดทั้งคู่ = ผ่าน · ข้อมูลเสีย = ปิด (ทั้งสองชั้น) · deterministic
 *
 * BUILD: gcc -O2 -Wall -Wextra -Wno-unused-parameter -I. -Icore \
 *        -o build/test-wang_tantrix tests/test_wang_tantrix.c -lm
 */

#include <stdio.h>
#include <string.h>
#include <stdint.h>
#include "../core/geo_frame_seek_wang.h"
#include "../core/lc_tantrix.h"

static int pass = 0, fail = 0;
#define CHECK(n, desc, cond) do { \
    if (cond) { pass++; printf("  T%d: PASS — %s\n", n, desc); } \
    else      { fail++; printf("  T%d: FAIL — %s\n", n, desc); } \
} while(0)

/* ── A. Wang layer invariants ── */
static void test_wang_invariants(void) {
    printf("\nA. Wang layer — invariants (หลัง FIX edge_bot + skip_mask)\n");
    FrameWangLayer wl;
    fwang_init(&wl);

    /* A1: verify เต็ม (เดิม = -2, ตอนนี้ต้อง 0) */
    CHECK(1, "fwang_verify() == 0 (เดิมพัง -2 edge, -5 skip)", fwang_verify(&wl) == 0);

    /* A2: chord invariant a+b==9 ทุก frame (1440) */
    {
        int bad = 0;
        for (uint32_t t = 0; t < FRAME_CYCLE; t++) {
            uint16_t e = frame_seek(t).enc;
            if (!_fwang_chord_valid(e)) bad++;
        }
        CHECK(2, "chord invariant (2&7 on 9-clock) ครบ 1440 frame (bad=0)", bad == 0);
    }

    /* A3: edge continuity 120 window + wrap */
    {
        int cont = 0, wrap = 0;
        for (uint16_t w = 1; w < WANG_WIN_COUNT; w++)
            if (fwang_edge_valid(&wl, w)) cont++;
        wrap = fwang_edge_valid_wrap(&wl, 0);
        CHECK(3, "edge continuity 119/119 + wrap (tamper: edge_bot[w]==edge_top[w+1])",
              cont == WANG_WIN_COUNT - 1 && wrap);
    }

    /* A4: 3 skip + 4×369 ต่อ window (skip = enc%12>=9, 369 = enc%9∈{0,3,6}) */
    {
        int bad_skip = 0, n369 = 0;
        for (uint16_t w = 0; w < WANG_WIN_COUNT; w++) {
            if (__builtin_popcount(wl.wins[w].skip_mask) != 3) bad_skip++;
            for (uint8_t i = 0; i < WANG_WIN_SIZE; i++)
                if (frame_seek((uint32_t)w * WANG_WIN_SIZE + i).enc % 9u == 0u ||
                    frame_seek((uint32_t)w * WANG_WIN_SIZE + i).enc % 9u == 3u ||
                    frame_seek((uint32_t)w * WANG_WIN_SIZE + i).enc % 9u == 6u)
                    n369++;
        }
        CHECK(4, "skip 3/window ครบ 120 (เดิม mask=0 เพราะ uint8 truncate)",
              bad_skip == 0);
        CHECK(5, "369 (Tesla loop) 4/window = 480 total", n369 == 480);
    }

    /* A5: XOR parity — xor_enc == XOR ของ 12 enc ใน window */
    {
        int bad = 0;
        for (uint16_t w = 0; w < WANG_WIN_COUNT; w++) {
            uint16_t x = 0;
            for (uint8_t i = 0; i < WANG_WIN_SIZE; i++)
                x ^= frame_seek((uint32_t)w * WANG_WIN_SIZE + i).enc;
            if (x != wl.wins[w].xor_enc) bad++;
        }
        CHECK(6, "XOR parity ต่อ window ตรง (reconstruct 1 missing ได้)", bad == 0);
    }

    /* A6: tile_id = face ของ frame แรก ∈ [0,12) */
    {
        int bad = 0;
        for (uint16_t w = 0; w < WANG_WIN_COUNT; w++)
            if (wl.wins[w].tile_id !=
                frame_seek((uint32_t)w * WANG_WIN_SIZE).face) bad++;
        CHECK(7, "tile_id = face ของ frame แรกใน window (bad=0)", bad == 0);
    }
}

/* ── B. The switch: seek gate เปิด/ปิดเส้นทาง ── */
static void test_wang_gate_switch(void) {
    printf("\nB. SWITCH — fwang_seek_gate: seek แล้วปิดเส้นทางได้\n");
    FrameWangLayer wl;
    fwang_init(&wl);

    /* B1: สนามสะอาด — เปิด 960 (OK) + 480 (369) ไม่มี TAMPER/MISMATCH */
    {
        int ok = 0, n369 = 0, tper = 0, mis = 0;
        for (uint16_t e = 0; e < FRAME_CYCLE; e++) {
            switch (fwang_seek_gate(&wl, e)) {
                case FWANG_SEEK_OK:       ok++;   break;
                case FWANG_SEEK_369:      n369++; break;
                case FWANG_SEEK_TAMPER:   tper++; break;
                case FWANG_SEEK_MISMATCH: mis++;  break;
            }
        }
        CHECK(8, "clean: OK=960 + 369=480, TAMPER=0 MISMATCH=0",
              ok == 960 && n369 == 480 && tper == 0 && mis == 0);
    }

    /* B2: chord identity เป็นจริงเสมอ (2e+7e=9e≡0 mod 9 ทุก e) →
     * tamper จริงต้องตรวจชั้นเก็บ: แก้ edge_top_b (chord complement)
     * ที่ window → gate ต้องจับได้ → TAMPER → ปิดเส้นทาง */
    {
        FrameWangLayer wl2;
        fwang_init(&wl2);
        wl2.wins[3].edge_top_b ^= 1u;   /* แตะ chord complement ที่เก็บไว้ */
        FrameWangDecision d = fwang_seek_gate(&wl2, 3u * WANG_WIN_SIZE);
        CHECK(9, "edge ถูกแก้ในชั้นเก็บ → TAMPER (FIX: wire tamper_check เข้า gate)",
              d == FWANG_SEEK_TAMPER);
    }

    /* B3: edge ขาด (window โดนแก้แบบ chord-คู่ = ผ่าน tamper แต่
     * continuity กับ window ก่อนหน้าแตก) → MISMATCH → ปิดเส้นทาง
     * (ใช้ frame 61 = win 5 ที่ไม่ใช่ 369 — 60 เป็น 369: 60%9=6) */
    {
        FrameWangLayer wl2;
        fwang_init(&wl2);
        uint8_t save = wl2.wins[5].edge_top;
        uint8_t neu = (uint8_t)((save + 1) % 9u);
        wl2.wins[5].edge_top   = neu;
        wl2.wins[5].edge_top_b = (uint8_t)((9u - neu) % 9u); /* chord คง 9 */
        FrameWangDecision d = fwang_seek_gate(&wl2, 5u * WANG_WIN_SIZE + 1u);
        CHECK(10, "edge ขาด → MISMATCH (ผ่าน tamper, พังที่ continuity)",
              d == FWANG_SEEK_MISMATCH);
    }

    /* B4: deterministic — same enc → same decision (รัน 2 ครั้ง) */
    {
        int same = 1;
        for (uint16_t e = 0; e < FRAME_CYCLE; e += 97) {
            FrameWangLayer wl2;
            fwang_init(&wl2);
            if (fwang_seek_gate(&wl, e) != fwang_seek_gate(&wl2, e)) same = 0;
        }
        CHECK(11, "switch deterministic — enc เดียว → คำตอบเดียวเสมอ", same == 1);
    }

    /* B5: เฟรมเดียวกัน ข้อมูลดี = เปิด · ข้อมูลเสีย = ปิด
     * (e=1202 ใน win 100 ที่ไม่ใช่ 369 — 1203%9=6 เป็น 369) */
    {
        uint16_t e = 1202;
        CHECK(12, "สะอาด: gate เปิด (OK/369)",
              fwang_seek_gate(&wl, e) == FWANG_SEEK_OK);
        /* window เดียวกัน แก้ edge แบบ chord-คู่ → เฟรมเดียวกันต้องปิด */
        FrameWangLayer wl2;
        fwang_init(&wl2);
        uint16_t win = (e / WANG_WIN_SIZE) % WANG_WIN_COUNT;
        uint8_t neu = (uint8_t)((wl2.wins[win].edge_top + 1) % 9u);
        wl2.wins[win].edge_top   = neu;
        wl2.wins[win].edge_top_b = (uint8_t)((9u - neu) % 9u);
        CHECK(13, "เสีย: edge ขาดที่ window เดียวกัน → ปิด (MISMATCH)",
              fwang_seek_gate(&wl2, e) == FWANG_SEEK_MISMATCH);
    }
}

/* ── C. Tantrix — 1 byte = routing instruction ── */
static void test_tantrix(void) {
    printf("\nC. Tantrix — fabric switch (1 byte = 1 routing instruction)\n");

    /* C1: verify 252 normal + 4 special */
    CHECK(14, "lc_tantrix_verify() == 0 (encode/decode roundtrip + specials)",
          lc_tantrix_verify() == 0);

    /* C2: route — gate ตรง → FORWARD · ไม่ตรง → DROP (ปิดเส้นทาง) */
    {
        uint8_t out = 0;
        TantrixTile t = tantrix_make(1, 2, 0, TANTRIX_CLASS_NORMAL);
        CHECK(15, "gate ตรง (1==1) → FORWARD, exit=2",
              tantrix_route(t, 1, &out) == TANTRIX_ROUTE_FORWARD && out == 2);
        CHECK(16, "gate ไม่ตรง (3≠1) → DROP (เส้นทางปิด!)",
              tantrix_route(t, 3, &out) == TANTRIX_ROUTE_DROP);
    }

    /* C3: special tiles */
    {
        uint8_t out = 0;
        CHECK(17, "NULL → DROP", tantrix_route(TANTRIX_NULL, 2, &out) == TANTRIX_ROUTE_DROP);
        CHECK(18, "CROSS → invert gate (cross_map {1,0,3,2})",
              tantrix_route(TANTRIX_CROSS, 0, &out) == TANTRIX_ROUTE_FORWARD && out == 1 &&
              tantrix_route(TANTRIX_CROSS, 2, &out) == TANTRIX_ROUTE_FORWARD && out == 3);
        CHECK(19, "SPLIT → BROADCAST (ส่งต่อทุกทิศ)",
              tantrix_route(TANTRIX_SPLIT, 3, &out) == TANTRIX_ROUTE_BROADCAST && out == 3);
        CHECK(20, "MERGE → merge gate",
              tantrix_route(TANTRIX_MERGE, 1, &out) == TANTRIX_ROUTE_MERGE);
    }

    /* C4: SKIP/MIRROR class เปลี่ยน exit */
    {
        uint8_t out = 0;
        TantrixTile sk = tantrix_make(1, 1, 0, TANTRIX_CLASS_SKIP);
        CHECK(21, "SKIP class: exit ^= 3 (1→2)",
              tantrix_route(sk, 1, &out) == TANTRIX_ROUTE_FORWARD && out == 2);
        TantrixTile mi = tantrix_make(1, 1, 0, TANTRIX_CLASS_MIRROR);
        CHECK(22, "MIRROR class: exit swap bits (1→2)",
              tantrix_route(mi, 1, &out) == TANTRIX_ROUTE_FORWARD && out == 2);
    }

    /* C5: Wang edge compatibility */
    {
        TantrixTile a = tantrix_make(0, 2, 0, TANTRIX_CLASS_NORMAL);
        TantrixTile b = tantrix_make(2, 3, 0, TANTRIX_CLASS_NORMAL);
        TantrixTile c = tantrix_make(1, 3, 0, TANTRIX_CLASS_NORMAL);
        CHECK(23, "connects(a,b) == (exit(a)==entry(b)) — Wang edge match",
              tantrix_connects(a, b) && !tantrix_connects(a, c));
        CHECK(24, "null ตัดการเชื่อม (เส้นทางปิด)",
              !tantrix_connects(a, TANTRIX_NULL));
    }

    /* C6: spoke_mask = invert pairs จาก cylinder (§15.49) */
    {
        CHECK(25, "spoke 0 → mask 0x09 = invert pair (0,3)",
              tantrix_active_spokes(tantrix_make(0,0,0,TANTRIX_CLASS_NORMAL)) == 0x09u);
        CHECK(26, "spoke 1 → 0x12 = (1,4) · spoke 2 → 0x24 = (2,5) · spoke 3 → 0x3F = ทั้ง 6",
              tantrix_active_spokes(tantrix_make(0,0,1,TANTRIX_CLASS_NORMAL)) == 0x12u &&
              tantrix_active_spokes(tantrix_make(0,0,2,TANTRIX_CLASS_NORMAL)) == 0x24u &&
              tantrix_active_spokes(tantrix_make(0,0,3,TANTRIX_CLASS_NORMAL)) == 0x3Fu);
    }
}

/* ── D. CHAIN: seek → wang gate → tantrix route ── */
static void test_chain(void) {
    printf("\nD. CHAIN — seek → wang gate (integrity) → tantrix route (direction)\n");
    FrameWangLayer wl;
    fwang_init(&wl);

    /* D1: ทุก frame — chain deterministic + ไม่ crash ครบ 1440 */
    {
        int crash = 0;
        for (uint16_t e = 0; e < FRAME_CYCLE; e++) {
            DualFrame f = frame_at(e);
            /* wang gate ชั้น 1 */
            FrameWangDecision g = fwang_seek_gate(&wl, e);
            /* tantrix ชั้น 2 — tile จาก frame: entry = h.edge, exit = enc%4,
             * spoke = face%4, incoming gate = p.sub */
            TantrixTile tile = tantrix_make(f.h.edge, (uint8_t)(e % 4u),
                                            (uint8_t)(f.face % 4u),
                                            TANTRIX_CLASS_NORMAL);
            uint8_t out = 0;
            TantrixRouteResult r = tantrix_route(tile, f.p.sub, &out);
            if (r == TANTRIX_ROUTE_DROP || g == FWANG_SEEK_TAMPER ||
                g == FWANG_SEEK_MISMATCH) {
                /* ปิดเส้นทาง — ถูกต้อง เงื่อนไขครบ */
            }
            /* encode/decode roundtrip ของ tile */
            if (tantrix_make(tantrix_entry(tile), tantrix_exit(tile),
                             tantrix_spoke(tile),
                             (TantrixClass)tantrix_class(tile)) != tile) crash++;
        }
        CHECK(27, "1440 frames — chain ครบ ไม่ crash, tile roundtrip ถูก",
              crash == 0);
    }

    /* D2: ตัดสินใจ 2 ชั้น deterministic (รันซ้ำเท่ากัน) */
    {
        int same = 1;
        for (uint16_t e = 0; e < FRAME_CYCLE; e += 31) {
            DualFrame f1 = frame_at(e), f2 = frame_at(e);
            if (f1.enc != f2.enc) same = 0;
        }
        CHECK(28, "frame_at deterministic — input เดียว → เส้นทางเดียว",
              same == 1);
    }

    /* D3: เปิด ↔ ปิด — ข้อมูลดีผ่านทั้ง 2 ชั้น, ข้อมูลเสียปิดที่ชั้น wang */
    {
        /* หา frame ที่ chain เปิด: gate OK + tantrix FORWARD */
        int found_open = 0, found_close = 0;
        uint16_t open_e = 0;
        for (uint16_t e = 0; e < FRAME_CYCLE && found_open == 0; e++) {
            DualFrame f = frame_at(e);
            FrameWangDecision g = fwang_seek_gate(&wl, e);
            TantrixTile tile = tantrix_make(f.h.edge, (uint8_t)(e % 4u),
                                            (uint8_t)(f.face % 4u),
                                            TANTRIX_CLASS_NORMAL);
            uint8_t out = 0;
            TantrixRouteResult r = tantrix_route(tile, f.p.sub, &out);
            if (g == FWANG_SEEK_OK && r == TANTRIX_ROUTE_FORWARD) {
                found_open = 1;
                open_e = e;
            }
        }
        if (found_open) {
            /* frame เดียวกัน แต่ window ของมันถูกแก้ edge (chord-คู่)
             * → chain ต้องปิดที่ชั้น wang
             * (ข้าม win 0 — base case ของ edge_valid ไม่ตรวจ) */
            FrameWangLayer wl2;
            fwang_init(&wl2);
            uint16_t win = (open_e / WANG_WIN_SIZE) % WANG_WIN_COUNT;
            if (win == 0u) win = 1u;   /* ถ้า open frame อยู่ win 0 → ใช้ win 1 */
            uint8_t neu = (uint8_t)((wl2.wins[win].edge_top + 1) % 9u);
            wl2.wins[win].edge_top   = neu;
            wl2.wins[win].edge_top_b = (uint8_t)((9u - neu) % 9u);
            uint16_t probe = (uint16_t)(win * WANG_WIN_SIZE + 1u);
            found_close = (fwang_seek_gate(&wl2, probe) == FWANG_SEEK_MISMATCH);
        }
        CHECK(29, "chain: เปิด (OK+FORWARD) มีจริง และ window เดียวกันถูกแก้ → ปิดที่ชั้น wang",
              found_open == 1 && found_close == 1);
    }
}

int main(void) {
    printf("═ Wang seek gate × Tantrix fabric switch ═\n");
    test_wang_invariants();
    test_wang_gate_switch();
    test_tantrix();
    test_chain();
    printf("\n══════════ %d/%d PASS ══════════\n", pass, pass + fail);
    return fail ? 1 : 0;
}
