/*
 * test_goldberg_file.c — Goldberg sphere persistence (.ggf file)
 * ═══════════════════════════════════════════════════════════════════
 *
 * T1.2i — serialize sphere ลงไฟล์จริง (geo_goldberg_file.h)
 *
 * Proof:
 *   T1  header: ขนาด 64B + magic/version/level ถูกต้อง
 *   T2  small roundtrip: 100B → save → load → byte-for-byte
 *   T3  chunk-boundary: 64/128/640B → save → load → lossless
 *   T4  tail partial: 1000B (chunk สุดท้าย 40B, padded 64) → lossless
 *   T5  multi-sphere: 745KB = 3 spheres → save → load → lossless
 *   T6  CRC detect: flip 1 byte ใน data → ggs_load ปฏิเสธ (corrupt)
 *   T7  tick detect: flip 1 byte ใน tick → ggs_load ปฏิเสธ
 *   T8  deterministic: save 2 รอบ → ไฟล์ byte-for-byte เท่ากัน
 *   T9  bad magic: ไฟล์ที่ไม่ใช่ .ggf → ggs_load ปฏิเสธ
 *   T10 empty (0B) → save/load ผ่าน, n_bytes = 0
 *   T11 level 5 (GP(5,0)=252 faces) roundtrip
 *   T12 file size formula: 64 + Σ (4 + count×68) ตรงกับไฟล์จริง
 *
 * BUILD: gcc -O2 -Wall -Icore -Icore/infra -o build/test_goldberg_file \
 *        tests/test_goldberg_file.c
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

/* save → load → memcmp กับต้นฉบับ; คืน 1 ถ้า lossless */
static int roundtrip(const uint8_t *data, uint64_t n, uint8_t level,
                     const char *path, uint64_t *out_n_bytes)
{
    if (ggs_save(data, n, level, path) != 0) return 0;
    uint64_t n_chunks = (n + 63) / 64;
    uint8_t *buf = (uint8_t *)malloc(n_chunks * 64);
    uint64_t got = 0;
    int rc = ggs_load(path, buf, n_chunks * 64, &got);
    if (rc != 0) { free(buf); return 0; }
    if (got != n) { free(buf); return 0; }
    if (memcmp(buf, data, n) != 0) { free(buf); return 0; }
    if (out_n_bytes) *out_n_bytes = got;
    free(buf);
    return 1;
}

static long file_size(const char *path)
{
    FILE *f = fopen(path, "rb");
    if (!f) return -1;
    fseeko(f, 0, SEEK_END);
    long sz = ftello(f);
    fclose(f);
    return sz;
}

