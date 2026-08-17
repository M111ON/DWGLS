/*
 * test_goldberg_lazy.c — Lazy read .ggf (GGFReader)
 * ═══════════════════════════════════════════════════════════════════
 *
 * T1.2j — อ่าน .ggf แบบ seek ต่อ node ไม่โหลดทั้งไฟล์ (geo_goldberg_file.h)
 *
 * Proof:
 *   T1  open: header fields + sphere index ตรง (offset/ขนาด section)
 *   T2  random access 1 sphere: chunk k ตรงกับต้นฉบับทุก chunk ที่สุ่ม
 *   T3  random access ข้าม sphere (745KB = 3 spheres): chunk ใน sphere 0/1/2
 *   T4  lazy full read == ggs_load (สอง path ให้ผลเท่ากัน byte-for-byte)
 *   T5  ggf_read unaligned: byte range [off, off+n) ตรงต้นฉบับ (off เศษ)
 *   T6  ggf_verify = 0 ไฟล์ดี · flip data → ≠ 0 (lazy CRC detect)
 *   T7  flip tick → ggf_chunk rc=-9 (lazy per-node detect)
 *   T8  empty (0B): open ok · chunk -1 · verify 0
 *   T9  tail partial (1000B): chunk สุดท้าย padded + ggf_read เฉพาะ bytes จริง
 *   T10 bad magic → open ปฏิเสธ
 *   T11 level 5 lazy read
 *   T12 RAM คงที่: open ไฟล์ใหญ่ (หลาย MB) → ไม่ alloc buffer ตามขนาด data
 *       (ตรวจผ่าน heap usage โดยประมาณ: ใช้เฉพาะ index = n_spheres ตัวเล็ก)
 *
 * BUILD: gcc -O2 -Wall -Icore -Icore/infra -o build/test_goldberg_lazy \
 *        tests/test_goldberg_lazy.c
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

static uint64_t rng = 12345;
static uint64_t rnd(void)
{
    rng = rng * 6364136223846793005ULL + 1442695040888963407ULL;
    return rng >> 33;
}

/* เปิด lazy reader + ตรวจ index ตรงกับไฟล์ */
static int open_ok(const char *path, GGFReader *r)
{
    int rc = ggf_open(path, r);
    if (rc != 0) return 0;
    /* sphere index: ผลรวม count = n_chunks */
    uint64_t total = 0;
    for (uint32_t s = 0; s < r->n_spheres; s++) total += r->sphere_cnt[s];
    return total == r->h.n_chunks;
}

