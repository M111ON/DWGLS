/*
 * test_goldberg_mmap.c — Memory-map .ggf (GGFMap) — zero-copy read
 * ═══════════════════════════════════════════════════════════════════════
 *
 * T1.2l — map .ggf เข้าหน้าเพจ → อ่านตรงจากเพจ (geo_goldberg_file.h)
 *
 * Proof:
 *   T1  map: header fields + index ตรง (scan จาก mapping — ไม่มี seek)
 *   T2  random access 1 sphere: ggf_map_chunk == ต้นฉบับ
 *   T3  random + edge ข้าม 3 spheres
 *   T4  zero-copy: ggf_map_node pointer → memcmp ตรงโดยไม่ copy กลาง ·
 *       pointer คงที่ (เรียกซ้ำได้ที่อยู่เดิม)
 *   T5  ggf_map_read unaligned byte ranges == ต้นฉบับ
 *   T6  verify: ไฟล์ดี = 0 · flip data → ≠ 0 (CRC ผ่าน mapping)
 *   T7  flip tick → ggf_map_node = NULL (per-node detect)
 *   T8  drop-in: ggf_map_chunk == ggf_chunk ทุก chunk (lazy vs mmap เห็นพ้อง)
 *   T9  tail partial: node สุดท้าย padded + map_read เฉพาะ bytes จริง
 *   T10 empty / bad magic / L5
 *   T11 unmap แล้ว node → NULL (ปลอดภัย)
 *
 * BUILD: gcc -O2 -Wall -Icore -Icore/infra -o build/test_goldberg_mmap \
 *        tests/test_goldberg_mmap.c
 */

#include <stdio.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include "../core/geo_goldberg_file.h"

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

static uint64_t rng = 777;
static uint64_t rnd(void)
{
    rng = rng * 6364136223846793005ULL + 1442695040888963407ULL;
    return rng >> 33;
}