int main(void)
{
    printf("═══ test_goldberg_file — Goldberg sphere persistence (.ggf) ═══\n\n");

    /* ── T1: header layout ──────────────────────────────────────── */
    {
        CHECK("T1a: GGFHeader = 64B", sizeof(GGFHeader) == 64);
        uint8_t d[64];
        fill(d, sizeof d, 5);
        CHECK("T1b: save 64B ok", ggs_save(d, 64, 8, "build/ggf_t1.ggf") == 0);
        FILE *f = fopen("build/ggf_t1.ggf", "rb");
        GGFHeader h;
        fread(&h, sizeof h, 1, f);
        fclose(f);
        CHECK("T1c: magic = GGF0", memcmp(h.magic, "GGF0", 4) == 0);
        CHECK("T1d: version = 1", h.version == 1);
        CHECK("T1e: level = 8", h.level == 8);
        CHECK("T1f: n_chunks = 1, n_bytes = 64", h.n_chunks == 1 && h.n_bytes == 64);
        CHECK("T1g: crc32 ≠ 0 (มี checksum)", h.crc32 != 0);
        remove("build/ggf_t1.ggf");
    }

    /* ── T2: small roundtrip ────────────────────────────────────── */
    {
        uint8_t d[100];
        fill(d, sizeof d, 7);
        CHECK("T2: 100B roundtrip lossless",
              roundtrip(d, 100, 8, "build/ggf_t2.ggf", NULL));
        remove("build/ggf_t2.ggf");
    }

    /* ── T3: chunk boundary ─────────────────────────────────────── */
    {
        uint64_t sizes[] = { 64, 128, 640 };
        int ok = 1;
        for (int i = 0; i < 3; i++) {
            uint8_t *d = (uint8_t *)malloc(sizes[i]);
            fill(d, sizes[i], 11 + (uint32_t)i);
            char p[64];
            sprintf(p, "build/ggf_t3_%d.ggf", i);
            if (!roundtrip(d, sizes[i], 8, p, NULL)) ok = 0;
            remove(p);
            free(d);
        }
        CHECK("T3: 64/128/640B roundtrip lossless", ok);
    }

    /* ── T4: tail partial ───────────────────────────────────────── */
    {
        uint8_t d[1000];
        fill(d, sizeof d, 23);
        uint64_t got = 0;
        CHECK("T4: 1000B (tail 40B) roundtrip lossless",
              roundtrip(d, 1000, 8, "build/ggf_t4.ggf", &got) && got == 1000);
        remove("build/ggf_t4.ggf");
    }

    /* ── T5: multi-sphere (745KB = 3 spheres) ───────────────────── */
    {
        uint64_t n = 322560 * 2 + 100000;
        uint8_t *d = (uint8_t *)malloc(n);
        fill(d, n, 31);
        CHECK("T5: 745KB (3 spheres) roundtrip lossless",
              roundtrip(d, n, 8, "build/ggf_t5.ggf", NULL));
        free(d);
        remove("build/ggf_t5.ggf");
    }

    /* ── T6: CRC detect — data พัง 1 byte ───────────────────────── */
    {
        uint8_t d[512];
        fill(d, sizeof d, 41);
        CHECK("T6a: save 512B ok", ggs_save(d, 512, 8, "build/ggf_t6.ggf") == 0);
        FILE *f = fopen("build/ggf_t6.ggf", "r+b");
        fseek(f, 64 + 4 + 4 + 40, SEEK_SET);   /* กลาง data ของ node แรก */
        uint8_t b;
        fread(&b, 1, 1, f);
        b ^= 0xFF;
        fseek(f, -1, SEEK_CUR);
        fwrite(&b, 1, 1, f);
        fclose(f);
        uint8_t buf[512 + 64];
        uint64_t got = 0;
        int rc = ggs_load("build/ggf_t6.ggf", buf, sizeof buf, &got);
        CHECK("T6b: data corrupt → ggs_load ปฏิเสธ (CRC)", rc == -10);
        remove("build/ggf_t6.ggf");
    }

    /* ── T7: tick detect — tick พัง 1 byte ──────────────────────── */
    {
        uint8_t d[256];
        fill(d, sizeof d, 43);
        CHECK("T7a: save 256B ok", ggs_save(d, 256, 8, "build/ggf_t7.ggf") == 0);
        FILE *f = fopen("build/ggf_t7.ggf", "r+b");
        fseek(f, 64 + 4 + 0, SEEK_SET);        /* tick ของ node แรก */
        uint8_t b;
        fread(&b, 1, 1, f);
        b ^= 0x01;
        fseek(f, -1, SEEK_CUR);
        fwrite(&b, 1, 1, f);
        fclose(f);
        uint8_t buf[256 + 64];
        uint64_t got = 0;
        int rc = ggs_load("build/ggf_t7.ggf", buf, sizeof buf, &got);
        CHECK("T7b: tick corrupt → ggs_load ปฏิเสธ", rc < 0 && rc != -3 && rc != -2);
        remove("build/ggf_t7.ggf");
    }

    /* ── T8: deterministic — save 2 รอบ → ไฟล์เท่ากัน ───────────── */
    {
        uint8_t d[4096];
        fill(d, sizeof d, 47);
        CHECK("T8a: save #1 ok", ggs_save(d, sizeof d, 8, "build/ggf_t8a.ggf") == 0);
        CHECK("T8b: save #2 ok", ggs_save(d, sizeof d, 8, "build/ggf_t8b.ggf") == 0);
        FILE *a = fopen("build/ggf_t8a.ggf", "rb");
        FILE *b = fopen("build/ggf_t8b.ggf", "rb");
        int same = 1;
        int ca, cb;
        do {
            ca = fgetc(a); cb = fgetc(b);
            if (ca != cb) same = 0;
        } while (ca != EOF && cb != EOF);
        if (ca != EOF || cb != EOF) same = 0;
        fclose(a); fclose(b);
        CHECK("T8c: ไฟล์ byte-for-byte เท่ากัน (deterministic)", same);
        remove("build/ggf_t8a.ggf");
        remove("build/ggf_t8b.ggf");
    }

    /* ── T9: bad magic ──────────────────────────────────────────── */
    {
        FILE *f = fopen("build/ggf_t9.bin", "wb");
        uint8_t junk[128];
        fill(junk, sizeof junk, 53);
        fwrite(junk, 1, sizeof junk, f);
        fclose(f);
        uint8_t buf[128];
        uint64_t got = 0;
        int rc = ggs_load("build/ggf_t9.bin", buf, sizeof buf, &got);
        CHECK("T9: ไฟล์ไม่ใช่ .ggf → ปฏิเสธ (bad magic)", rc == -4);
        remove("build/ggf_t9.bin");
    }

    /* ── T10: empty ─────────────────────────────────────────────── */
    {
        CHECK("T10a: save 0B ok", ggs_save(NULL, 0, 8, "build/ggf_t10.ggf") == 0);
        uint8_t buf[64];
        uint64_t got = 99;
        int rc = ggs_load("build/ggf_t10.ggf", buf, sizeof buf, &got);
        CHECK("T10b: load 0B → ok, n_bytes = 0", rc == 0 && got == 0);
        remove("build/ggf_t10.ggf");
    }

    /* ── T11: level 5 ───────────────────────────────────────────── */
    {
        uint8_t d[5000];
        fill(d, sizeof d, 59);
        CHECK("T11: L5 (GP(5,0)=252) roundtrip lossless",
              roundtrip(d, sizeof d, 5, "build/ggf_t11.ggf", NULL));
        remove("build/ggf_t11.ggf");
    }

    /* ── T12: file size formula ─────────────────────────────────── */
    {
        uint8_t d[1000];
        fill(d, sizeof d, 61);
        CHECK("T12a: save 1000B ok", ggs_save(d, 1000, 8, "build/ggf_t12.ggf") == 0);
        /* 16 chunks → 1 sphere → 64 + 4 + 16×68 = 1156 */
        long sz = file_size("build/ggf_t12.ggf");
        CHECK("T12b: size = 64 + 4 + 16×68 = 1156", sz == 1156);
        remove("build/ggf_t12.ggf");

        /* 745KB → 3 spheres: chunk count = 11643 → 5040/5040/1563 */
        uint64_t n = 322560 * 2 + 100000;
        uint8_t *big = (uint8_t *)malloc(n);
        fill(big, n, 67);
        CHECK("T12c: save 745KB ok", ggs_save(big, n, 8, "build/ggf_t12b.ggf") == 0);
        uint64_t c0 = 5040, c1 = 5040, c2 = 11643 - 10080;
        long expect = 64 + 3 * 4 + (c0 + c1 + c2) * 68;
        sz = file_size("build/ggf_t12b.ggf");
        CHECK("T12d: 3-sphere size formula ตรง", sz == expect);
        free(big);
        remove("build/ggf_t12b.ggf");
    }

    printf("\n═══ RESULT: %d PASS / %d FAIL ═══\n", pass_count, fail_count);
    return fail_count ? 1 : 0;
}