int main(void)
{
    printf("═══ test_goldberg_lazy — Lazy read .ggf (seek ต่อ node) ═══\n\n");

    /* ── T1: open + index ───────────────────────────────────────── */ 
    {
        uint8_t d[3000];                       /* 47 chunks → 1 sphere */
        fill(d, sizeof d, 3);
        CHECK("T1a: save 3000B ok", ggs_save(d, sizeof d, 8, "build/ggf_lz_t1.ggf") == 0);
        GGFReader r;
        CHECK("T1b: ggf_open ok", open_ok("build/ggf_lz_t1.ggf", &r));
        if (pass_count) { /* only if open succeeded — guard struct use */
            CHECK("T1c: n_chunks = 47", r.h.n_chunks == 47);
            CHECK("T1d: n_spheres = 1", r.n_spheres == 1);
            CHECK("T1e: sphere 0 count = 47", r.sphere_cnt[0] == 47);
            CHECK("T1f: sphere_off = 64 (หลัง header)", r.sphere_off[0] == 64);
        }
        ggf_close(&r);
        remove("build/ggf_lz_t1.ggf");
    }

    /* ── T2: random access 1 sphere ─────────────────────────────── */
    {
        uint64_t n = 12345;                    /* 193 chunks */
        uint8_t *d = (uint8_t *)malloc(n);
        fill(d, n, 7);
        CHECK("T2a: save ok", ggs_save(d, n, 8, "build/ggf_lz_t2.ggf") == 0);
        GGFReader r;
        CHECK("T2b: open ok", open_ok("build/ggf_lz_t2.ggf", &r));
        int all = 1;
        for (int t = 0; t < 50; t++) {
            uint64_t k = rnd() % r.h.n_chunks;
            uint8_t c[GGS_CHUNK];
            if (ggf_chunk(&r, k, c) != 0) { all = 0; break; }
            uint64_t base = k * GGS_CHUNK;
            uint32_t cmp_n = (n - base >= GGS_CHUNK) ? GGS_CHUNK : (uint32_t)(n - base);
            if (memcmp(c, d + base, cmp_n) != 0) { all = 0; break; }
        }
        CHECK("T2c: 50 random chunks ตรงต้นฉบับ", all);
        CHECK("T2d: chunk เกิน → -1", ggf_chunk(&r, r.h.n_chunks, (uint8_t[GGS_CHUNK]){0}) == -1);
        ggf_close(&r);
        free(d);
        remove("build/ggf_lz_t2.ggf");
    }

    /* ── T3: random access ข้าม sphere ───────────────────────────── */
    {
        uint64_t n = 322560 * 2 + 100000;      /* 745KB = 3 spheres */
        uint8_t *d = (uint8_t *)malloc(n);
        fill(d, n, 11);
        CHECK("T3a: save 745KB ok", ggs_save(d, n, 8, "build/ggf_lz_t3.ggf") == 0);
        GGFReader r;
        CHECK("T3b: open ok", open_ok("build/ggf_lz_t3.ggf", &r));
        CHECK("T3c: n_spheres = 3", r.n_spheres == 3);
        /* ก่อนอื่นหา chunk ขอบ sphere (first/last ของ sphere 1) */
        int ok = 1;
        uint64_t p = r.per_sphere;
        uint64_t edge[] = { 0, p - 1, p, p + 1, 2 * p - 1, 2 * p, r.h.n_chunks - 1 };
        for (int e = 0; e < 7; e++) {
            uint64_t k = edge[e];
            uint8_t c[GGS_CHUNK];
            if (ggf_chunk(&r, k, c) != 0) { ok = 0; break; }
            uint32_t cmp_n = (n - k * GGS_CHUNK >= GGS_CHUNK)
                             ? GGS_CHUNK : (uint32_t)(n - k * GGS_CHUNK);
            if (memcmp(c, d + k * GGS_CHUNK, cmp_n) != 0) { ok = 0; break; }
        }
        for (int t = 0; t < 100 && ok; t++) {
            uint64_t k = rnd() % r.h.n_chunks;
            uint8_t c[GGS_CHUNK];
            if (ggf_chunk(&r, k, c) != 0) { ok = 0; break; }
            uint32_t cmp_n = (n - k * GGS_CHUNK >= GGS_CHUNK)
                             ? GGS_CHUNK : (uint32_t)(n - k * GGS_CHUNK);
            if (memcmp(c, d + k * GGS_CHUNK, cmp_n) != 0) { ok = 0; break; }
        }
        CHECK("T3d: edge + 100 random chunks ข้าม sphere ตรงต้นฉบับ", ok);
        ggf_close(&r);
        free(d);
        remove("build/ggf_lz_t3.ggf");
    }

    /* ── T4: lazy full read == ggs_load ─────────────────────────── */
    {
        uint64_t n = 322560 + 70000;           /* 1 sphere + เศษ */
        uint8_t *d = (uint8_t *)malloc(n);
        fill(d, n, 13);
        CHECK("T4a: save ok", ggs_save(d, n, 8, "build/ggf_lz_t4.ggf") == 0);
        /* full path */
        uint64_t n_chunks = (n + 63) / 64;
        uint8_t *full = (uint8_t *)malloc(n_chunks * GGS_CHUNK);
        uint64_t got = 0;
        CHECK("T4b: ggs_load ok", ggs_load("build/ggf_lz_t4.ggf", full, n_chunks * GGS_CHUNK, &got) == 0);
        /* lazy path — อ่านทุก chunk ตามลำดับ */
        GGFReader r;
        CHECK("T4c: open ok", open_ok("build/ggf_lz_t4.ggf", &r));
        uint8_t *lazy = (uint8_t *)malloc(n_chunks * GGS_CHUNK);
        int all = 1;
        for (uint64_t k = 0; k < r.h.n_chunks && all; k++)
            if (ggf_chunk(&r, k, lazy + k * GGS_CHUNK) != 0) all = 0;
        CHECK("T4d: lazy อ่านครบทุก chunk", all);
        CHECK("T4e: lazy buffer == ggs_load buffer byte-for-byte",
              all && memcmp(lazy, full, n_chunks * GGS_CHUNK) == 0);
        CHECK("T4f: lazy decode == ต้นฉบับ (n bytes)", memcmp(lazy, d, n) == 0);
        ggf_close(&r);
        free(lazy); free(full); free(d);
        remove("build/ggf_lz_t4.ggf");
    }

    /* ── T5: ggf_read unaligned ─────────────────────────────────── */
    {
        uint64_t n = 10000;                    /* 157 chunks */
        uint8_t *d = (uint8_t *)malloc(n);
        fill(d, n, 17);
        CHECK("T5a: save ok", ggs_save(d, n, 8, "build/ggf_lz_t5.ggf") == 0);
        GGFReader r;
        CHECK("T5b: open ok", open_ok("build/ggf_lz_t5.ggf", &r));
        uint64_t offs[] = { 0, 1, 63, 64, 100, 127, 128, 5000, 9998, n - 1 };
        int ok = 1;
        for (int t = 0; t < 10 && ok; t++) {
            uint64_t off = offs[t];
            uint64_t len = (t % 2) ? 7 : 129;  /* เศษทั้งสองแบบ */
            if (off + len > n) len = n - off;
            uint8_t got2[256], exp2[256];
            if (ggf_read(&r, off, got2, len) != 0) { ok = 0; break; }
            memcpy(exp2, d + off, len);
            if (memcmp(got2, exp2, len) != 0) { ok = 0; break; }
        }
        CHECK("T5c: 10 unaligned ranges ตรงต้นฉบับ", ok);
        uint8_t tmp;
        CHECK("T5d: อ่านเกิน n_bytes → -1", ggf_read(&r, n, &tmp, 1) == -1);
        ggf_close(&r);
        free(d);
        remove("build/ggf_lz_t5.ggf");
    }

    /* ── T6: lazy CRC verify ────────────────────────────────────── */
    {
        uint8_t d[512];
        fill(d, sizeof d, 19);
        CHECK("T6a: save ok", ggs_save(d, sizeof d, 8, "build/ggf_lz_t6.ggf") == 0);
        GGFReader r;
        CHECK("T6b: open ok", open_ok("build/ggf_lz_t6.ggf", &r));
        CHECK("T6c: ggf_verify ไฟล์ดี = 0", ggf_verify(&r) == 0);
        ggf_close(&r);
        /* flip data 1 byte */
        FILE *f = fopen("build/ggf_lz_t6.ggf", "r+b");
        fseek(f, 64 + 4 + 4 + 40, SEEK_SET);
        uint8_t b;
        fread(&b, 1, 1, f);
        b ^= 0xFF;
        fseek(f, -1, SEEK_CUR);
        fwrite(&b, 1, 1, f);
        fclose(f);
        CHECK("T6d: open หลัง corrupt ok", open_ok("build/ggf_lz_t6.ggf", &r));
        CHECK("T6e: ggf_verify จับ corrupt (≠0)", ggf_verify(&r) != 0);
        ggf_close(&r);
        remove("build/ggf_lz_t6.ggf");
    }

    /* ── T7: tick flip → per-node detect ────────────────────────── */
    {
        uint8_t d[256];                        /* 4 chunks */
        fill(d, sizeof d, 23);
        CHECK("T7a: save ok", ggs_save(d, sizeof d, 8, "build/ggf_lz_t7.ggf") == 0);
        FILE *f = fopen("build/ggf_lz_t7.ggf", "r+b");
        fseek(f, 64 + 4 + 0, SEEK_SET);        /* tick node 0 */
        uint8_t b;
        fread(&b, 1, 1, f);
        b ^= 0x01;
        fseek(f, -1, SEEK_CUR);
        fwrite(&b, 1, 1, f);
        fclose(f);
        GGFReader r;
        CHECK("T7b: open ok", open_ok("build/ggf_lz_t7.ggf", &r));
        uint8_t c[GGS_CHUNK];
        CHECK("T7c: node 0 → rc=-9 (tick ผิด)", ggf_chunk(&r, 0, c) == -9);
        CHECK("T7d: node 1 ยังอ่านได้ (rc=0)", ggf_chunk(&r, 1, c) == 0);
        CHECK("T7e: ggf_verify จับได้ (≠0)", ggf_verify(&r) != 0);
        ggf_close(&r);
        remove("build/ggf_lz_t7.ggf");
    }

    /* ── T8: empty ──────────────────────────────────────────────── */
    {
        CHECK("T8a: save 0B ok", ggs_save(NULL, 0, 8, "build/ggf_lz_t8.ggf") == 0);
        GGFReader r;
        CHECK("T8b: open 0B ok", ggf_open("build/ggf_lz_t8.ggf", &r) == 0);
        uint8_t c[GGS_CHUNK];
        CHECK("T8c: chunk → -1 (ไม่มี data)", ggf_chunk(&r, 0, c) == -1);
        CHECK("T8d: ggf_verify 0B = 0", ggf_verify(&r) == 0);
        ggf_close(&r);
        remove("build/ggf_lz_t8.ggf");
    }

    /* ── T9: tail partial (1000B → chunk สุดท้าย 40B padded) ────── */
    {
        uint8_t d[1000];
        fill(d, sizeof d, 29);
        CHECK("T9a: save ok", ggs_save(d, sizeof d, 8, "build/ggf_lz_t9.ggf") == 0);
        GGFReader r;
        CHECK("T9b: open ok", open_ok("build/ggf_lz_t9.ggf", &r));
        uint8_t c[GGS_CHUNK];
        CHECK("T9c: chunk สุดท้าย (k=15) ok", ggf_chunk(&r, 15, c) == 0);
        CHECK("T9d: tail 40B ตรง + ที่เหลือเป็น 0 (padded)",
              memcmp(c, d + 960, 40) == 0 &&
              c[40] == 0 && c[63] == 0);
        uint8_t b;
        CHECK("T9e: ggf_read เฉพาะ bytes จริง (9999..999)", 
              ggf_read(&r, 999, &b, 1) == 0 && b == d[999]);
        ggf_close(&r);
        remove("build/ggf_lz_t9.ggf");
    }

    /* ── T10: bad magic ─────────────────────────────────────────── */
    {
        FILE *f = fopen("build/ggf_lz_t10.bin", "wb");
        uint8_t junk[128];
        fill(junk, sizeof junk, 31);
        fwrite(junk, 1, sizeof junk, f);
        fclose(f);
        GGFReader r;
        CHECK("T10: ไฟล์ไม่ใช่ .ggf → ปฏิเสธ (rc=-4)", ggf_open("build/ggf_lz_t10.bin", &r) == -4);
        remove("build/ggf_lz_t10.bin");
    }

    /* ── T11: level 5 ───────────────────────────────────────────── */
    {
        uint8_t d[5000];
        fill(d, sizeof d, 37);
        CHECK("T11a: save L5 ok", ggs_save(d, sizeof d, 5, "build/ggf_lz_t11.ggf") == 0);
        GGFReader r;
        CHECK("T11b: open L5 ok", open_ok("build/ggf_lz_t11.ggf", &r));
        uint8_t c[GGS_CHUNK];
        CHECK("T11c: chunk ตรงต้นฉบับ", ggf_chunk(&r, 10, c) == 0 &&
              memcmp(c, d + 640, 64) == 0);
        ggf_close(&r);
        remove("build/ggf_lz_t11.ggf");
    }

    /* ── T12: lazy ≠ โหลดทั้งไฟล์ — open ไฟล์ใหญ่ไม่ alloc data buffer ── */
    {
        /* 4MB → 65536 chunks → 1+ spheres; lazy alloc = index เท่านั้น
         * (sphere_off + sphere_cnt ≈ 12B × n_spheres) */
        uint64_t n = 4 * 1024 * 1024 + 13;
        uint8_t *d = (uint8_t *)malloc(n);
        fill(d, n, 41);
        CHECK("T12a: save 4MB ok", ggs_save(d, n, 8, "build/ggf_lz_t12.ggf") == 0);
        GGFReader r;
        CHECK("T12b: open 4MB ok", open_ok("build/ggf_lz_t12.ggf", &r));
        CHECK("T12c: n_chunks = 65537", r.h.n_chunks == 65537);
        /* open ไม่ alloc buffer ขนาด data — index เล็ก (n_spheres ≤ 14) */
        CHECK("T12d: index เล็ก (n_spheres ≤ 15)",
              r.n_spheres > 0 && r.n_spheres <= 15);
        uint8_t c[GGS_CHUNK];
        CHECK("T12e: random chunk ในไฟล์ใหญ่ ok",
              ggf_chunk(&r, 65536, c) == 0 && memcmp(c, d + 65536 * 64, 13) == 0);
        ggf_close(&r);
        free(d);
        remove("build/ggf_lz_t12.ggf");
    }

    printf("\n═══ RESULT: %d PASS / %d FAIL ═══\n", pass_count, fail_count);
    return fail_count ? 1 : 0;
}
