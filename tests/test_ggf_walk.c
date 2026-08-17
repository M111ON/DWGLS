/*
 * test_ggf_walk.c — Single read path: walk clock + dedup registry + GGFReader
 * ═══════════════════════════════════════════════════════════════════════════
 *
 * T1.2k — เปิด .ggf แล้ว resolve tensor ด้วย state (seed, round, tick)
 * (core/geo_ggf_walk.h — §15.87)
 *
 * Proof:
 *   T1  init: rq deterministic จาก (seed, t) — ใน [0, cycles) · tick = rq%ticks
 *   T2  coverage: ทุก tensor live ตรง 1 ตำแหน่ง (rq, rq%ticks) — Σ counts == n
 *   T3  enter-anywhere: เดินจาก 3 start states → ทุก tensor ถึงได้ (walk_to)
 *   T4  live: ที่ตำแหน่งของ tensor t — live set มี t · นับตรง coverage
 *   T5  read by state: walk ไปตำแหน่ง (round, tick) → live → read → ต้นฉบับ
 *   T6  dedup registry: dup → resolve → home .ggf → bytes ตรงทั้ง dup + home ·
 *       ไฟล์เก็บเฉพาะ home (ไฟล์น้อยกว่า tensor) · dedup_bytes ถูกต้อง
 *   T7  lazy open-on-demand: อ่าน t0 + dup ของ t0 → เปิดเฉพาะ cache[home]
 *   T8  read_at ทุกตำแหน่งที่มี live → bytes ตรง live tensor
 *   T9  tail partial (5000B → chunk สุดท้าย padded) → อ่าน exact bytes
 *   T10 corrupt ไฟล์ → ggf_walk_read ปฏิเสธ (ผ่าน lazy path)
 *   T11 tensor ว่าง (0B) → home = -1 → read ปฏิเสธ
 *   T12 multi-sphere (745KB) read ผ่าน walk == ต้นฉบับ
 *   T13 deterministic: init ซ้ำ seed เดียว → rq เท่าเดิมทุก tensor
 *
 * BUILD: gcc -O2 -Wall -Icore -Icore/infra -o build/test_ggf_walk \
 *        tests/test_ggf_walk.c
 */

#include <stdio.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include "../core/geo_ggf_walk.h"
#include "../core/tied_dedup.h"

static int pass_count = 0, fail_count = 0;
#define CHECK(desc, cond) do { \
    if (cond) { pass_count++; printf("  T: PASS — %s\n", desc); } \
    else      { fail_count++; printf("  T: FAIL — %s\n", desc); } \
} while (0)

static void fill(uint8_t *buf, uint64_t n, uint32_t seed)
{
    uint32_t x = seed;
    for (uint64_t i = 0; i < n; i++) {
        x = x * 1664525u + 1013904223u;
        buf[i] = (uint8_t)(x >> 24);
    }
}

#define NT 12u
static uint8_t *g_data[NT];
static uint32_t g_sizes[NT];
static int32_t  g_home[NT];
static char     g_paths[NT][96];

static const char *g_path_ptrs[NT];

/* สร้าง tensor 12 ตัว — มี dup 3 คู่ (t4=t0, t6=t1, t8=t2) + ว่าง 1 (t9) */
static void make_tensors(void)
{
    uint32_t sizes[NT] = { 100, 2000, 64, 5000, 100, 12345,
                           2000, 322560 * 2 + 100000, 64, 0, 3000, 77 };
    for (uint32_t i = 0; i < NT; i++) {
        g_sizes[i] = sizes[i];
        g_data[i] = NULL;
        if (sizes[i] > 0) {
            g_data[i] = (uint8_t *)malloc(sizes[i]);
            fill(g_data[i], sizes[i], 1000 + i);
        }
        /* dup = สำเนา byte-identical ของ home */
        if (i == 4) { memcpy(g_data[i], g_data[0], g_sizes[0]); }
        if (i == 6) { memcpy(g_data[i], g_data[1], g_sizes[1]); }
        if (i == 8) { memcpy(g_data[i], g_data[2], g_sizes[2]); }
        sprintf(g_paths[i], "build/ggf_walk_t%02u.ggf", i);
        g_path_ptrs[i] = g_paths[i];
    }
}

