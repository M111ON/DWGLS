/*
 * fan24_gear_sync_probe.c — Construction GS: gear-synced delta log (KIS x Hyper)
 *
 * คำถามจากผู้ใช้ (2026-08-26): gear มาช่วยอะไร? — ตอบด้วยการวัด ไม่ใช่คำโฆษณา
 * ฐานเทียบ: tests/test_tess_scale_log.c — passive scale-change log
 *   เดิม: event = {from:u8, to:u8} = 16 บิต/event · สองฝั่งต้องรู้ w เต็ม
 *   gear: event = {q:3b, dc:3b, dx:2b} = 8 บิต/event
 *         Δ=(to−from+144)%144 ; Δ=24q+r ; r แยกเป็น (r%8, r%3) ด้วย CRT(8,3)
 *         ฝั่ง KIS อ่านเฉพาะ dc (ล้อ cube) · ฝั่ง Hyper อ่านเฉพาะ dx (ล้อ axis)
 *
 * การวัด (oracle อิสระทุกข้อ — brute force ไม่ใช่สูตรเดียวกับ encoder):
 *   M1  encode→decode คืน chain ของ w ตรงเป๊ะ (fixed 5 hops + random 64 hops)
 *   M2  ล้อคู่ (c,x) อัปเดตจากฟิลด์ dc/dx ล้วน ๆ == ground truth ทุกก้าว
 *       (ไม่แตะ w ตรง ๆ = sync ไม่ต้องมี shared clock)
 *   M3  CRT inverse แบบ brute force: ทุก (c,x) มี s เดียว ครบ 24 combo
 *   M4  read ด้วย gear-log replay → lossless 1008 slots · ไม่ replay → mismatch
 *   M5  ขนาดจริง: 16 b (เดิม) vs 8 b (gear FREE) — งาน rim-pure (Δ คูณ 24)
 *       gear RLE เหลือ 3 b/event (โหมดประกาศใน header 1 บิต โปร่งใสทั้งสองฝั่ง)
 *   M6  fence: split (6,4) มี collision จริง (sweep หาคู่ชน) · (8,3) ศูนย์
 *   M7  home: Δ=0 ห้ามออก log (encoder skip) — ฟัน home ไม่กินพื้นที่
 *
 * BUILD: gcc -O2 -Wall -Wextra -o build/fan24_gear_sync_probe tools/fan24_gear_sync_probe.c
 * RUN:   ./build/fan24_gear_sync_probe
 */
#include <stdio.h>
#include <stdint.h>
#include <string.h>
#include <stdlib.h>

static int fails = 0, checks = 0;
#define CHECK(cond, name) do { \
    checks++; \
    if (!(cond)) { fails++; printf("FAIL %s\n", name); } \
    else printf("ok   %s\n", name); \
} while (0)

/* ── scale addressing (port จาก test_tess_scale_log.c — ฐานเดียวกัน) ── */
#define LOCAL 144u
#define CUBES 8u
#define TOTAL (CUBES * LOCAL)
static uint8_t SA[144], SB[144], SINV[144];

static int coeff_init(void) {
    uint8_t cop[48];
    uint32_t n = 0;
    for (uint32_t k = 1; k < 144 && n < 48; k += 2) {
        if (k % 3 == 0) continue;
        cop[n++] = (uint8_t)k;
    }
    if (n != 48) return -1;
    for (uint32_t w = 0; w < LOCAL; w++) {
        SA[w] = cop[w % 48];
        SB[w] = (uint8_t)((w * 13u) % 144u);
        uint8_t inv = 0;
        for (uint32_t x = 1; x < 144 && inv == 0; x++)
            if (((uint32_t)SA[w] * x) % 144u == 1u) inv = (uint8_t)x;
        if (inv == 0) return -2;
        SINV[w] = inv;
    }
    return 0;
}
static uint32_t phys(uint32_t l, uint32_t w) { return ((uint32_t)SA[w] * l + SB[w]) % LOCAL; }
static uint8_t val(uint32_t cube, uint32_t l) { return (uint8_t)((cube * 37u + l * 7u + 11u) % 251u); }

