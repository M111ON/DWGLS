/*
 * test_goldberg_store.c — Goldberg decagram storage API (geo_goldberg_store.h)
 * ═══════════════════════════════════════════════════════════════════════════
 *
 * T1.2h — ประกอบ goldberg storage เข้าระบบเป็น header (ไม่ใช่แค่ probe)
 *
 * Proof:
 *   T1  ggs_init: level clamp 1..8, faces/hex_total/per_sphere ถูกต้อง
 *   T2  ggs_tile/ggs_dim: decagram addressing ตรงกับ geo_goldberg_decagram.h
 *   T3  small store: 100B → 2 chunks — lossless, stats ถูกต้อง
 *   T4  chunk-boundary: 64B/128B/640B (หลายเท่าของ 64) — lossless
 *   T5  partial tail: 1000B = 15.625 chunks → 16 — lossless
 *   T6  multi-sphere streaming: ใหญ่กว่า 1 sphere (5040 chunks) → หลาย sphere
 *       — เขียน→verify→destroy ทีละ sphere — lossless ทั้งก้อน
 *   T7  deterministic: store เดียวกัน 2 รอบ → stats ซ้ำกัน (replay ได้)
 *   T8  empty (0B) = OK โดยไม่แตะ tring
 *
 * BUILD: gcc -O2 -Wall -Icore -o build/test_goldberg_store tests/test_goldberg_store.c
 */

#include <stdio.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include "../core/geo_goldberg_store.h"

static int pass_count = 0, fail_count = 0;
#define CHECK(desc, cond) do { \
    if (cond) { pass_count++; printf("  T: PASS — %s\n", desc); } \
    else      { fail_count++; printf("  T: FAIL — %s\n", desc); } \
} while (0)

/* deterministic pseudo-random fill */
static void fill(uint8_t *buf, uint64_t n, uint32_t seed)
{
    uint32_t x = seed;
    for (uint64_t i = 0; i < n; i++) {
        x = x * 1664525u + 1013904223u;
        buf[i] = (uint8_t)(x >> 24);
    }
}