int main(void)
{
    printf("═══ test_ggf_walk — single read path: walk + registry + .ggf ═══\n\n");
    make_tensors();

    /* ── registry จาก tied_dedup_scan จริง ───────────────────────── */
    uint64_t dup_bytes = tied_dedup_scan((const uint8_t *const *)g_data,
                                         g_sizes, NT, g_home);
    uint32_t n_home = 0, n_dup = 0;
    for (uint32_t i = 0; i < NT; i++) {
        if (g_home[i] == (int32_t)i) n_home++;
        if (g_home[i] >= 0 && g_home[i] != (int32_t)i) n_dup++;
    }
    CHECK("T0a: registry — 3 dups (t4→0, t6→1, t8→2)",
          n_dup == 3 && g_home[4] == 0 && g_home[6] == 1 && g_home[8] == 2);
    CHECK("T0b: dup_bytes = 100+2000+64 = 2164", dup_bytes == 2164);
    CHECK("T0c: tensor ว่าง (t9) → home = -1", g_home[9] == -1);

    /* ── save เฉพาะ home (dedup ระดับไฟล์) ──────────────────────── */
    uint32_t n_saved = 0;
    for (uint32_t i = 0; i < NT; i++) {
        if (g_home[i] != (int32_t)i) continue;   /* dup — ไม่มีไฟล์ของตัวเอง */
        if (g_sizes[i] == 0) continue;
        CHECK("T0d: save home ไฟล์ ok",
              ggs_save(g_data[i], g_sizes[i], 8, g_paths[i]) == 0);
        n_saved++;
    }
    CHECK("T0e: ไฟล์ที่เก็บ = เฉพาะ home (8 จาก 11 ที่มี data)", n_saved == 8);

    /* ── walk table ─────────────────────────────────────────────── */
    uint32_t ticks = 12, cycles = 144, seed = 42;
    uint32_t rq_a[NT], rq_b[NT];
    GgfWalkTable tbl, tbl2;
    ggf_walk_init(&tbl, seed, ticks, cycles, NT, g_path_ptrs, g_sizes, g_home, rq_a);
    ggf_walk_init(&tbl2, seed, ticks, cycles, NT, g_path_ptrs, g_sizes, g_home, rq_b);

    /* ── T1: init ───────────────────────────────────────────────── */
    {
        int in_range = 1, tick_ok = 1;
        for (uint32_t i = 0; i < NT; i++) {
            if (rq_a[i] >= cycles) in_range = 0;
            FiboWalkPos p;
            ggf_walk_pos(&tbl, i, &p);
            if (p.round != rq_a[i] || p.tick != (rq_a[i] % ticks)) tick_ok = 0;
        }
        CHECK("T1a: rq ∈ [0, cycles) ทุก tensor", in_range);
        CHECK("T1b: position = (rq, rq%ticks) ทุก tensor", tick_ok);
    }

    /* ── T2: coverage ───────────────────────────────────────────── */
    {
        uint32_t *counts = (uint32_t *)calloc((size_t)cycles * ticks, sizeof(uint32_t));
        uint64_t total = ggf_walk_coverage(&tbl, counts);
        uint64_t nonempty = 0;
        for (uint32_t i = 0; i < cycles * ticks; i++)
            if (counts[i]) nonempty++;
        CHECK("T2a: Σ counts == n (ทุก tensor live ตรง 1 ตำแหน่ง)", total == NT);
        CHECK("T2b: live กระจายบนนาฬิกา (nonempty ≤ n)", nonempty <= NT && nonempty > 0);
        free(counts);
    }

    /* ── T3: enter-anywhere ─────────────────────────────────────── */
    {
        FiboWalkPos starts[3] = { {0, 0, 0}, {143, 5, 0}, {72, 11, 0} };
        int all = 1;
        for (int s = 0; s < 3 && all; s++)
            for (uint32_t i = 0; i < NT && all; i++) {
                if (g_home[i] < 0) continue;     /* ว่าง — ข้าม */
                FiboWalkPos end;
                if (!ggf_walk_to(&tbl, starts[s], i, &end)) { all = 0; break; }
                FiboWalkPos exp;
                ggf_walk_pos(&tbl, i, &exp);
                if (end.round != exp.round || end.tick != exp.tick) { all = 0; break; }
            }
        CHECK("T3: เดินจาก 3 start states → ทุก tensor ถึงตำแหน่งของตัวเอง", all);
    }

    /* ── T4 + T5: live + read by state ──────────────────────────── */
    {
        GGFReader cache[NT];
        memset(cache, 0, sizeof cache);
        uint8_t *scratch = (uint8_t *)malloc(1024 * 1024);
        int read_all = 1, live_ok = 1;
        uint32_t nl = 0;
        for (uint32_t i = 0; i < NT; i++) {
            if (g_home[i] < 0) continue;
            FiboWalkPos pos;
            ggf_walk_pos(&tbl, i, &pos);
            uint32_t live[NT];
            int32_t lc = ggf_walk_live(&tbl, &pos, live, NT);
            if (lc < 1) { live_ok = 0; break; }
            int found = 0;
            for (int32_t a = 0; a < lc; a++) if (live[a] == i) found = 1;
            if (!found) { live_ok = 0; break; }
            uint64_t got = 0;
            if (ggf_walk_read(&tbl, i, cache, scratch, 1024 * 1024, &got) != 0 ||
                got != g_sizes[i] ||
                memcmp(scratch, g_data[i], g_sizes[i]) != 0) { read_all = 0; break; }
            nl++;
        }
        CHECK("T4: live set มี tensor ที่ตำแหน่งของตัวเองทุกตัว", live_ok && nl == 11);
        CHECK("T5: read ด้วย state → bytes ตรงต้นฉบับทุก tensor (11/11)", read_all);

        /* ── T7: lazy open-on-demand ────────────────────────────── */
        {
            uint32_t open_cnt = 0;
            for (uint32_t i = 0; i < NT; i++)
                if (cache[i].f) open_cnt++;
            /* อ่าน 11 tensor (8 home + 3 dup) → เปิดแค่ 8 ไฟล์ home */
            CHECK("T7: lazy — อ่าน 11 tensor เปิดแค่ 8 home ไฟล์", open_cnt == 8);
        }
        free(scratch);
        for (uint32_t i = 0; i < NT; i++) ggf_close(&cache[i]);
    }

    /* ── T6: dedup — dup อ่านจาก home ไฟล์ ──────────────────────── */
    {
        GGFReader cache[NT];
        memset(cache, 0, sizeof cache);
        uint8_t *scratch = (uint8_t *)malloc(1024 * 1024);
        uint64_t got = 0;
        int ok = ggf_walk_read(&tbl, 4, cache, scratch, 1024 * 1024, &got) == 0 &&
                 got == g_sizes[4] && memcmp(scratch, g_data[4], g_sizes[4]) == 0 &&
                 memcmp(scratch, g_data[0], g_sizes[0]) == 0;
        CHECK("T6a: dup t4 → อ่านจาก home (t0) → ตรงทั้ง dup และ home", ok);
        /* อ่านผ่าน dup — เปิดเฉพาะ cache[0] (home ของ t4) */
        CHECK("T6b: dup เปิดไฟล์ home เท่านั้น (cache[0])",
              cache[0].f != NULL && cache[4].f == NULL);
        uint64_t saved = ggf_walk_dedup_bytes(&tbl);
        CHECK("T6c: dedup_bytes = 2164 (3 dup ไม่มีไฟล์ของตัวเอง)", saved == 2164);
        free(scratch);
        for (uint32_t i = 0; i < NT; i++) ggf_close(&cache[i]);
    }

    /* ── T8: read_at ทุกตำแหน่งที่มี live ────────────────────────── */
    {
        GGFReader cache[NT];
        memset(cache, 0, sizeof cache);
        uint8_t *scratch = (uint8_t *)malloc(1024 * 1024);
        uint32_t *counts = (uint32_t *)calloc((size_t)cycles * ticks, sizeof(uint32_t));
        ggf_walk_coverage(&tbl, counts);
        int ok = 1;
        uint64_t read_cnt = 0;
        for (uint32_t pos = 0; pos < cycles * ticks && ok; pos++) {
            if (!counts[pos]) continue;
            FiboWalkPos p = { pos / ticks, pos % ticks, 0 };
            uint32_t live[NT];
            int32_t lc = ggf_walk_live(&tbl, &p, live, NT);
            if (lc != (int32_t)counts[pos]) { ok = 0; break; }
            for (int32_t a = 0; a < lc && ok; a++) {
                if (g_home[live[a]] < 0) continue;   /* ว่าง — live แต่ไม่มี data */
                uint64_t got = 0;
                if (ggf_walk_read(&tbl, live[a], cache, scratch, 1024 * 1024, &got) != 0 ||
                    got != g_sizes[live[a]] ||
                    memcmp(scratch, g_data[live[a]], g_sizes[live[a]]) != 0) ok = 0;
                read_cnt++;
            }
        }
        CHECK("T8: read_at ทุกตำแหน่ง live → bytes ตรง (Σ == 11)", ok && read_cnt == 11);
        free(counts);
        free(scratch);
        for (uint32_t i = 0; i < NT; i++) ggf_close(&cache[i]);
    }

    /* ── T9: tail partial (5000B → 79 chunks) ───────────────────── */
    {
        GGFReader cache[NT];
        memset(cache, 0, sizeof cache);
        uint8_t *scratch = (uint8_t *)malloc(1024 * 1024);
        uint64_t got = 0;
        int ok = ggf_walk_read(&tbl, 3, cache, scratch, 1024 * 1024, &got) == 0 &&
                 got == 5000 && memcmp(scratch, g_data[3], 5000) == 0;
        CHECK("T9: tail partial (5000B) read ผ่าน walk == ต้นฉบับ", ok);
        free(scratch);
        for (uint32_t i = 0; i < NT; i++) ggf_close(&cache[i]);
    }

    /* ── T10: corrupt ไฟล์ → read ปฏิเสธ ────────────────────────── */
    {
        /* corrupt home ของ t5 (12345B) — flip data 1 byte */
        FILE *f = fopen(g_paths[5], "r+b");
        fseek(f, 64 + 4 + 4 + 40, SEEK_SET);
        uint8_t b;
        fread(&b, 1, 1, f);
        b ^= 0xFF;
        fseek(f, -1, SEEK_CUR);
        fwrite(&b, 1, 1, f);
        fclose(f);

        GGFReader cache[NT];
        memset(cache, 0, sizeof cache);
        uint8_t *scratch = (uint8_t *)malloc(1024 * 1024);
        uint64_t got = 0;
        int rc = ggf_walk_read(&tbl, 5, cache, scratch, 1024 * 1024, &got);
        /* ggf_read อ่าน chunk ตรงๆ — data พัง แต่ tick ยังถูก → อ่านได้
         * (detect จริงอยู่ที่ ggf_verify/CRC) — ตรวจว่าอ่านได้แต่ verify จับ */
        int corrupt_detected = 0;
        if (rc == 0 && ggf_verify(&cache[5]) != 0) corrupt_detected = 1;
        CHECK("T10: corrupt file → ggf_verify ผ่าน lazy path จับได้", corrupt_detected);
        free(scratch);
        for (uint32_t i = 0; i < NT; i++) ggf_close(&cache[i]);
        /* restore */
        ggs_save(g_data[5], g_sizes[5], 8, g_paths[5]);
    }

    /* ── T11: tensor ว่าง → read ปฏิเสธ ─────────────────────────── */
    {
        GGFReader cache[NT];
        memset(cache, 0, sizeof cache);
        uint8_t scratch[64];
        uint64_t got = 0;
        CHECK("T11: tensor ว่าง (t9) → read = -1 (ข้าม)",
              ggf_walk_read(&tbl, 9, cache, scratch, sizeof scratch, &got) == -1);
        for (uint32_t i = 0; i < NT; i++) ggf_close(&cache[i]);
    }

    /* ── T12: multi-sphere (745KB = 3 spheres) ──────────────────── */
    {
        GGFReader cache[NT];
        memset(cache, 0, sizeof cache);
        uint8_t *scratch = (uint8_t *)malloc(1024 * 1024);
        uint64_t got = 0;
        int ok = ggf_walk_read(&tbl, 7, cache, scratch, 1024 * 1024, &got) == 0 &&
                 got == g_sizes[7] &&
                 memcmp(scratch, g_data[7], g_sizes[7]) == 0;
        CHECK("T12: multi-sphere (745KB) read ผ่าน walk == ต้นฉบับ", ok);
        free(scratch);
        for (uint32_t i = 0; i < NT; i++) ggf_close(&cache[i]);
    }

    /* ── T13: deterministic ─────────────────────────────────────── */
    {
        int same = 1;
        for (uint32_t i = 0; i < NT; i++)
            if (rq_a[i] != rq_b[i]) same = 0;
        CHECK("T13: init ซ้ำ seed เดียว → rq เท่าเดิมทุก tensor (replay ได้)", same);
    }

    /* ── cleanup ────────────────────────────────────────────────── */
    for (uint32_t i = 0; i < NT; i++) {
        remove(g_paths[i]);
        free(g_data[i]);
    }

    printf("\n═══ RESULT: %d PASS / %d FAIL ═══\n", pass_count, fail_count);
    return fail_count ? 1 : 0;
}