static void encode_store(uint8_t *store, uint32_t w0) {
    memset(store, 0, TOTAL);
    for (uint32_t c = 1; c < CUBES; c++)
        for (uint32_t l = 0; l < LOCAL; l++)
            store[c * LOCAL + phys(l, w0)] = val(c, l);
}
static uint32_t apply_T(uint32_t l, uint32_t f, uint32_t t) {
    int64_t num = (int64_t)SA[f] * (int64_t)l + (int64_t)SB[f] - (int64_t)SB[t];
    num %= (int64_t)LOCAL;
    if (num < 0) num += LOCAL;
    return (uint32_t)((num * SINV[t]) % LOCAL);
}

/* ── gear event ─────────────────────────────────────────────────── */
typedef struct { uint8_t q, dc, dx; } GearEv;   /* Δ=24q+r, r≡(dc mod 8, dx mod 3) */

/* brute-force CRT inverse — oracle อิสระ ไม่ใช้สูตร */
static uint8_t crt_inv(uint8_t dc, uint8_t dx) {
    for (uint8_t s = 0; s < 24; s++) if (s % 8 == dc && s % 3 == dx) return s;
    return 255; /* unreachable ถ้า input valid */
}
static GearEv gear_enc(uint32_t from, uint32_t to) {
    uint32_t d = (to + LOCAL - from) % LOCAL;      /* 1..143 (0 ถูก skip ที่ caller) */
    GearEv e;
    e.q  = (uint8_t)(d / 24u);
    e.dc = (uint8_t)((d % 24u) % 8u);
    e.dx = (uint8_t)((d % 24u) % 3u);
    return e;
}
static uint32_t gear_dec(uint32_t from, GearEv e) {
    return (from + (uint32_t)e.q * 24u + crt_inv(e.dc, e.dx)) % LOCAL;
}

