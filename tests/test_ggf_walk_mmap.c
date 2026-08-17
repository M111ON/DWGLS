/*
 * test_ggf_walk_mmap.c — save ผ่าน mmap view + walk clock ผ่าน GGFMap
 * ═══════════════════════════════════════════════════════════════════════
 *
 * T1.2m — (1) ggf_save_map: เขียน .ggf ผ่าน mmap view (อ่านด้วย GGFMap
 * ได้ทันที ไม่ต้อง reopen) · (2) geo_ggf_walk ใช้ GGFMap (zero-copy)
 *
 * Section A — ggf_save_map (geo_goldberg_file.h):
 *   A1  save_map 100B → ggf_map → chunk ตรง + CRC ผ่าน (อ่านได้ทันที)
 *   A2  save_map multi-sphere 745KB → random chunks + verify
 *   A3  deterministic: save_map == ggs_save ไฟล์ byte-for-byte เท่ากัน
 *   A4  empty 0B ผ่าน save_map → map ok
 *   A5  level 1 → ปฏิเสธ (ไม่มี hex tile)
 *   A6  corrupt หลัง save_map → verify จับ
 *   A7  tail partial 1000B → node สุดท้าย padded ตรง
 *
 * Section B — walk clock ผ่าน GGFMap (geo_ggf_walk.h):
 *   B1  resolve by state (seed/round/tick) → read_map == ต้นฉบับ 11/11
 *   B2  zero-copy: ggf_walk_node_map pointer ตรง chunk ต้นฉบับ (ไม่ copy)
 *   B3  dup → home map: อ่าน dup == dup และ == home · mapping เปิด = 8 home
 *   B4  enter-anywhere 3 start states → ทุก tensor ถึง (walk_to)
 *   B5  read_map == read (lazy) byte-for-byte (สอง path เห็นพ้อง)
 *   B6  tensor ว่าง → ปฏิเสธ
 *
 * BUILD: gcc -O2 -Wall -Icore -Icore/infra -o build/test_ggf_walk_mmap \
 *        tests/test_ggf_walk_mmap.c
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

static uint64_t rng = 555;
static uint64_t rnd(void)
{
    rng = rng * 6364136223846793005ULL + 1442695040888963407ULL;
    return rng >> 33;
}

#define NT 12u
static uint8_t *g_data[NT];
static uint32_t g_sizes[NT];
static int32_t  g_home[NT];
static char     g_paths[NT][96];
static const char *g_path_ptrs[NT];

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
        if (i == 4) memcpy(g_data[i], g_data[0], g_sizes[0]);
        if (i == 6) memcpy(g_data[i], g_data[1], g_sizes[1]);
        if (i == 8) memcpy(g_data[i], g_data[2], g_sizes[2]);
        sprintf(g_paths[i], "build/ggf_wm_t%02u.ggf", i);
        g_path_ptrs[i] = g_paths[i];
    }
}

int main(void)
{
    printf("═══ test_ggf_walk_mmap — save ผ่าน mmap view + walk ผ่าน GGFMap ═══\n\n");

    /* ══════════ Section A — ggf_save_map ══════════ */
    printf("— Section A: ggf_save_map (เขียน .ggf ผ่าน mmap view) —\n");

    /* A1: 100B */
    {
        uint8_t d[100];
        fill(d, sizeof d, 5);
        CHECK("A1a: save_map 100B ok", ggf_save_map(d, 100, 8, "build/ggf_wm_a1.ggf") == 0);
        GGFMap m;
        CHECK("A1b: ggf_map อ่านทันที (ไม่ต้อง reopen)", ggf_map("build/ggf_wm_a1.ggf", &m) == 0);
        CHECK("A1c: chunk ตรงต้นฉบับ", ggf_map_chunk(&m, 0, (uint8_t[GGS_CHUNK]){0}) == 0);
        const uint8_t *c0 = ggf_map_node(&m, 0, NULL);
        const uint8_t *c1 = ggf_map_node(&m, 1, NULL);
        CHECK("A1d: node 0 ตรง 64B + node 1 ตรง 36B (tail partial)",
              c0 && c1 && memcmp(c0, d, 64) == 0 && memcmp(c1, d + 64, 36) == 0);
        CHECK("A1e: CRC ผ่าน", ggf_map_verify(&m) == 0);
        ggf_unmap(&m);
        remove("build/ggf_wm_a1.ggf");
    }

    /* A2: multi-sphere 745KB */
    {
        uint64_t n = 322560 * 2 + 100000;
        uint8_t *d = (uint8_t *)malloc(n);
        fill(d, n, 7);
        CHECK("A2a: save_map 745KB ok", ggf_save_map(d, n, 8, "build/ggf_wm_a2.ggf") == 0);
        GGFMap m;
        CHECK("A2b: map ok", ggf_map("build/ggf_wm_a2.ggf", &m) == 0);
        CHECK("A2c: 3 spheres", m.n_spheres == 3);
        int ok = 1;
        for (int t = 0; t < 50 && ok; t++) {
            uint64_t k = rnd() % m.h.n_chunks;
            const uint8_t *p = ggf_map_node(&m, k, NULL);
            uint32_t cmp_n = (n - k * GGS_CHUNK >= GGS_CHUNK)
                             ? GGS_CHUNK : (uint32_t)(n - k * GGS_CHUNK);
            if (!p || memcmp(p, d + k * GGS_CHUNK, cmp_n) != 0) ok = 0;
        }
        CHECK("A2d: 50 random nodes ตรง (zero-copy)", ok);
        CHECK("A2e: CRC ผ่าน", ggf_map_verify(&m) == 0);
        ggf_unmap(&m);
        free(d);
        remove("build/ggf_wm_a2.ggf");
    }

    /* A3: deterministic — save_map == ggs_save byte-for-byte */
    {
        uint8_t d[3000];
        fill(d, sizeof d, 11);
        CHECK("A3a: save_map ok", ggf_save_map(d, sizeof d, 8, "build/ggf_wm_a3a.ggf") == 0);
        CHECK("A3b: ggs_save ok", ggs_save(d, sizeof d, 8, "build/ggf_wm_a3b.ggf") == 0);
        FILE *a = fopen("build/ggf_wm_a3a.ggf", "rb");
        FILE *b = fopen("build/ggf_wm_a3b.ggf", "rb");
        int same = 1, ca, cb;
        do { ca = fgetc(a); cb = fgetc(b); if (ca != cb) same = 0; } while (ca != EOF);
        if (ca != EOF || cb != EOF) same = 0;
        fclose(a); fclose(b);
        CHECK("A3c: ไฟล์ byte-for-byte เท่ากัน (layout เดียวกัน)", same);
        remove("build/ggf_wm_a3a.ggf");
        remove("build/ggf_wm_a3b.ggf");
    }

    /* A4: empty */
    {
        CHECK("A4a: save_map 0B ok", ggf_save_map(NULL, 0, 8, "build/ggf_wm_a4.ggf") == 0);
        GGFMap m;
        CHECK("A4b: map 0B ok · node NULL", ggf_map("build/ggf_wm_a4.ggf", &m) == 0 &&
              ggf_map_node(&m, 0, NULL) == NULL);
        ggf_unmap(&m);
        remove("build/ggf_wm_a4.ggf");
    }

    /* A5: level 1 */
    {
        uint8_t d[64];
        fill(d, sizeof d, 13);
        CHECK("A5: level 1 → ปฏิเสธ (rc=-9)", ggf_save_map(d, 64, 1, "build/ggf_wm_a5.ggf") == -9);
    }

    /* A6: corrupt หลัง save_map */
    {
        uint8_t d[512];
        fill(d, sizeof d, 17);
        CHECK("A6a: save_map ok", ggf_save_map(d, sizeof d, 8, "build/ggf_wm_a6.ggf") == 0);
        FILE *f = fopen("build/ggf_wm_a6.ggf", "r+b");
        fseek(f, 64 + 4 + 4 + 40, SEEK_SET);
        uint8_t b;
        fread(&b, 1, 1, f);
        b ^= 0xFF;
        fseek(f, -1, SEEK_CUR);
        fwrite(&b, 1, 1, f);
        fclose(f);
        GGFMap m;
        CHECK("A6b: map ok", ggf_map("build/ggf_wm_a6.ggf", &m) == 0);
        CHECK("A6c: verify จับ corrupt", ggf_map_verify(&m) != 0);
        ggf_unmap(&m);
        remove("build/ggf_wm_a6.ggf");
    }

    /* A7: tail partial 1000B */
    {
        uint8_t d[1000];
        fill(d, sizeof d, 19);
        CHECK("A7a: save_map ok", ggf_save_map(d, 1000, 8, "build/ggf_wm_a7.ggf") == 0);
        GGFMap m;
        CHECK("A7b: map ok", ggf_map("build/ggf_wm_a7.ggf", &m) == 0);
        const uint8_t *c = ggf_map_node(&m, 15, NULL);
        CHECK("A7c: tail 40B ตรง + พักเป็น 0", c && memcmp(c, d + 960, 40) == 0 &&
              c[40] == 0 && c[63] == 0);
        CHECK("A7d: ไฟล์บนดิสก์ถูกต้อง (CRC เหนือ data ทั้งหมดผ่าน)", ggf_map_verify(&m) == 0);
        ggf_unmap(&m);
        remove("build/ggf_wm_a7.ggf");
    }

    /* ══════════ Section B — walk clock ผ่าน GGFMap ══════════ */
    printf("\n— Section B: walk clock + zero-copy (GGFMap) —\n");
    make_tensors();

    tied_dedup_scan((const uint8_t *const *)g_data, g_sizes, NT, g_home);
    for (uint32_t i = 0; i < NT; i++) {
        if (g_home[i] != (int32_t)i || g_sizes[i] == 0) continue;
        CHECK("B0: save home ผ่าน save_map", ggf_save_map(g_data[i], g_sizes[i], 8, g_paths[i]) == 0);
    }

    uint32_t ticks = 12, cycles = 144, seed = 42;
    uint32_t rq[NT];
    GgfWalkTable tbl;
    ggf_walk_init(&tbl, seed, ticks, cycles, NT, g_path_ptrs, g_sizes, g_home, rq);
    GGFMap cache[NT];
    memset(cache, 0, sizeof cache);

    /* B1: resolve by state → read_map == ต้นฉบับ */
    {
        uint8_t *scratch = (uint8_t *)malloc(1024 * 1024);
        int all = 1;
        for (uint32_t i = 0; i < NT; i++) {
            if (g_home[i] < 0) continue;
            FiboWalkPos start = { 7, 3, 0 };
            FiboWalkPos end;
            if (!ggf_walk_to(&tbl, start, i, &end)) { all = 0; break; }
            uint64_t got = 0;
            if (ggf_walk_read_map(&tbl, i, cache, scratch, 1024 * 1024, &got) != 0 ||
                got != g_sizes[i] ||
                memcmp(scratch, g_data[i], g_sizes[i]) != 0) { all = 0; break; }
        }
        CHECK("B1: resolve by state → read_map ตรงต้นฉบับ 11/11", all);
        free(scratch);
    }

    /* B2: zero-copy node pointer */
    {
        const uint8_t *p = ggf_walk_node_map(&tbl, 5, 10, cache);
        CHECK("B2: zero-copy pointer ตรง chunk 10 ของ tensor 5",
              p && memcmp(p, g_data[5] + 640, 64) == 0);
    }

    /* B3: dup → home map + lazy count */
    {
        const uint8_t *p4 = ggf_walk_node_map(&tbl, 4, 0, cache);   /* dup → home 0 */
        CHECK("B3a: dup t4 อ่านจาก home map (t0) ตรงทั้ง dup และ home",
              p4 && memcmp(p4, g_data[4], 64) == 0 && memcmp(p4, g_data[0], 64) == 0);
        uint32_t mapped = 0;
        for (uint32_t i = 0; i < NT; i++) if (cache[i].base) mapped++;
        CHECK("B3b: lazy — อ่าน 11 tensor เปิด 8 home maps (dup ใช้ map ของ home)",
              mapped == 8);
    }

    /* B4: enter-anywhere */
    {
        FiboWalkPos starts[3] = { {0, 0, 0}, {143, 5, 0}, {72, 11, 0} };
        int all = 1;
        for (int s = 0; s < 3 && all; s++)
            for (uint32_t i = 0; i < NT && all; i++) {
                if (g_home[i] < 0) continue;
                FiboWalkPos end;
                if (!ggf_walk_to(&tbl, starts[s], i, &end)) { all = 0; break; }
            }
        CHECK("B4: เดินจาก 3 start states → ทุก tensor ถึง", all);
    }

    /* B5: read_map == read (lazy) byte-for-byte */
    {
        GGFReader rcache[NT];
        memset(rcache, 0, sizeof rcache);
        uint8_t *a = (uint8_t *)malloc(1024 * 1024);
        uint8_t *b = (uint8_t *)malloc(1024 * 1024);
        int same = 1;
        for (uint32_t i = 0; i < NT && same; i++) {
            if (g_home[i] < 0) continue;
            uint64_t ga = 0, gb = 0;
            if (ggf_walk_read_map(&tbl, i, cache, a, 1024 * 1024, &ga) != 0 ||
                ggf_walk_read(&tbl, i, rcache, b, 1024 * 1024, &gb) != 0 ||
                ga != gb || memcmp(a, b, ga) != 0) same = 0;
        }
        CHECK("B5: read_map == read (lazy) ทุก byte", same);
        free(a); free(b);
        for (uint32_t i = 0; i < NT; i++) ggf_close(&rcache[i]);
    }

    /* B6: ว่าง → ปฏิเสธ */
    {
        uint8_t scratch[64];
        uint64_t got = 0;
        CHECK("B6: tensor ว่าง (t9) → read_map = -1",
              ggf_walk_read_map(&tbl, 9, cache, scratch, sizeof scratch, &got) == -1);
    }

    for (uint32_t i = 0; i < NT; i++) ggf_unmap(&cache[i]);
    for (uint32_t i = 0; i < NT; i++) {
        remove(g_paths[i]);
        free(g_data[i]);
    }

    printf("\n═══ RESULT: %d PASS / %d FAIL ═══\n", pass_count, fail_count);
    return fail_count ? 1 : 0;
}