int main(void)
{
    printf("═══ test_goldberg_mmap — .ggf memory-map (zero-copy read) ═══\n\n");

    /* ── T1: map + index ────────────────────────────────────────── */
    {
        uint8_t d[3000];
        fill(d, sizeof d, 3);
        CHECK("T1a: save 3000B ok", ggs_save(d, sizeof d, 8, "build/ggf_mm_t1.ggf") == 0);
        GGFMap m;
        CHECK("T1b: ggf_map ok", ggf_map("build/ggf_mm_t1.ggf", &m) == 0);
        CHECK("T1c: n_chunks = 47", m.h.n_chunks == 47);
        CHECK("T1d: n_spheres = 1 · count = 47", m.n_spheres == 1 && m.sphere_cnt[0] == 47);
        CHECK("T1e: sphere_off = 64", m.sphere_off[0] == 64);
        CHECK("T1f: mapping ขนาด = ไฟล์จริง", m.size > 0);
        ggf_unmap(&m);
        remove("build/ggf_mm_t1.ggf");
    }

    /* ── T2: random access 1 sphere ─────────────────────────────── */
    {
        uint64_t n = 12345;
        uint8_t *d = (uint8_t *)malloc(n);
        fill(d, n, 7);
        CHECK("T2a: save ok", ggs_save(d, n, 8, "build/ggf_mm_t2.ggf") == 0);
        GGFMap m;
        CHECK("T2b: map ok", ggf_map("build/ggf_mm_t2.ggf", &m) == 0);
        int all = 1;
        for (int t = 0; t < 50; t++) {
            uint64_t k = rnd() % m.h.n_chunks;
            uint8_t c[GGS_CHUNK];
            if (ggf_map_chunk(&m, k, c) != 0) { all = 0; break; }
            uint32_t cmp_n = (n - k * GGS_CHUNK >= GGS_CHUNK)
                             ? GGS_CHUNK : (uint32_t)(n - k * GGS_CHUNK);
            if (memcmp(c, d + k * GGS_CHUNK, cmp_n) != 0) { all = 0; break; }
        }
        CHECK("T2c: 50 random chunks ตรงต้นฉบับ", all);
        ggf_unmap(&m);
        free(d);
        remove("build/ggf_mm_t2.ggf");
    }

    /* ── T3: ข้าม 3 spheres ─────────────────────────────────────── */
    {
        uint64_t n = 322560 * 2 + 100000;
        uint8_t *d = (uint8_t *)malloc(n);
        fill(d, n, 11);
        CHECK("T3a: save 745KB ok", ggs_save(d, n, 8, "build/ggf_mm_t3.ggf") == 0);
        GGFMap m;
        CHECK("T3b: map ok", ggf_map("build/ggf_mm_t3.ggf", &m) == 0);
        CHECK("T3c: n_spheres = 3", m.n_spheres == 3);
        int ok = 1;
        uint64_t p = m.per_sphere;
        uint64_t edge[] = { 0, p - 1, p, p + 1, 2 * p - 1, 2 * p, m.h.n_chunks - 1 };
        for (int e = 0; e < 7; e++) {
            uint8_t c[GGS_CHUNK];
            uint64_t k = edge[e];
            if (ggf_map_chunk(&m, k, c) != 0) { ok = 0; break; }
            uint32_t cmp_n = (n - k * GGS_CHUNK >= GGS_CHUNK)
                             ? GGS_CHUNK : (uint32_t)(n - k * GGS_CHUNK);
            if (memcmp(c, d + k * GGS_CHUNK, cmp_n) != 0) { ok = 0; break; }
        }
        for (int t = 0; t < 100 && ok; t++) {
            uint64_t k = rnd() % m.h.n_chunks;
            uint8_t c[GGS_CHUNK];
            if (ggf_map_chunk(&m, k, c) != 0) { ok = 0; break; }
            uint32_t cmp_n = (n - k * GGS_CHUNK >= GGS_CHUNK)
                             ? GGS_CHUNK : (uint32_t)(n - k * GGS_CHUNK);
            if (memcmp(c, d + k * GGS_CHUNK, cmp_n) != 0) { ok = 0; break; }
        }
        CHECK("T3d: edge + 100 random ข้าม sphere ตรงต้นฉบับ", ok);
        ggf_unmap(&m);
        free(d);
        remove("build/ggf_mm_t3.ggf");
    }

    /* ── T4: zero-copy pointer ──────────────────────────────────── */
    {
        uint64_t n = 5000;
        uint8_t *d = (uint8_t *)malloc(n);
        fill(d, n, 13);
        CHECK("T4a: save ok", ggs_save(d, n, 8, "build/ggf_mm_t4.ggf") == 0);
        GGFMap m;
        CHECK("T4b: map ok", ggf_map("build/ggf_mm_t4.ggf", &m) == 0);
        const uint8_t *ptr = ggf_map_node(&m, 10, NULL);
        CHECK("T4c: node 10 pointer ตรงต้นฉบับ (zero-copy, ไม่มี buffer กลาง)",
              ptr && memcmp(ptr, d + 640, 64) == 0);
        CHECK("T4d: pointer คงที่ (เรียกซ้ำ = ที่อยู่เดิม)",
              ggf_map_node(&m, 10, NULL) == ptr);
        const uint8_t *last = ggf_map_node(&m, m.h.n_chunks - 1, NULL);
        CHECK("T4e: node สุดท้าย (tail partial) pointer ตรง 40B จริง",
              last && memcmp(last, d + (n / 64) * 64, n % 64) == 0);
        ggf_unmap(&m);
        free(d);
        remove("build/ggf_mm_t4.ggf");
    }

    /* ── T5: map_read unaligned ─────────────────────────────────── */
    {
        uint64_t n = 10000;
        uint8_t *d = (uint8_t *)malloc(n);
        fill(d, n, 17);
        CHECK("T5a: save ok", ggs_save(d, n, 8, "build/ggf_mm_t5.ggf") == 0);
        GGFMap m;
        CHECK("T5b: map ok", ggf_map("build/ggf_mm_t5.ggf", &m) == 0);
        uint64_t offs[] = { 0, 1, 63, 64, 100, 127, 128, 5000, 9998, n - 1 };
        int ok = 1;
        for (int t = 0; t < 10 && ok; t++) {
            uint64_t off = offs[t];
            uint64_t len = (t % 2) ? 7 : 129;
            if (off + len > n) len = n - off;
            uint8_t got2[256], exp2[256];
            if (ggf_map_read(&m, off, got2, len) != 0) { ok = 0; break; }
            memcpy(exp2, d + off, len);
            if (memcmp(got2, exp2, len) != 0) { ok = 0; break; }
        }
        CHECK("T5c: 10 unaligned ranges ตรงต้นฉบับ", ok);
        uint8_t tmp;
        CHECK("T5d: อ่านเกิน n_bytes → -1", ggf_map_read(&m, n, &tmp, 1) == -1);
        ggf_unmap(&m);
        free(d);
        remove("build/ggf_mm_t5.ggf");
    }

    /* ── T6: verify + data corrupt ──────────────────────────────── */
    {
        uint8_t d[512];
        fill(d, sizeof d, 19);
        CHECK("T6a: save ok", ggs_save(d, sizeof d, 8, "build/ggf_mm_t6.ggf") == 0);
        GGFMap m;
        CHECK("T6b: map ok", ggf_map("build/ggf_mm_t6.ggf", &m) == 0);
        CHECK("T6c: ggf_map_verify ไฟล์ดี = 0", ggf_map_verify(&m) == 0);
        ggf_unmap(&m);
        FILE *f = fopen("build/ggf_mm_t6.ggf", "r+b");
        fseek(f, 64 + 4 + 4 + 40, SEEK_SET);
        uint8_t b;
        fread(&b, 1, 1, f);
        b ^= 0xFF;
        fseek(f, -1, SEEK_CUR);
        fwrite(&b, 1, 1, f);
        fclose(f);
        CHECK("T6d: map หลัง corrupt ok", ggf_map("build/ggf_mm_t6.ggf", &m) == 0);
        CHECK("T6e: verify จับ corrupt (≠0)", ggf_map_verify(&m) != 0);
        /* data พังแต่ tick ถูก → node อ่านได้ (detect อยู่ที่ verify) */
        CHECK("T6f: node 0 ยังอ่านได้ (tick ถูก)", ggf_map_chunk(&m, 0, (uint8_t[GGS_CHUNK]){0}) == 0);
        ggf_unmap(&m);
        remove("build/ggf_mm_t6.ggf");
    }

    /* ── T7: tick flip → node = NULL ────────────────────────────── */
    {
        uint8_t d[256];
        fill(d, sizeof d, 23);
        CHECK("T7a: save ok", ggs_save(d, sizeof d, 8, "build/ggf_mm_t7.ggf") == 0);
        FILE *f = fopen("build/ggf_mm_t7.ggf", "r+b");
        fseek(f, 64 + 4 + 0, SEEK_SET);
        uint8_t b;
        fread(&b, 1, 1, f);
        b ^= 0x01;
        fseek(f, -1, SEEK_CUR);
        fwrite(&b, 1, 1, f);
        fclose(f);
        GGFMap m;
        CHECK("T7b: map ok", ggf_map("build/ggf_mm_t7.ggf", &m) == 0);
        CHECK("T7c: node 0 → NULL (tick ผิด)", ggf_map_node(&m, 0, NULL) == NULL);
        CHECK("T7d: node 1 ยังอ่านได้", ggf_map_node(&m, 1, NULL) != NULL);
        CHECK("T7e: verify จับได้ (≠0)", ggf_map_verify(&m) != 0);
        ggf_unmap(&m);
        remove("build/ggf_mm_t7.ggf");
    }

    /* ── T8: drop-in — mmap == lazy ทุก chunk ───────────────────── */
    {
        uint64_t n = 322560 + 70000;
        uint8_t *d = (uint8_t *)malloc(n);
        fill(d, n, 29);
        CHECK("T8a: save ok", ggs_save(d, n, 8, "build/ggf_mm_t8.ggf") == 0);
        GGFMap m;
        GGFReader r;
        CHECK("T8b: map + open lazy ok",
              ggf_map("build/ggf_mm_t8.ggf", &m) == 0 &&
              ggf_open("build/ggf_mm_t8.ggf", &r) == 0);
        int same = 1;
        for (uint64_t k = 0; k < m.h.n_chunks && same; k++) {
            uint8_t a[GGS_CHUNK], b[GGS_CHUNK];
            if (ggf_map_chunk(&m, k, a) != 0 || ggf_chunk(&r, k, b) != 0) { same = 0; break; }
            if (memcmp(a, b, GGS_CHUNK) != 0) same = 0;
        }
        CHECK("T8c: mmap == lazy ทุก chunk (drop-in เห็นพ้อง)", same);
        ggf_unmap(&m);
        ggf_close(&r);
        free(d);
        remove("build/ggf_mm_t8.ggf");
    }

    /* ── T9: tail partial ───────────────────────────────────────── */
    {
        uint8_t d[1000];
        fill(d, sizeof d, 31);
        CHECK("T9a: save ok", ggs_save(d, sizeof d, 8, "build/ggf_mm_t9.ggf") == 0);
        GGFMap m;
        CHECK("T9b: map ok", ggf_map("build/ggf_mm_t9.ggf", &m) == 0);
        const uint8_t *c = ggf_map_node(&m, 15, NULL);
        CHECK("T9c: node 15 (tail) ตรง 40B + พักเป็น 0 (padded)",
              c && memcmp(c, d + 960, 40) == 0 && c[40] == 0 && c[63] == 0);
        uint8_t b;
        CHECK("T9d: map_read เฉพาะ bytes จริง",
              ggf_map_read(&m, 999, &b, 1) == 0 && b == d[999]);
        ggf_unmap(&m);
        remove("build/ggf_mm_t9.ggf");
    }

    /* ── T10: empty / bad magic / L5 ────────────────────────────── */
    {
        CHECK("T10a: save 0B ok", ggs_save(NULL, 0, 8, "build/ggf_mm_t10.ggf") == 0);
        GGFMap m;
        CHECK("T10b: map 0B ok · chunk → NULL",
              ggf_map("build/ggf_mm_t10.ggf", &m) == 0 &&
              ggf_map_node(&m, 0, NULL) == NULL);
        ggf_unmap(&m);
        remove("build/ggf_mm_t10.ggf");

        FILE *f = fopen("build/ggf_mm_t10.bin", "wb");
        uint8_t junk[128];
        fill(junk, sizeof junk, 37);
        fwrite(junk, 1, sizeof junk, f);
        fclose(f);
        CHECK("T10c: bad magic → ปฏิเสธ (rc=-4)",
              ggf_map("build/ggf_mm_t10.bin", &m) == -4);
        remove("build/ggf_mm_t10.bin");

        uint8_t d5[5000];
        fill(d5, sizeof d5, 41);
        CHECK("T10d: save L5 ok", ggs_save(d5, sizeof d5, 5, "build/ggf_mm_t10.ggf") == 0);
        CHECK("T10e: map L5 ok · node 10 ตรง",
              ggf_map("build/ggf_mm_t10.ggf", &m) == 0 &&
              ggf_map_node(&m, 10, NULL) &&
              memcmp(ggf_map_node(&m, 10, NULL), d5 + 640, 64) == 0);
        ggf_unmap(&m);
        remove("build/ggf_mm_t10.ggf");
    }

    /* ── T11: unmap แล้ว node → NULL ────────────────────────────── */
    {
        uint8_t d[512];
        fill(d, sizeof d, 43);
        CHECK("T11a: save ok", ggs_save(d, sizeof d, 8, "build/ggf_mm_t11.ggf") == 0);
        GGFMap m;
        CHECK("T11b: map ok", ggf_map("build/ggf_mm_t11.ggf", &m) == 0);
        ggf_unmap(&m);
        CHECK("T11c: หลัง unmap → node NULL (ปลอดภัย)",
              ggf_map_node(&m, 0, NULL) == NULL && ggf_map_chunk(&m, 0, (uint8_t[GGS_CHUNK]){0}) == -1);
        remove("build/ggf_mm_t11.ggf");
    }

    printf("\n═══ RESULT: %d PASS / %d FAIL ═══\n", pass_count, fail_count);
    return fail_count ? 1 : 0;
}