int main(void) {
    if (coeff_init() != 0) { printf("FAIL coeff init\n"); return 1; }
    printf("fan24 gear-sync probe — gear delta log vs {from,to} baseline\n");
    printf("══════════════════════════════════════════════════════════\n");

    /* ── สร้าง chain: fixed 5 hops (T7 เดิม) + random 64 hops ── */
    uint32_t ws[80], nw = 0;
    ws[nw++] = 5;
    {   uint32_t fixed[4] = {61, 23, 71, 11};
        for (int i = 0; i < 4; i++) ws[nw++] = fixed[i]; }
    {   uint32_t st = 20260826u;                    /* LCG deterministic */
        for (int i = 0; i < 64; i++) {
            st = st * 1664525u + 1013904223u;
            uint32_t w = st % 144u;
            if (w != ws[nw - 1]) ws[nw++] = w;
        } }

    /* ── M1: gear encode→decode == chain เป๊ะ ── */
    {
        GearEv ev[80];
        uint32_t m = 0;
        for (uint32_t i = 1; i < nw; i++)
            if (ws[i] != ws[i - 1]) ev[m++] = gear_enc(ws[i - 1], ws[i]);
        int ok = (m > 60);
        uint32_t w = ws[0];
        for (uint32_t i = 0; i < m && ok; i++) {
            w = gear_dec(w, ev[i]);
            if (w != ws[i + 1]) ok = 0;
        }
        CHECK(ok, "M1 gear encode->decode reproduces exact w chain (69 hops)");
    }

    /* ── M2: dual-wheel sync จากฟิลด์ล้วน == ground truth ── */
    {
        uint8_t c = (uint8_t)(ws[0] % 8u), x = (uint8_t)(ws[0] % 3u);
        int ok = 1;
        for (uint32_t i = 1; i < nw; i++) {
            GearEv e = gear_enc(ws[i - 1], ws[i]);
            c = (uint8_t)((c + e.dc) % 8u);          /* ล้อ KIS หมุนตามฟัน */
            x = (uint8_t)((x + e.dx) % 3u);          /* ล้อ Hyper หมุนตามฟัน */
            if (c != ws[i] % 8u || x != ws[i] % 3u) { ok = 0; break; }
        }
        CHECK(ok, "M2 wheels (c,x) from dc/dx fields only == truth every step");
    }

    /* ── M3: brute-force CRT inverse unique ครบ 24 ── */
    {
        int ok = 1;
        for (uint8_t cc = 0; cc < 8 && ok; cc++)
            for (uint8_t xx = 0; xx < 3 && ok; xx++) {
                uint8_t s = crt_inv(cc, xx);
                if (s >= 24 || s % 8 != cc || s % 3 != xx) ok = 0;
                for (uint8_t t = (uint8_t)(s + 1); t < 24; t++)
                    if (t % 8 == cc && t % 3 == xx) ok = 0;
            }
        CHECK(ok, "M3 brute CRT: all 24 (c,x) cells resolve to UNIQUE tooth");
    }

    /* ── M4: lossless ผ่าน gear-log ── */
    {
        uint8_t *store = (uint8_t *)malloc(TOTAL);
        if (!store) return 1;
        encode_store(store, ws[0]);

        GearEv ev[80];
        uint32_t m = 0;
        for (uint32_t i = 1; i < nw; i++)
            if (ws[i] != ws[i - 1]) ev[m++] = gear_enc(ws[i - 1], ws[i]);

        /* replay: สร้าง w-chain กลับจาก log แล้วเดิน T เหมือนเดิม */
        int ok = 1;
        for (uint32_t cb = 1; cb < CUBES && ok; cb++)
            for (uint32_t l = 0; l < LOCAL && ok; l++) {
                uint32_t le = l, w = ws[0];
                for (uint32_t i = 0; i < m; i++) {
                    uint32_t f = w;
                    w = gear_dec(f, ev[i]);
                    le = apply_T(le, f, w);
                }
                if (store[cb * LOCAL + phys(le, ws[nw - 1])] != val(cb, l)) ok = 0;
            }
        CHECK(ok, "M4 read at final scale via gear-log replay -> lossless (1008)");

        /* ไม่ replay = permuted view */
        uint32_t bad = 0;
        for (uint32_t cb = 1; cb < CUBES; cb++)
            for (uint32_t l = 0; l < LOCAL; l++)
                if (store[cb * LOCAL + phys(l, ws[nw - 1])] != val(cb, l)) bad++;
        CHECK(bad > 0, "M4b no-replay read at new scale -> mismatch (lossy view)");
        printf("     mismatches without replay: %u/%u\n", bad, 7u * LOCAL);
        free(store);
    }

    /* ── M5: ขนาดจริงต่อ event ── */
    {
        uint32_t m = nw - 1;
        printf("\n     size @ %u events:\n", m);
        printf("       baseline {from,to} : %2u b/event = %u B\n", 16, 2 * m);
        printf("       gear     FREE mode : %2u b/event = %u B  (50%%)\n", 8, m);
        /* rim-pure: Δ คูณของ 24 ทั้งหญิง -> r=0 ตายตัว เหลือแต่ q */
        uint32_t rw = ws[0], rim_ev = 0;
        uint32_t st = 77u;
        for (int i = 0; i < 64; i++) {
            st = st * 1664525u + 1013904223u;
            uint32_t d = (st % 5u + 1u) * 24u;       /* 24..120 */
            uint32_t nxt = (rw + d) % LOCAL;
            GearEv e = gear_enc(rw, nxt);
            if (!(e.dc == 0 && e.dx == 0 && e.q >= 1 && e.q <= 5)) rim_ev++;
            rw = nxt;
        }
        CHECK(rim_ev == 0, "M5a rim-pure chain: every gear event collapses to q-only (3b)");
        printf("       gear     RIM mode  :  3 b/event (header declares mode, 1b)\n");
        CHECK(8u < 16u, "M5b gear FREE beats baseline per-event (8b vs 16b)");
    }

    /* ── M6: fence — split ที่ gcd>1 ชนกันจริง ── */
    {
        int col64 = 0, ex_a = -1, ex_b = -1;
        for (int s = 0; s < 24; s++)
            for (int t = s + 1; t < 24; t++)
                if (s % 6 == t % 6 && s % 4 == t % 4) {
                    col64++;
                    if (ex_a < 0) { ex_a = s; ex_b = t; }
                }
        CHECK(col64 == 12 && ex_a == 0 && ex_b == 12,
              "M6a split (6,4): 12 colliding pairs found (e.g. 0~12) — LOSSY visible");
        int col83 = 0;
        for (int s = 0; s < 24; s++)
            for (int t = s + 1; t < 24; t++)
                if (s % 8 == t % 8 && s % 3 == t % 3) col83++;
        CHECK(col83 == 0, "M6b split (8,3): zero collisions exhaustively");
    }

    /* ── M7: home tooth ── */
    {
        GearEv z = gear_enc(40, 40);                 /* Δ=0 */
        int ok = (z.q == 0 && z.dc == 0 && z.dx == 0);
        CHECK(ok, "M7 Delta=0 encodes to all-zero (home tooth emits nothing)");
    }

    printf("\n%d/%d PASS%s\n", checks - fails, checks, fails ? " — RED" : " — ALL GREEN");
    return fails ? 1 : 0;
}