int main(void)
{
    printf("═══ test_goldberg_store — decagram-Goldberg storage API (system) ═══\n\n");

    /* ── T1: init ─────────────────────────────────────────────────── */
    {
        GoldbergStore s;
        ggs_init(&s, 8);
        CHECK("T1a: level clamp 8", s.level == 8);
        CHECK("T1b: faces = 10·64+2 = 642", s.faces == 642);
        CHECK("T1c: hex_total = 10(64−1) = 630", s.hex_total == 630);
        CHECK("T1d: per_sphere = 630×8 = 5040 chunks",
              s.per_sphere == 5040 && s.per_sphere == (uint64_t)630 * GP_MAX_DIM);
        CHECK("T1e: stats เริ่ม 0", s.chunks_stored == 0 && s.bytes_stored == 0);

        GoldbergStore s2;
        ggs_init(&s2, 99);   /* over-range → clamp */
        CHECK("T1f: level clamp 99→8", s2.level == 8);
        ggs_init(&s2, 0);    /* under-range → clamp 1 */
        CHECK("T1g: level clamp 0→1", s2.level == 1 && s2.faces == 12 && s2.hex_total == 0);
        CHECK("T1h: per_sphere @L1 = 0 chunks", s2.per_sphere == 0);
    }

    /* ── T2: addressing ตรงกับ decagram ───────────────────────────── */
    {
        GoldbergStore s;
        ggs_init(&s, 8);
        int ok = 1;
        for (uint64_t k = 0; k < 5040; k++) {
            uint32_t tile = ggs_tile(s.level, k);
            uint8_t  dim  = ggs_dim(s.level, k);
            if (tile != GGD_PENTAGONS + (uint32_t)(k % 630)) ok = 0;
            if (dim  != (uint8_t)(k / 630)) ok = 0;
            if (tile >= s.faces || dim >= GP_MAX_DIM) ok = 0;
        }
        CHECK("T2a: ggs_tile/ggs_dim = decagram addressing ครบ 5040 chunks", ok);
        CHECK("T2b: k=0 → tile 12, dim 0",
              ggs_tile(8, 0) == 12 && ggs_dim(8, 0) == 0);
        CHECK("T2c: k=630 → tile 12, dim 1 (รอบ dim ต่อ hex)",
              ggs_tile(8, 630) == 12 && ggs_dim(8, 630) == 1);
        CHECK("T2d: k=5039 → tile 641, dim 7 (สุดท้าย)",
              ggs_tile(8, 5039) == 641 && ggs_dim(8, 5039) == 7);
    }

    /* ── T3: small store (100B = 2 chunks) ────────────────────────── */
    {
        uint8_t data[100];
        fill(data, sizeof data, 7);
        GoldbergStore s;
        ggs_init(&s, 8);
        CHECK("T3a: ggs_store 100B = 0 (lossless)", ggs_store(&s, data, 100) == 0);
        CHECK("T3b: chunks_stored = 2", s.chunks_stored == 2);
        CHECK("T3c: bytes_stored = 128 (2×64B)", s.bytes_stored == 128);
        CHECK("T3d: ggs_spheres(100B) = 1", ggs_spheres(&s, (100 + 63) / 64) == 1);
    }

    /* ── T4: chunk-boundary sizes ─────────────────────────────────── */
    {
        uint64_t sizes[] = { 64, 128, 640, 630 * 64 }; /* 630×64 = 1 dim ครบ */
        int ok = 1;
        for (int i = 0; i < 4; i++) {
            uint8_t *data = (uint8_t *)malloc(sizes[i]);
            fill(data, sizes[i], 11 + (uint32_t)i);
            GoldbergStore s;
            ggs_init(&s, 8);
            if (ggs_store(&s, data, sizes[i]) != 0) ok = 0;
            if (s.chunks_stored != sizes[i] / 64) ok = 0;
            free(data);
        }
        CHECK("T4: 64/128/640/40320B lossless + chunks นับถูก", ok);
    }

    /* ── T5: partial tail (1000B = 16 chunks, chunk 16 เหลือ 40B) ── */
    {
        uint8_t data[1000];
        fill(data, sizeof data, 23);
        GoldbergStore s;
        ggs_init(&s, 8);
        CHECK("T5a: ggs_store 1000B lossless", ggs_store(&s, data, 1000) == 0);
        CHECK("T5b: chunks = 16", s.chunks_stored == 16);
        CHECK("T5c: bytes_stored = 1024", s.bytes_stored == 1024);
    }

    /* ── T6: multi-sphere streaming (ใหญ่กว่า 1 sphere) ───────────── */
    {
        /* 5040 chunks/sphere → 5040×64 = 322,560B ต่อ sphere
         * ใช้ 3 sphere เต็ม + tail: 2.5 sphere ≈ 1.1MB */
        uint64_t n = 322560 * 2 + 100000;   /* 745,120B = 11,643 chunks */
        uint8_t *data = (uint8_t *)malloc(n);
        fill(data, n, 31);
        GoldbergStore s;
        ggs_init(&s, 8);
        CHECK("T6a: spheres(n) = 3", ggs_spheres(&s, (n + 63) / 64) == 3);
        CHECK("T6b: ggs_store 745KB streaming lossless", ggs_store(&s, data, n) == 0);
        CHECK("T6c: chunks_stored = 11643", s.chunks_stored == (n + 63) / 64);
        CHECK("T6d: bytes_stored = 11643×64", s.bytes_stored == 11643ULL * 64);
        free(data);
    }

    /* ── T7: deterministic — store ซ้ำ 2 รอบ → stats เท่ากัน ─────── */
    {
        uint8_t data[5000];
        fill(data, sizeof data, 41);
        GoldbergStore a, b;
        ggs_init(&a, 5);
        ggs_init(&b, 5);
        CHECK("T7a: store #1 lossless", ggs_store(&a, data, sizeof data) == 0);
        CHECK("T7b: store #2 lossless", ggs_store(&b, data, sizeof data) == 0);
        CHECK("T7c: stats ซ้ำกัน (replay/deterministic)",
              a.chunks_stored == b.chunks_stored && a.bytes_stored == b.bytes_stored);
        CHECK("T7d: L5 faces = 252, hex_total = 240",
              a.faces == 252 && a.hex_total == 240);
    }

    /* ── T8: empty ────────────────────────────────────────────────── */
    {
        GoldbergStore s;
        ggs_init(&s, 8);
        CHECK("T8a: ggs_store 0B = 0 (ไม่แตะ tring)", ggs_store(&s, NULL, 0) == 0);
        CHECK("T8b: stats = 0", s.chunks_stored == 0 && s.bytes_stored == 0);
    }

    printf("\n═══ RESULT: %d PASS / %d FAIL ═══\n", pass_count, fail_count);
    return fail_count ? 1 : 0;
}
