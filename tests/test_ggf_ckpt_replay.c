/*
 * test_ggf_ckpt_replay.c — checkpoint/replay ของ .ggf storage
 * ═══════════════════════════════════════════════════════════════════════
 *
 * T1.2n — save .ggf (home, dedup ระดับไฟล์) + manifest → restore ใน
 * structures ใหม่ทั้งหมดผ่าน walk clock (เหมือน fibo_checkpoint_sweep:
 * เก็บแค่ seed+method — regenerate ได้) (core/geo_ggf_ckpt.h)
 *
 * Proof:
 *   T1  manifest roundtrip: write → read → ทุก field ตรง (seed/ticks/cycles/
 *       n/sizes/home_of/dup_bytes)
 *   T2  checkpoint: save เฉพาะ home ผ่าน ggf_save_map (8 ไฟล์) + manifest
 *   T3  replay ใน structures ใหม่ (state มาจาก manifest ล้วน) → lossless 11/11
 *   T4  dup: replay อ่าน dup ผ่าน home map — bytes ตรงทั้ง dup และ home
 *   T5  corrupt manifest (magic) → ปฏิเสธ
 *   T6  corrupt .ggf (tick พัง) → replay จับ fail (ผ่าน zero-copy path)
 *   T7  home ไฟล์หาย → replay จับ fail (map ล้ม)
 *   T8  manifest n=0 → ปฏิเสธ
 *   T9  deterministic: replay 2 รอบ → ผลเท่ากัน (bytes/ok เหมือนเดิม)
 *   T10 ว่าง (home=-1) → ข้าม (ไม่นับ fail)
 *
 * BUILD: gcc -O2 -Wall -Icore -Icore/infra -o build/test_ggf_ckpt_replay \
 *        tests/test_ggf_ckpt_replay.c
 */

#include <stdio.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include "../core/geo_ggf_ckpt.h"
#include "../core/tied_dedup.h"

/* mkdir recursive (Windows-safe — system() ใช้ cmd.exe ไม่รู้จัก mkdir -p) */
static void ensure_dir(const char *dir)
{
    char tmp[512];
    snprintf(tmp, sizeof(tmp), "%s", dir);
    for (char *s = tmp + 1; *s; s++) {
        if (*s == '/' || *s == '\\') { *s = '\0'; mkdir(tmp); *s = '/'; }
    }
    mkdir(tmp);
}

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
static const char *g_names[NT] = {
    "tok.embd.weight", "blk.0.attn.q", "blk.0.norm", "out.weight",
    "tok.embd.weight", "blk.1.ffn.down", "blk.0.attn.q", "big.tensor",
    "blk.0.norm", "empty.zero", "mix.3000", "tiny.77"
};

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
    }
}

/* cmp: เทียบกับต้นฉบับ (ใน test — data ยังอยู่ใน memory เพื่อเทียบ) */
static int cmp_orig(void *ctx, uint32_t idx, const uint8_t *got, uint64_t got_n)
{
    (void)ctx;
    return got_n == g_sizes[idx] && memcmp(got, g_data[idx], got_n) == 0;
}


static const char **g_paths;
static char g_path_buf[NT][256];

/* path ของ delta dir (สร้างใหม่ทุกครั้ง — deterministic เหมือน build_paths) */
static const char *g_paths_db_delta(uint32_t idx)
{
    static char dbuf[NT][256];
    ggf_ckpt_path(dbuf[idx], sizeof dbuf[idx], "build/ckpt_db/delta", idx,
                  g_names[idx]);
    return dbuf[idx];
}

static void build_paths(const char *dir)
{
    for (uint32_t i = 0; i < NT; i++) {
        ggf_ckpt_path(g_path_buf[i], sizeof g_path_buf[i], dir, i, g_names[i]);
        g_paths[i] = g_path_buf[i];
    }
}

int main(void)
{
    printf("═══ test_ggf_ckpt_replay — checkpoint .ggf → replay ผ่าน walk clock ═══\n\n");
    ensure_dir("build/ckpt_t");
    ensure_dir("build/ckpt_t_mid");
    ensure_dir("build/ckpt_db/base");
    ensure_dir("build/ckpt_db/delta");
    make_tensors();
    g_paths = (const char **)malloc(NT * sizeof(char *));
    build_paths("build/ckpt_t");

    /* ── T1: manifest roundtrip ─────────────────────────────────── */
    {
        uint32_t sizes[NT], rq_a[NT], rq_b[NT];
        for (uint32_t i = 0; i < NT; i++) sizes[i] = g_sizes[i];
        CHECK("T1a: manifest write ok",
              ggf_ckpt_write("build/ckpt_t/manifest.mfp", 42u, 12u, 144u, NT,
                             g_names, sizes, g_home, NULL, NULL,
                             2164u, 100000u, 0u, 0u, "tester v1", "test-model.gguf") == 0);
        GgfCkptHeader h;
        GgfCkptEntry *e = NULL;
        CHECK("T1b: manifest read ok (crc64 ผ่าน)",
              ggf_ckpt_read("build/ckpt_t/manifest.mfp", &h, &e) == 0);
        CHECK("T1c: fields ตรง (seed/ticks/cycles/n/dup/data)",
              h.seed == 42 && h.ticks == 12 && h.cycles == 144 && h.n == NT &&
              h.dup_bytes == 2164 && h.data_bytes == 100000);
        CHECK("T1c2: provenance ตรง (ใคร/โมเดลไหน/เมื่อไหร่)",
              strcmp(h.note, "tester v1") == 0 &&
              strcmp(h.model, "test-model.gguf") == 0 &&
              h.created_utc > 0);
        int same = 1;
        for (uint32_t i = 0; i < NT; i++)
            if (e[i].size != g_sizes[i] || strcmp(e[i].name, g_names[i]) != 0)
                same = 0;
        CHECK("T1d: entries ตรง (size/name ทุกตัว)", same);
        /* rq คำนวณจาก seed — manifest ไม่เก็บ → คำนวณใหม่ได้เท่าเดิม */
        for (uint32_t i = 0; i < NT; i++) {
            rq_a[i] = ggf_walk_rq_of(42, i, 144);
            rq_b[i] = ggf_walk_rq_of(42, i, 144);
        }
        CHECK("T1e: rq regenerate จาก seed (ไม่ต้องเก็บใน manifest)",
              memcmp(rq_a, rq_b, sizeof rq_a) == 0);
        free(e);
    }

    /* ── T2: checkpoint — save home + manifest จริง ─────────────── */
    {
        tied_dedup_scan((const uint8_t *const *)g_data, g_sizes, NT, g_home);
        CHECK("T2a: registry 3 dups (t4→0 t6→1 t8→2) + 1 ว่าง",
              g_home[4] == 0 && g_home[6] == 1 && g_home[8] == 2 && g_home[9] == -1);
        uint32_t n_save = 0;
        for (uint32_t i = 0; i < NT; i++) {
            if (g_home[i] != (int32_t)i || g_sizes[i] == 0) continue;
            if (ggf_save_map(g_data[i], g_sizes[i], 8, g_paths[i]) == 0) n_save++;
        }
        CHECK("T2b: save เฉพาะ home ผ่าน ggf_save_map (8 ไฟล์)", n_save == 8);
        CHECK("T2c: manifest write ok",
              ggf_ckpt_write("build/ckpt_t/manifest.mfp", 42u, 12u, 144u, NT,
                             g_names, g_sizes, g_home, NULL, NULL,
                             2164u, 0u, 0u, 0u, "tester v1", "test-model.gguf") == 0);
    }

    /* ── T3+T4: replay ใน structures ใหม่ (state จาก manifest ล้วน) ── */
    {
        GgfCkptHeader h;
        GgfCkptEntry *e = NULL;
        CHECK("T3a: manifest read ok",
              ggf_ckpt_read("build/ckpt_t/manifest.mfp", &h, &e) == 0);
        uint64_t bytes = 0;
        uint32_t ok = 0, fail = 0, skip = 0;
        int rc = ggf_ckpt_replay(&h, e, "build/ckpt_t", "", cmp_orig, NULL,
                                 &bytes, &ok, &fail, &skip);
        CHECK("T3b: replay lossless 11/11 (fail 0, skip 0)",
              rc == 0 && ok == 11 && fail == 0 && skip == 0);
        uint64_t expect_bytes = 0;
        for (uint32_t i = 0; i < NT; i++)
            if (g_home[i] >= 0) expect_bytes += g_sizes[i];
        CHECK("T3c: bytes อ่าน = Σ tensor (11 ตัว)", bytes == expect_bytes);
        free(e);
    }

    /* ── T5: manifest corrupt → ปฏิเสธ ──────────────────────────── */
    {
        FILE *f = fopen("build/ckpt_t/bad.mfp", "wb");
        uint8_t junk[1024];          /* ใหญ่กว่า header (584B) → fread header ผ่าน */
        memset(junk, 0xAB, sizeof junk);
        fwrite(junk, 1, sizeof junk, f);
        fclose(f);
        GgfCkptHeader h;
        GgfCkptEntry *e = NULL;
        CHECK("T5: corrupt manifest → ปฏิเสธ (rc=-4)",
              ggf_ckpt_read("build/ckpt_t/bad.mfp", &h, &e) == -4);
        remove("build/ckpt_t/bad.mfp");
    }

    /* ── T6: corrupt .ggf (tick พัง) → replay จับ fail ──────────── */
    {
        /* corrupt home ของ tensor 3 (out.weight, 5000B) — tick node 0 */
        FILE *f = fopen(g_paths[3], "r+b");
        fseek(f, 64 + 4 + 0, SEEK_SET);
        uint8_t b;
        fread(&b, 1, 1, f);
        b ^= 0x01;
        fseek(f, -1, SEEK_CUR);
        fwrite(&b, 1, 1, f);
        fclose(f);

        GgfCkptHeader h;
        GgfCkptEntry *e = NULL;
        ggf_ckpt_read("build/ckpt_t/manifest.mfp", &h, &e);
        uint64_t bytes = 0;
        uint32_t ok = 0, fail = 0, skip = 0;
        int rc = ggf_ckpt_replay(&h, e, "build/ckpt_t", "", cmp_orig, NULL,
                                 &bytes, &ok, &fail, &skip);
        CHECK("T6: .ggf tick พัง → replay จับ fail (10/11 ok, fail=1)",
              rc != 0 && ok == 10 && fail == 1);
        free(e);
        /* restore */
        ggf_save_map(g_data[3], g_sizes[3], 8, g_paths[3]);
    }

    /* ── T7: home ไฟล์หาย → replay จับ fail ─────────────────────── */
    {
        remove(g_paths[5]);                       /* home ของ tensor 5 */
        GgfCkptHeader h;
        GgfCkptEntry *e = NULL;
        ggf_ckpt_read("build/ckpt_t/manifest.mfp", &h, &e);
        uint64_t bytes = 0;
        uint32_t ok = 0, fail = 0, skip = 0;
        int rc = ggf_ckpt_replay(&h, e, "build/ckpt_t", "", cmp_orig, NULL,
                                 &bytes, &ok, &fail, &skip);
        CHECK("T7: home ไฟล์หาย → replay จับ fail (map ล้ม)", rc != 0 && fail == 1);
        free(e);
        ggf_save_map(g_data[5], g_sizes[5], 8, g_paths[5]);
    }

    /* ── T5b-T5e: tamper — แก้ manifest ตรงไหนก็จับได้ (crc64) ───── */
    {
        /* helper: คัดลอก manifest แล้ว flip 1 byte → read ต้องปฏิเสธ */
        FILE *src = fopen("build/ckpt_t/manifest.mfp", "rb");
        FILE *dst = fopen("build/ckpt_t/tamper.mfp", "wb");
        int c;
        while ((c = fgetc(src)) != EOF) fputc(c, dst);
        fclose(src); fclose(dst);

        GgfCkptHeader h;
        GgfCkptEntry *e = NULL;
        /* (a) แก้ชื่อ tensor (entry 0) */
        FILE *f = fopen("build/ckpt_t/tamper.mfp", "r+b");
        fseek(f, (long)sizeof(GgfCkptHeader) + 5, SEEK_SET);
        uint8_t b;
        fread(&b, 1, 1, f); b ^= 0x01;
        fseek(f, -1, SEEK_CUR); fwrite(&b, 1, 1, f);
        fclose(f);
        CHECK("T5b: แก้ชื่อ tensor → ปฏิเสธ (rc=-7 crc64)",
              ggf_ckpt_read("build/ckpt_t/tamper.mfp", &h, &e) == -7);
        /* (b) แก้ size (entry 0) */
        f = fopen("build/ckpt_t/tamper.mfp", "r+b");
        fseek(f, (long)sizeof(GgfCkptHeader) + GGF_CKPT_NAME_LEN, SEEK_SET);
        fread(&b, 1, 1, f); b ^= 0x01;
        fseek(f, -1, SEEK_CUR); fwrite(&b, 1, 1, f);
        fclose(f);
        CHECK("T5c: แก้ size → ปฏิเสธ (rc=-7)",
              ggf_ckpt_read("build/ckpt_t/tamper.mfp", &h, &e) == -7);
        /* (c) แก้ home_of (entry 0) */
        f = fopen("build/ckpt_t/tamper.mfp", "r+b");
        fseek(f, (long)sizeof(GgfCkptHeader) + GGF_CKPT_NAME_LEN + 4, SEEK_SET);
        fread(&b, 1, 1, f); b ^= 0x01;
        fseek(f, -1, SEEK_CUR); fwrite(&b, 1, 1, f);
        fclose(f);
        CHECK("T5d: แก้ home_of → ปฏิเสธ (rc=-7)",
              ggf_ckpt_read("build/ckpt_t/tamper.mfp", &h, &e) == -7);
        /* (d) แก้ seed (header) */
        f = fopen("build/ckpt_t/tamper.mfp", "r+b");
        fseek(f, 8, SEEK_SET);                     /* seed field */
        fread(&b, 1, 1, f); b ^= 0x01;
        fseek(f, -1, SEEK_CUR); fwrite(&b, 1, 1, f);
        fclose(f);
        CHECK("T5e: แก้ seed → ปฏิเสธ (rc=-7)",
              ggf_ckpt_read("build/ckpt_t/tamper.mfp", &h, &e) == -7);
        /* (e) แก้ note (provenance — crc ครอบด้วย) */
        f = fopen("build/ckpt_t/tamper.mfp", "r+b");
        fseek(f, (long)offsetof(GgfCkptHeader, note) + 2, SEEK_SET);
        fread(&b, 1, 1, f); b ^= 0x01;
        fseek(f, -1, SEEK_CUR); fwrite(&b, 1, 1, f);
        fclose(f);
        CHECK("T5f: แก้ note (provenance) → ปฏิเสธ (rc=-7)",
              ggf_ckpt_read("build/ckpt_t/tamper.mfp", &h, &e) == -7);
        /* (f) แก้ model */
        f = fopen("build/ckpt_t/tamper.mfp", "r+b");
        fseek(f, (long)offsetof(GgfCkptHeader, model) + 5, SEEK_SET);
        fread(&b, 1, 1, f); b ^= 0x01;
        fseek(f, -1, SEEK_CUR); fwrite(&b, 1, 1, f);
        fclose(f);
        CHECK("T5g: แก้ model → ปฏิเสธ (rc=-7)",
              ggf_ckpt_read("build/ckpt_t/tamper.mfp", &h, &e) == -7);
        remove("build/ckpt_t/tamper.mfp");
    }

    /* ── T8: manifest n=0 → ปฏิเสธ ──────────────────────────────── */
    {
        GgfCkptHeader h0;
        memset(&h0, 0, sizeof h0);
        memcpy(h0.magic, GGF_CKPT_MAGIC, 4);
        h0.version = GGF_CKPT_VERSION;
        h0.n = 0;
        FILE *f = fopen("build/ckpt_t/zero.mfp", "wb");
        fwrite(&h0, sizeof h0, 1, f);
        fclose(f);
        GgfCkptHeader h;
        GgfCkptEntry *e = NULL;
        CHECK("T8: manifest n=0 → ปฏิเสธ (rc=-6)",
              ggf_ckpt_read("build/ckpt_t/zero.mfp", &h, &e) == -6);
        remove("build/ckpt_t/zero.mfp");
    }

    /* ── T9: deterministic — replay 2 รอบ → ผลเท่ากัน ───────────── */
    {
        GgfCkptHeader h;
        GgfCkptEntry *e = NULL;
        ggf_ckpt_read("build/ckpt_t/manifest.mfp", &h, &e);
        uint64_t b1 = 0, b2 = 0;
        uint32_t ok1 = 0, f1 = 0, s1 = 0, ok2 = 0, f2 = 0, s2 = 0;
        ggf_ckpt_replay(&h, e, "build/ckpt_t", "", cmp_orig, NULL, &b1, &ok1, &f1, &s1);
        ggf_ckpt_replay(&h, e, "build/ckpt_t", "", cmp_orig, NULL, &b2, &ok2, &f2, &s2);
        CHECK("T9: replay 2 รอบ → ผลเท่ากัน (deterministic, replay ได้)",
              ok1 == ok2 && f1 == f2 && b1 == b2 && f1 == 0);
        free(e);
    }

    /* ── T10: ว่าง ข้าม (ไม่นับ fail) ────────────────────────────── */
    {
        /* T3 แล้ว — 11/11 โดยนับเฉพาะ home ≥ 0 (t9 ว่าง ไม่นับ) */
        CHECK("T10: ว่าง (home=-1) ข้าม — ไม่นับใน fail (ดู T3b: ok=11 จาก 12)",
              g_home[9] == -1);
    }

    /* ══════════ VERIFY — สแกน .ggf+manifest โดยไม่มีโมเดลต้นทาง ══════════ */
    printf("\n— VERIFY (ggf_ckpt_verify — ไม่ต้องมีโมเดลต้นทาง) —\n");

    /* T11: verify ไฟล์ดี */
    {
        uint32_t files = 0, fail = 0;
        int rc = ggf_ckpt_verify("build/ckpt_t", &files, &fail);
        CHECK("T11: verify ผ่าน — 8 home .ggf CRC32 ผ่าน (fail 0)",
              rc == 0 && files == 8 && fail == 0);
    }

    /* T12: corrupt data → verify จับ */
    {
        FILE *f = fopen(g_paths[3], "r+b");     /* home ของ t3 (5000B) */
        fseek(f, 64 + 4 + 4 + 40, SEEK_SET);
        uint8_t b;
        fread(&b, 1, 1, f); b ^= 0xFF;
        fseek(f, -1, SEEK_CUR); fwrite(&b, 1, 1, f);
        fclose(f);
        uint32_t files = 0, fail = 0;
        int rc = ggf_ckpt_verify("build/ckpt_t", &files, &fail);
        CHECK("T12: data corrupt → verify จับ (fail=1, CRC32)",
              rc != 0 && fail == 1 && files == 7);
        ggf_save_map(g_data[3], g_sizes[3], 8, g_paths[3]);
    }

    /* T13: ไฟล์หาย → verify จับ */
    {
        remove(g_paths[5]);
        uint32_t files = 0, fail = 0;
        int rc = ggf_ckpt_verify("build/ckpt_t", &files, &fail);
        CHECK("T13: home ไฟล์หาย → verify จับ (fail=1)", rc != 0 && fail == 1);
        ggf_save_map(g_data[5], g_sizes[5], 8, g_paths[5]);
    }

    /* T14: size ผิด (n_bytes ถูกแก้ในไฟล์) → verify จับ */
    {
        FILE *f = fopen(g_paths[7], "r+b");     /* home ของ t7 (745KB) */
        fseek(f, 16, SEEK_SET);                  /* n_bytes LSB ใน GGFHeader */
        uint8_t b;
        fread(&b, 1, 1, f); b ^= 0x40;
        fseek(f, -1, SEEK_CUR); fwrite(&b, 1, 1, f);
        fclose(f);
        uint32_t files = 0, fail = 0;
        int rc = ggf_ckpt_verify("build/ckpt_t", &files, &fail);
        CHECK("T14: n_bytes ผิด (แก้ในไฟล์) → verify จับ (size ≠ manifest)",
              rc != 0 && fail == 1);
        ggf_save_map(g_data[7], g_sizes[7], 8, g_paths[7]);
    }

    /* T15: manifest ถูกแก้ → verify ปฏิเสธ (crc64) */
    {
        FILE *f = fopen("build/ckpt_t/manifest.mfp", "r+b");
        fseek(f, 8, SEEK_SET);                   /* seed field */
        uint8_t b;
        fread(&b, 1, 1, f); b ^= 0x01;
        fseek(f, -1, SEEK_CUR); fwrite(&b, 1, 1, f);
        fclose(f);
        uint32_t files = 0, fail = 0;
        int rc = ggf_ckpt_verify("build/ckpt_t", &files, &fail);
        CHECK("T15: manifest ถูกแก้ → verify ปฏิเสธ (crc64)", rc != 0);
        /* restore manifest จริง */
        ggf_ckpt_write("build/ckpt_t/manifest.mfp", 42u, 12u, 144u, NT,
                       g_names, g_sizes, g_home, NULL, NULL,
                       2164u, 0u, 0u, 0u, "tester v1", "test-model.gguf");
    }

    /* ══════════ MID-ROUND — checkpoint กลางรอบ ══════════ */
    printf("\n— MID-ROUND checkpoint (replay เริ่มจาก round กลาง) —\n");
    {
        /* คำนวณ expected จาก rq (deterministic — เหมือน replay) */
        uint32_t ckpt_r = 72, ckpt_t = 0;
        uint64_t ckpt_pos = (uint64_t)ckpt_r * 12 + ckpt_t;
        uint32_t exp_pending = 0, exp_skip = 0, exp_t0 = 0, exp_t4 = 0;
        for (uint32_t i = 0; i < NT; i++) {
            if (g_home[i] < 0) continue;
            uint32_t rq = ggf_walk_rq_of(42, i, 144);
            uint64_t pos = (uint64_t)rq * 12 + (rq % 12);
            if (pos < ckpt_pos) {
                exp_skip++;
                if (i == 0) exp_t0 = 1;
            } else {
                exp_pending++;
                if (i == 4) exp_t4 = 1;          /* dup ของ t0 — state ของตัวเอง */
            }
        }
        CHECK("T16a: มีทั้ง pending และ skip (กลางรอบตัดจริง)",
              exp_pending > 0 && exp_skip > 0);
        CHECK("T16b: home (t0) อยู่ก่อน checkpoint → skip แต่ dup (t4) อยู่หลัง → pending",
              exp_t0 == 1 && exp_t4 == 1);

        char mid[1024];
        snprintf(mid, sizeof mid, "build/ckpt_t_mid/mid.mfp");
        CHECK("T16c: write manifest กลางรอบ (round=72 tick=0)",
              ggf_ckpt_write(mid, 42u, 12u, 144u, NT, g_names, g_sizes, g_home,
                             NULL, NULL, 2164u, 0u, ckpt_r, ckpt_t,
                             "tester v1", "mid.gguf") == 0);
        GgfCkptHeader h;
        GgfCkptEntry *e = NULL;
        CHECK("T16d: read กลางรอบ ok", ggf_ckpt_read(mid, &h, &e) == 0);
        CHECK("T16e: ckpt_round/tick ตรง", h.ckpt_round == 72 && h.ckpt_tick == 0);
        uint64_t bytes = 0;
        uint32_t ok = 0, fail = 0, skip = 0;
        int rc = ggf_ckpt_replay(&h, e, "build/ckpt_t", "", cmp_orig, NULL,
                                 &bytes, &ok, &fail, &skip);
        CHECK("T16f: replay กลางรอบ — pending %u/11 lossless · skip %u · fail 0",
              rc == 0 && ok == exp_pending && skip == exp_skip && fail == 0);
        free(e);
    }

    /* ══════════ DELTA — เก็บเฉพาะ tensor ที่เปลี่ยนจาก base ══════════ */
    printf("\n— DELTA checkpoint (diff ระดับไฟล์ — เก็บเฉพาะที่เปลี่ยน) —\n");
    {
        /* base: checkpoint เต็มใน build/ckpt_db/base (สำเนาเดิมของ data) */
        static uint8_t *base_data[NT];      /* สำเนาต้นฉบับก่อนแก้ */
        static char base_paths[NT][256];
        system("mkdir -p build/ckpt_db/base build/ckpt_db/delta");
        for (uint32_t i = 0; i < NT; i++) {
            base_data[i] = NULL;
            if (g_sizes[i] > 0) {
                base_data[i] = (uint8_t *)malloc(g_sizes[i]);
                memcpy(base_data[i], g_data[i], g_sizes[i]);
            }
            ggf_ckpt_path(base_paths[i], sizeof base_paths[i],
                          "build/ckpt_db/base", i, g_names[i]);
        }
        uint8_t *base_status = (uint8_t *)malloc(NT);
        memset(base_status, GGF_CKPT_STORED, NT);
        uint32_t n_base_save = 0;
        for (uint32_t i = 0; i < NT; i++) {
            if (g_home[i] != (int32_t)i || g_sizes[i] == 0) continue;
            if (ggf_save_map(base_data[i], g_sizes[i], 8, base_paths[i]) == 0)
                n_base_save++;
        }
        CHECK("T17a: base checkpoint เต็ม — save 8 home .ggf", n_base_save == 8);
        CHECK("T17b: base manifest (status=NULL → ทุกตัว STORED)",
              ggf_ckpt_write("build/ckpt_db/base/manifest.mfp", 42u, 12u, 144u, NT,
                             g_names, g_sizes, g_home, NULL, NULL,
                             2164u, 0u, 0u, 0u, "base v1", "delta-test.gguf") == 0);

        /* เปลี่ยน data ของ tensor 1, 5, 7 (รอบใหม่) — และ dup ของ t1
         * (t6) ต้องเปลี่ยนตาม (tied pair: home เปลี่ยน ⇒ dup เปลี่ยนด้วย) */
        for (uint32_t k = 0; k < 3; k++) {
            uint32_t t = (k == 0) ? 1 : (k == 1) ? 5 : 7;
            g_data[t][g_sizes[t] / 2] ^= 0x5A;
            g_data[t][g_sizes[t] / 2 + 7] ^= 0xA5;
        }
        g_data[6][g_sizes[6] / 2] ^= 0x5A;
        g_data[6][g_sizes[6] / 2 + 7] ^= 0xA5;

        /* delta: status จาก ggf_ckpt_cmp_base (diff ระดับไฟล์) */
        uint8_t *status = (uint8_t *)malloc(NT);
        for (uint32_t i = 0; i < NT; i++) {
            if (g_home[i] == (int32_t)i && g_sizes[i] > 0)
                status[i] = ggf_ckpt_cmp_base("build/ckpt_db/base", i,
                                              g_names[i], g_sizes[i], g_data[i]);
            else
                status[i] = GGF_CKPT_STORED;
        }
        for (uint32_t i = 0; i < NT; i++)
            if (g_home[i] != (int32_t)i) status[i] = status[g_home[i]];
        uint32_t n_same = 0, n_stored = 0;
        for (uint32_t i = 0; i < NT; i++) {
            if (g_home[i] != (int32_t)i) continue;
            if (status[i] == GGF_CKPT_SAME) n_same++; else n_stored++;
        }
        CHECK("T17c: diff จับถูก — เปลี่ยน 3 (STORED) · ไม่เปลี่ยน 5 (SAME)",
              n_stored == 3 && n_same == 5 &&
              status[1] == GGF_CKPT_STORED && status[3] == GGF_CKPT_SAME &&
              status[0] == GGF_CKPT_SAME);
        CHECK("T17d: dup สถานะตาม home (t4→t0 SAME · t6→t1 STORED)",
              status[4] == GGF_CKPT_SAME && status[6] == GGF_CKPT_STORED);

        /* เก็บเฉพาะ STORED ลง delta dir */
        uint32_t n_delta_save = 0;
        for (uint32_t i = 0; i < NT; i++) {
            if (g_home[i] != (int32_t)i || g_sizes[i] == 0) continue;
            if (status[i] != GGF_CKPT_SAME)
                if (ggf_save_map(g_data[i], g_sizes[i], 8, g_paths_db_delta(i)) == 0)
                    n_delta_save++;
        }
        CHECK("T17e: delta เก็บเฉพาะ STORED (3 ไฟล์ — ไม่ใช่ 8)", n_delta_save == 3);
        CHECK("T17f: delta manifest (base_dir + status)",
              ggf_ckpt_write("build/ckpt_db/delta/manifest.mfp", 42u, 12u, 144u, NT,
                             g_names, g_sizes, g_home, status,
                             "build/ckpt_db/base",
                             2164u, 0u, 0u, 0u, "delta v1", "delta-test.gguf") == 0);

        /* replay: merge delta + base → lossless 11/11 */
        {
            GgfCkptHeader h;
            GgfCkptEntry *e = NULL;
            CHECK("T17g: read delta manifest ok (base_dir ตรง)",
                  ggf_ckpt_read("build/ckpt_db/delta/manifest.mfp", &h, &e) == 0 &&
                  strcmp(h.base_dir, "build/ckpt_db/base") == 0);
            uint64_t bytes = 0;
            uint32_t ok = 0, fail = 0, skip = 0;
            int rc = ggf_ckpt_replay(&h, e, "build/ckpt_db/delta",
                                     "build/ckpt_db/base", cmp_orig, NULL,
                                     &bytes, &ok, &fail, &skip);
            CHECK("T17h: replay delta merge — 11/11 lossless (3 จาก delta, 5 จาก base)",
                  rc == 0 && ok == 11 && fail == 0 && skip == 0);
            free(e);
        }

        /* verify: delta ตรวจทั้ง base+delta โดยไม่ต้องมี data */
        {
            uint32_t files = 0, fail = 0;
            int rc = ggf_ckpt_verify("build/ckpt_db/delta", &files, &fail);
            CHECK("T17i: verify delta — 8 home (3 delta + 5 base) CRC ผ่าน",
                  rc == 0 && files == 8 && fail == 0);
        }

        /* corrupt base file ที่ SAME อ้าง → verify + replay จับ */
        {
            FILE *f = fopen(base_paths[3], "r+b");   /* base t3 (SAME) */
            fseek(f, 64 + 4 + 4 + 33, SEEK_SET);
            uint8_t b;
            fread(&b, 1, 1, f); b ^= 0xFF;
            fseek(f, -1, SEEK_CUR); fwrite(&b, 1, 1, f);
            fclose(f);
            uint32_t files = 0, fail = 0;
            int rc = ggf_ckpt_verify("build/ckpt_db/delta", &files, &fail);
            CHECK("T17j: base .ggf corrupt → verify delta จับ (fail=1)",
                  rc != 0 && fail == 1);
            GgfCkptHeader h;
            GgfCkptEntry *e = NULL;
            ggf_ckpt_read("build/ckpt_db/delta/manifest.mfp", &h, &e);
            uint32_t ok = 0, fail2 = 0, skip = 0;
            ggf_ckpt_replay(&h, e, "build/ckpt_db/delta", "build/ckpt_db/base",
                            cmp_orig, NULL, NULL, &ok, &fail2, &skip);
            CHECK("T17k: base .ggf corrupt → replay delta จับ fail (1)",
                  rc != 0 && fail2 == 1);
            free(e);
            ggf_save_map(base_data[3], g_sizes[3], 8, base_paths[3]);   /* restore */
        }

        /* corrupt delta file (STORED) → verify จับ */
        {
            FILE *f = fopen(g_paths_db_delta(1), "r+b");
            fseek(f, 64 + 4 + 4 + 10, SEEK_SET);
            uint8_t b;
            fread(&b, 1, 1, f); b ^= 0xFF;
            fseek(f, -1, SEEK_CUR); fwrite(&b, 1, 1, f);
            fclose(f);
            uint32_t files = 0, fail = 0;
            int rc = ggf_ckpt_verify("build/ckpt_db/delta", &files, &fail);
            CHECK("T17l: delta .ggf corrupt → verify จับ (fail=1)",
                  rc != 0 && fail == 1);
            ggf_save_map(g_data[1], g_sizes[1], 8, g_paths_db_delta(1));
        }

        /* base ไฟล์หาย → delta ต้องเก็บเอง (STORED) + replay ผ่าน */
        {
            remove(base_paths[11]);
            uint8_t st = ggf_ckpt_cmp_base("build/ckpt_db/base", 11,
                                           g_names[11], g_sizes[11], g_data[11]);
            CHECK("T17m: base หาย → cmp_base คืน STORED (เก็บเอง)",
                  st == GGF_CKPT_STORED);
            ggf_save_map(g_data[11], g_sizes[11], 8, g_paths_db_delta(11));
            uint8_t *st2 = (uint8_t *)malloc(NT);
            memcpy(st2, status, NT);
            st2[11] = GGF_CKPT_STORED;
            ggf_ckpt_write("build/ckpt_db/delta/manifest.mfp", 42u, 12u, 144u, NT,
                           g_names, g_sizes, g_home, st2, "build/ckpt_db/base",
                           2164u, 0u, 0u, 0u, "delta v1", "delta-test.gguf");
            GgfCkptHeader h;
            GgfCkptEntry *e = NULL;
            ggf_ckpt_read("build/ckpt_db/delta/manifest.mfp", &h, &e);
            uint64_t bytes = 0;
            uint32_t ok = 0, fail = 0, skip = 0;
            int rc = ggf_ckpt_replay(&h, e, "build/ckpt_db/delta",
                                     "build/ckpt_db/base", cmp_orig, NULL,
                                     &bytes, &ok, &fail, &skip);
            CHECK("T17n: base หาย → replay delta ยัง lossless (เก็บเองที่ delta)",
                  rc == 0 && ok == 11 && fail == 0);
            free(e); free(st2);
            ggf_save_map(base_data[11], g_sizes[11], 8, base_paths[11]);
        }

        /* tamper: แก้ status ใน entry → crc64 จับ */
        {
            FILE *src = fopen("build/ckpt_db/delta/manifest.mfp", "rb");
            FILE *dst = fopen("build/ckpt_db/delta/tamper.mfp", "wb");
            int c;
            while ((c = fgetc(src)) != EOF) fputc(c, dst);
            fclose(src); fclose(dst);
            /* entry 0: status byte อยู่ที่ sizeof(header) + 128 + 4 + 4 */
            FILE *f = fopen("build/ckpt_db/delta/tamper.mfp", "r+b");
            fseek(f, (long)sizeof(GgfCkptHeader) + GGF_CKPT_NAME_LEN + 8, SEEK_SET);
            uint8_t b;
            fread(&b, 1, 1, f); b ^= 0xFF;
            fseek(f, -1, SEEK_CUR); fwrite(&b, 1, 1, f);
            fclose(f);
            GgfCkptHeader h;
            GgfCkptEntry *e = NULL;
            CHECK("T17o: แก้ status ใน manifest → ปฏิเสธ (rc=-7 crc64)",
                  ggf_ckpt_read("build/ckpt_db/delta/tamper.mfp", &h, &e) == -7);
            /* แก้ base_dir ใน header → crc64 จับ */
            f = fopen("build/ckpt_db/delta/tamper.mfp", "r+b");
            fseek(f, (long)offsetof(GgfCkptHeader, base_dir) + 3, SEEK_SET);
            fread(&b, 1, 1, f); b ^= 0x01;
            fseek(f, -1, SEEK_CUR); fwrite(&b, 1, 1, f);
            fclose(f);
            CHECK("T17p: แก้ base_dir → ปฏิเสธ (rc=-7)",
                  ggf_ckpt_read("build/ckpt_db/delta/tamper.mfp", &h, &e) == -7);
            remove("build/ckpt_db/delta/tamper.mfp");
        }

        /* SAME + ไม่มี base_dir → ปฏิเสธ (rc=-11) */
        {
            uint8_t *st = (uint8_t *)malloc(NT);
            memset(st, GGF_CKPT_SAME, NT);
            ggf_ckpt_write("build/ckpt_db/delta/nobase.mfp", 42u, 12u, 144u, NT,
                           g_names, g_sizes, g_home, st, NULL,
                           2164u, 0u, 0u, 0u, "delta v1", "delta-test.gguf");
            GgfCkptHeader h;
            GgfCkptEntry *e = NULL;
            CHECK("T17q: SAME แต่ไม่มี base_dir → ปฏิเสธ (rc=-11)",
                  ggf_ckpt_read("build/ckpt_db/delta/nobase.mfp", &h, &e) == -11);
            remove("build/ckpt_db/delta/nobase.mfp");
            free(st);
        }

        /* cleanup delta section */
        for (uint32_t i = 0; i < NT; i++) {
            remove(base_paths[i]);
            remove(g_paths_db_delta(i));
            free(base_data[i]);
        }
        remove("build/ckpt_db/base/manifest.mfp");
        remove("build/ckpt_db/delta/manifest.mfp");
        free(status); free(base_status);
    }

    /* ══════════ DELTA CHAIN — base → delta1 → delta2 (multi-level) + GC ══════════ */
    printf("\n— DELTA CHAIN (multi-level: base → delta1 → delta2) + GC —\n");
    {
        const char *db = "build/ckpt_db/base2";   /* tail (เต็ม) */
        const char *d1 = "build/ckpt_db/d1";
        const char *d2 = "build/ckpt_db/d2";
        const char *gc = "build/ckpt_db/gc";
        const char *lp = "build/ckpt_db/loop";
        ensure_dir(db); ensure_dir(d1); ensure_dir(d2); ensure_dir(gc);
        ensure_dir(lp);

        static char p_b[NT][256], p_1[NT][256], p_2[NT][256], p_g[NT][256];
        for (uint32_t i = 0; i < NT; i++) {
            ggf_ckpt_path(p_b[i], sizeof p_b[i], db, i, g_names[i]);
            ggf_ckpt_path(p_1[i], sizeof p_1[i], d1, i, g_names[i]);
            ggf_ckpt_path(p_2[i], sizeof p_2[i], d2, i, g_names[i]);
            ggf_ckpt_path(p_g[i], sizeof p_g[i], gc, i, g_names[i]);
        }

        /* base: สำเนาของ g_data ปัจจุบัน (T17 แก้ t1/t5/t7/t6 ไปแล้ว — ใช้เป็น
         * ฐานของ chain นี้ · deterministic เท่าเดิม) */
        static uint8_t *b_data[NT];
        uint32_t n_b = 0;
        for (uint32_t i = 0; i < NT; i++) {
            b_data[i] = NULL;
            if (g_sizes[i] > 0) {
                b_data[i] = (uint8_t *)malloc(g_sizes[i]);
                memcpy(b_data[i], g_data[i], g_sizes[i]);
            }
            if (g_home[i] == (int32_t)i && g_sizes[i] > 0) {
                if (ggf_save_map(b_data[i], g_sizes[i], 8, p_b[i]) == 0) n_b++;
            }
        }
        CHECK("T18a: base2 (tail) — save 8 home .ggf", n_b == 8);
        ggf_ckpt_write("build/ckpt_db/base2/manifest.mfp", 42u, 12u, 144u, NT,
                       g_names, g_sizes, g_home, NULL, NULL,
                       2164u, 0u, 0u, 0u, "chain base", "chain-test.gguf");

        /* delta1: เปลี่ยน t1 (+dup t6 ตาม) และ t5 */
        for (uint32_t k = 0; k < 2; k++) {
            uint32_t t = (k == 0) ? 1 : 5;
            g_data[t][g_sizes[t] / 2] ^= 0x11;
            g_data[t][g_sizes[t] / 2 + 3] ^= 0x22;
        }
        g_data[6][g_sizes[6] / 2] ^= 0x11;
        g_data[6][g_sizes[6] / 2 + 3] ^= 0x22;

        uint8_t *st1 = (uint8_t *)malloc(NT);
        for (uint32_t i = 0; i < NT; i++) {
            if (g_home[i] == (int32_t)i && g_sizes[i] > 0)
                st1[i] = ggf_ckpt_cmp_base(db, i, g_names[i], g_sizes[i], g_data[i]);
            else st1[i] = GGF_CKPT_STORED;
        }
        for (uint32_t i = 0; i < NT; i++)
            if (g_home[i] != (int32_t)i) st1[i] = st1[g_home[i]];
        uint32_t st1_cnt = 0;
        for (uint32_t i = 0; i < NT; i++)
            if (g_home[i] == (int32_t)i && st1[i] == GGF_CKPT_STORED) st1_cnt++;
        CHECK("T18b: delta1 diff ถูก — เปลี่ยน 2 (t1,t5 STORED) · ไม่เปลี่ยน 6 (SAME)",
              st1_cnt == 2 && st1[1] == GGF_CKPT_STORED && st1[5] == GGF_CKPT_STORED &&
              st1[3] == GGF_CKPT_SAME && st1[7] == GGF_CKPT_SAME);
        uint32_t n_d1 = 0;
        for (uint32_t i = 0; i < NT; i++) {
            if (g_home[i] != (int32_t)i || g_sizes[i] == 0) continue;
            if (st1[i] == GGF_CKPT_STORED)
                if (ggf_save_map(g_data[i], g_sizes[i], 8, p_1[i]) == 0) n_d1++;
        }
        CHECK("T18c: delta1 เก็บเฉพาะ STORED (2 ไฟล์)", n_d1 == 2);
        CHECK("T18d: delta1 manifest (base_dir=base2)",
              ggf_ckpt_write("build/ckpt_db/d1/manifest.mfp", 42u, 12u, 144u, NT,
                             g_names, g_sizes, g_home, st1, db,
                             2164u, 0u, 0u, 0u, "delta1", "chain-test.gguf") == 0);

        /* delta2: เปลี่ยน t7 (ตัวเดียว) — cmp ผ่าน chain (เห็น t1/t5 ของ d1) */
        g_data[7][g_sizes[7] / 2] ^= 0x33;
        g_data[7][g_sizes[7] / 2 + 5] ^= 0x44;
        uint8_t *st2 = (uint8_t *)malloc(NT);
        for (uint32_t i = 0; i < NT; i++) {
            if (g_home[i] == (int32_t)i && g_sizes[i] > 0)
                st2[i] = ggf_ckpt_cmp_base(d1, i, g_names[i], g_sizes[i], g_data[i]);
            else st2[i] = GGF_CKPT_STORED;
        }
        for (uint32_t i = 0; i < NT; i++)
            if (g_home[i] != (int32_t)i) st2[i] = st2[g_home[i]];
        CHECK("T18e: delta2 cmp ผ่าน chain — เปลี่ยน t7 (STORED) · t1/t5 SAME (อ้าง d1) · t3 SAME (อ้าง base)",
              st2[7] == GGF_CKPT_STORED && st2[1] == GGF_CKPT_SAME &&
              st2[5] == GGF_CKPT_SAME && st2[3] == GGF_CKPT_SAME);
        uint32_t n_d2 = 0;
        for (uint32_t i = 0; i < NT; i++) {
            if (g_home[i] != (int32_t)i || g_sizes[i] == 0) continue;
            if (st2[i] == GGF_CKPT_STORED)
                if (ggf_save_map(g_data[i], g_sizes[i], 8, p_2[i]) == 0) n_d2++;
        }
        CHECK("T18f: delta2 เก็บเฉพาะ STORED (1 ไฟล์ — t7)", n_d2 == 1);
        CHECK("T18g: delta2 manifest (base_dir=d1)",
              ggf_ckpt_write("build/ckpt_db/d2/manifest.mfp", 42u, 12u, 144u, NT,
                             g_names, g_sizes, g_home, st2, d1,
                             2164u, 0u, 0u, 0u, "delta2", "chain-test.gguf") == 0);

        /* chain open: 3 ระดับ + resolve */
        {
            GgfCkptChain ch;
            int rc = ggf_ckpt_chain_open(d2, &ch);
            CHECK("T18h: chain_open(d2) — 3 ระดับ (d2→d1→base2) n ตรง",
                  rc == 0 && ch.depth == 3 && ch.n == NT &&
                  strcmp(ch.links[2].dir, db) == 0);
            char path[512];
            int ok_r = ggf_ckpt_chain_path(&ch, 1, g_names[1], path, sizeof path) == 0 &&
                       strncmp(path, d1, strlen(d1)) == 0;
            ok_r &= ggf_ckpt_chain_path(&ch, 3, g_names[3], path, sizeof path) == 0 &&
                    strncmp(path, db, strlen(db)) == 0;
            ok_r &= ggf_ckpt_chain_path(&ch, 7, g_names[7], path, sizeof path) == 0 &&
                    strncmp(path, d2, strlen(d2)) == 0;
            CHECK("T18i: resolve ผ่าน chain — t1→d1 · t3→base2 · t7→d2", ok_r);
            ggf_ckpt_chain_close(&ch);
        }

        /* replay d2 — ไฟล์มาจากทั้ง 3 ระดับ */
        {
            GgfCkptHeader h;
            GgfCkptEntry *e = NULL;
            CHECK("T18j: read d2 manifest ok",
                  ggf_ckpt_read("build/ckpt_db/d2/manifest.mfp", &h, &e) == 0);
            uint64_t bytes = 0;
            uint32_t ok = 0, fail = 0, skip = 0;
            int rc = ggf_ckpt_replay(&h, e, d2, NULL, cmp_orig, NULL,
                                     &bytes, &ok, &fail, &skip);
            CHECK("T18k: replay d2 ผ่านทั้ง chain — 11/11 lossless (base+d1+d2)",
                  rc == 0 && ok == 11 && fail == 0);
            free(e);
        }

        /* verify d2 — ผ่าน chain (8 home: 1 ใน d2 + 2 ใน d1 + 5 ใน base2) */
        {
            uint32_t files = 0, fail = 0;
            int rc = ggf_ckpt_verify(d2, &files, &fail);
            CHECK("T18l: verify d2 — 8 home ผ่าน chain (CRC ทุกไฟล์)",
                  rc == 0 && files == 8 && fail == 0);
        }

        /* corrupt ไฟล์ลึกใน chain (t3 อยู่ที่ base2 — SAME→SAME→STORED) */
        {
            FILE *f = fopen(p_b[3], "r+b");
            fseek(f, 64 + 4 + 4 + 20, SEEK_SET);
            uint8_t b;
            fread(&b, 1, 1, f); b ^= 0xFF;
            fseek(f, -1, SEEK_CUR); fwrite(&b, 1, 1, f);
            fclose(f);
            uint32_t files = 0, fail = 0;
            int rc = ggf_ckpt_verify(d2, &files, &fail);
            CHECK("T18m: corrupt ไฟล์ลึก (base2 t3) → verify d2 จับ (fail=1)",
                  rc != 0 && fail == 1);
            GgfCkptHeader h;
            GgfCkptEntry *e = NULL;
            ggf_ckpt_read("build/ckpt_db/d2/manifest.mfp", &h, &e);
            uint32_t ok = 0, fail2 = 0, skip = 0;
            ggf_ckpt_replay(&h, e, d2, NULL, cmp_orig, NULL, NULL, &ok, &fail2, &skip);
            CHECK("T18n: corrupt ไฟล์ลึก → replay d2 จับ fail (1)", fail2 == 1);
            free(e);
            ggf_save_map(b_data[3], g_sizes[3], 8, p_b[3]);
        }

        /* manifest ระดับกลางพัง (d1 ถูกแก้) → chain_open(d2) ปฏิเสธ */
        {
            FILE *f = fopen("build/ckpt_db/d1/manifest.mfp", "r+b");
            fseek(f, 8, SEEK_SET);                 /* seed field */
            uint8_t b;
            fread(&b, 1, 1, f); b ^= 0x01;
            fseek(f, -1, SEEK_CUR); fwrite(&b, 1, 1, f);
            fclose(f);
            GgfCkptChain ch;
            int rc = ggf_ckpt_chain_open(d2, &ch);
            CHECK("T18o: manifest กลาง (d1) ถูกแก้ → chain_open ปฏิเสธ (crc64)",
                  rc != 0);
            ggf_ckpt_write("build/ckpt_db/d1/manifest.mfp", 42u, 12u, 144u, NT,
                           g_names, g_sizes, g_home, st1, db,
                           2164u, 0u, 0u, 0u, "delta1", "chain-test.gguf");
        }

        /* วน: manifest base_dir ชี้ตัวเอง → chain_open จับ (-3) */
        {
            uint8_t *sts = (uint8_t *)malloc(NT);
            memset(sts, GGF_CKPT_SAME, NT);
            for (uint32_t i = 0; i < NT; i++)
                if (g_home[i] != (int32_t)i) sts[i] = GGF_CKPT_STORED;
            ggf_ckpt_write("build/ckpt_db/loop/manifest.mfp", 42u, 12u, 144u, NT,
                           g_names, g_sizes, g_home, sts, lp,   /* base = ตัวเอง */
                           2164u, 0u, 0u, 0u, "loop", "chain-test.gguf");
            GgfCkptChain ch;
            int rc = ggf_ckpt_chain_open(lp, &ch);
            CHECK("T18p: chain วน (base ชี้ตัวเอง) → ปฏิเสธ (-3)", rc == -3);
            remove("build/ckpt_db/loop/manifest.mfp");
            free(sts);
        }

        /* ═══ GC: รวม chain เป็น snapshot ใหม่ ═══ */
        {
            uint32_t n_home = 0, n_fail = 0;
            uint64_t bytes = 0;
            int rc = ggf_ckpt_gc(d2, gc, "gc test", &n_home, &bytes, &n_fail);
            CHECK("T19a: GC — รวม chain → snapshot (8 home, 0 fail)",
                  rc == 0 && n_home == 8 && n_fail == 0);
            GgfCkptHeader h;
            GgfCkptEntry *e = NULL;
            CHECK("T19b: GC manifest — เต็ม (base ว่าง, n ตรง)",
                  ggf_ckpt_read("build/ckpt_db/gc/manifest.mfp", &h, &e) == 0 &&
                  h.base_dir[0] == 0 && h.n == NT &&
                  h.dup_bytes == 2164 && h.ckpt_round == 0);
            int all_stored = 1;
            for (uint32_t i = 0; i < NT; i++)
                if (e[i].status != GGF_CKPT_STORED) all_stored = 0;
            CHECK("T19c: ทุก entry ของ snapshot เป็น STORED (self-contained)",
                  all_stored);
            free(e);

            /* ลบ chain เดิมทั้งหมด (จำลอง GC จริง) — snapshot ต้องอยู่ได้ลำพัง */
            for (uint32_t i = 0; i < NT; i++)
                remove(p_b[i]), remove(p_1[i]), remove(p_2[i]);
            remove("build/ckpt_db/base2/manifest.mfp");
            remove("build/ckpt_db/d1/manifest.mfp");
            remove("build/ckpt_db/d2/manifest.mfp");

            GgfCkptHeader h2;
            GgfCkptEntry *e2 = NULL;
            ggf_ckpt_read("build/ckpt_db/gc/manifest.mfp", &h2, &e2);
            uint64_t bytes2 = 0;
            uint32_t ok = 0, fail = 0, skip = 0;
            int rr = ggf_ckpt_replay(&h2, e2, gc, NULL, cmp_orig, NULL,
                                     &bytes2, &ok, &fail, &skip);
            CHECK("T19d: replay snapshot ลำพัง (chain ลบแล้ว) — 11/11 lossless",
                  rr == 0 && ok == 11 && fail == 0);
            uint32_t files = 0, vfail = 0;
            int vr = ggf_ckpt_verify(gc, &files, &vfail);
            CHECK("T19e: verify snapshot ลำพัง — 8/8 ผ่าน (ไม่พึ่ง chain)",
                  vr == 0 && files == 8 && vfail == 0);
            free(e2);

            /* chain เดิมลบแล้ว → d2 เดิมใช้ไม่ได้ (พิสูจน์ว่าต้องใช้ snapshot) */
            GgfCkptHeader h3;
            GgfCkptEntry *e3 = NULL;
            int rd3 = ggf_ckpt_read("build/ckpt_db/d2/manifest.mfp", &h3, &e3);
            int rr3 = 1;
            if (rd3 == 0) {
                uint32_t ok3 = 0, fail3 = 0, skip3 = 0;
                rr3 = ggf_ckpt_replay(&h3, e3, d2, NULL, cmp_orig, NULL, NULL,
                                      &ok3, &fail3, &skip3);
                free(e3);
            }
            CHECK("T19f: chain เดิมลบแล้ว → d2 ใช้ไม่ได้ (snapshot แทนที่)",
                  rd3 != 0 || rr3 != 0);
        }

        /* cleanup chain section */
        for (uint32_t i = 0; i < NT; i++) {
            remove(p_g[i]);
            free(b_data[i]);
        }
        remove("build/ckpt_db/gc/manifest.mfp");
        free(st1); free(st2);
    }

    /* ══════════ AUTO-GC — chain ลึกเกิน threshold → รวม snapshot อัตโนมัติ ══════════ */
    printf("\n— AUTO-GC (ggf_ckpt_auto_gc — chain ลึกเกิน → รวมเป็น snapshot ใหม่) —\n");
    {
        const char *a_b = "build/ckpt_db/ag_base";
        const char *a_1 = "build/ckpt_db/ag_d1";
        const char *a_2 = "build/ckpt_db/ag_d2";
        const char *a_g = "build/ckpt_db/ag_gc";
        const char *a_3 = "build/ckpt_db/ag_d3";
        ensure_dir(a_b); ensure_dir(a_1); ensure_dir(a_2);
        ensure_dir(a_g); ensure_dir(a_3);

        static char q_b[NT][256], q_1[NT][256], q_2[NT][256], q_g[NT][256], q_3[NT][256];
        for (uint32_t i = 0; i < NT; i++) {
            ggf_ckpt_path(q_b[i], sizeof q_b[i], a_b, i, g_names[i]);
            ggf_ckpt_path(q_1[i], sizeof q_1[i], a_1, i, g_names[i]);
            ggf_ckpt_path(q_2[i], sizeof q_2[i], a_2, i, g_names[i]);
            ggf_ckpt_path(q_g[i], sizeof q_g[i], a_g, i, g_names[i]);
            ggf_ckpt_path(q_3[i], sizeof q_3[i], a_3, i, g_names[i]);
        }

        /* base เต็มจาก g_data ปัจจุบัน (T18 แก้ t1/t5/t6/t7 ไปแล้ว) */
        static uint8_t *q_data[NT];
        for (uint32_t i = 0; i < NT; i++) {
            q_data[i] = NULL;
            if (g_sizes[i] > 0) {
                q_data[i] = (uint8_t *)malloc(g_sizes[i]);
                memcpy(q_data[i], g_data[i], g_sizes[i]);
            }
            if (g_home[i] == (int32_t)i && g_sizes[i] > 0)
                ggf_save_map(q_data[i], g_sizes[i], 8, q_b[i]);
        }
        ggf_ckpt_write("build/ckpt_db/ag_base/manifest.mfp", 42u, 12u, 144u, NT,
                       g_names, g_sizes, g_home, NULL, NULL,
                       2164u, 0u, 0u, 0u, "ag base", "ag.gguf");

        /* d1: เปลี่ยน t2 (+dup t8 ตาม — tied pair: home เปลี่ยน ⇒ dup เปลี่ยน) */
        g_data[2][g_sizes[2] / 2] ^= 0xAA;
        g_data[8][g_sizes[8] / 2] ^= 0xAA;
        uint8_t *q1s = (uint8_t *)malloc(NT);
        for (uint32_t i = 0; i < NT; i++) {
            if (g_home[i] == (int32_t)i && g_sizes[i] > 0)
                q1s[i] = ggf_ckpt_cmp_base(a_b, i, g_names[i], g_sizes[i], g_data[i]);
            else q1s[i] = GGF_CKPT_STORED;
        }
        for (uint32_t i = 0; i < NT; i++)
            if (g_home[i] != (int32_t)i) q1s[i] = q1s[g_home[i]];
        for (uint32_t i = 0; i < NT; i++)
            if (g_home[i] == (int32_t)i && q1s[i] == GGF_CKPT_STORED && g_sizes[i] > 0)
                ggf_save_map(g_data[i], g_sizes[i], 8, q_1[i]);
        ggf_ckpt_write("build/ckpt_db/ag_d1/manifest.mfp", 42u, 12u, 144u, NT,
                       g_names, g_sizes, g_home, q1s, a_b,
                       2164u, 0u, 0u, 0u, "ag d1", "ag.gguf");

        /* d2: เปลี่ยน t5 → เก็บ 1 ไฟล์ (chain ลึก 3: d2→d1→base) */
        g_data[5][g_sizes[5] / 2] ^= 0xBB;
        uint8_t *q2s = (uint8_t *)malloc(NT);
        for (uint32_t i = 0; i < NT; i++) {
            if (g_home[i] == (int32_t)i && g_sizes[i] > 0)
                q2s[i] = ggf_ckpt_cmp_base(a_1, i, g_names[i], g_sizes[i], g_data[i]);
            else q2s[i] = GGF_CKPT_STORED;
        }
        for (uint32_t i = 0; i < NT; i++)
            if (g_home[i] != (int32_t)i) q2s[i] = q2s[g_home[i]];
        for (uint32_t i = 0; i < NT; i++)
            if (g_home[i] == (int32_t)i && q2s[i] == GGF_CKPT_STORED && g_sizes[i] > 0)
                ggf_save_map(g_data[i], g_sizes[i], 8, q_2[i]);
        ggf_ckpt_write("build/ckpt_db/ag_d2/manifest.mfp", 42u, 12u, 144u, NT,
                       g_names, g_sizes, g_home, q2s, a_1,
                       2164u, 0u, 0u, 0u, "ag d2", "ag.gguf");

        /* max_chain ใหญ่พอ → ไม่ GC (chain ลึก 3 < 4 → return 0) */
        {
            char nb[1024];
            int ag = ggf_ckpt_auto_gc(a_2, a_g, 4, "auto-gc", nb, sizeof nb, NULL, NULL);
            CHECK("T20a: chain ลึก 3 < max 4 → ไม่ GC (ใช้ base เดิม)", ag == 0);
        }
        /* max_chain=3 → ลึกเท่า threshold → GC อัตโนมัติ (return 1, base ใหม่) */
        {
            char nb[1024];
            uint32_t h = 0;
            uint64_t by = 0;
            int ag = ggf_ckpt_auto_gc(a_2, a_g, 3, "auto-gc", nb, sizeof nb, &h, &by);
            CHECK("T20b: chain ลึก 3 ≥ max 3 → GC อัตโนมัติ (home 8, bytes > 0)",
                  ag == 1 && h == 8 && by > 0);
            CHECK("T20c: base ใหม่ = ag_gc (self-contained)", strcmp(nb, a_g) == 0);
            GgfCkptHeader hd;
            GgfCkptEntry *e = NULL;
            CHECK("T20d: GC manifest — เต็ม (base ว่าง · ทุกตัว STORED)",
                  ggf_ckpt_read("build/ckpt_db/ag_gc/manifest.mfp", &hd, &e) == 0 &&
                  hd.base_dir[0] == 0);
            free(e);
        }
        /* delta ระดับถัดไป (d3) บน base ที่ถูก GC แล้ว — chain ใหม่สั้น */
        {
            g_data[7][g_sizes[7] / 2] ^= 0xCC;          /* เปลี่ยน t7 */
            uint8_t *q3s = (uint8_t *)malloc(NT);
            for (uint32_t i = 0; i < NT; i++) {
                if (g_home[i] == (int32_t)i && g_sizes[i] > 0)
                    q3s[i] = ggf_ckpt_cmp_base(a_g, i, g_names[i], g_sizes[i], g_data[i]);
                else q3s[i] = GGF_CKPT_STORED;
            }
            for (uint32_t i = 0; i < NT; i++)
                if (g_home[i] != (int32_t)i) q3s[i] = q3s[g_home[i]];
            uint32_t st3 = 0;
            for (uint32_t i = 0; i < NT; i++)
                if (g_home[i] == (int32_t)i && q3s[i] == GGF_CKPT_STORED) st3++;
            CHECK("T20e: d3 เทียบ GC base — เปลี่ยน t7 (STORED) · t2/t5 SAME (GC เก็บให้แล้ว)",
                  st3 == 1 && q3s[7] == GGF_CKPT_STORED &&
                  q3s[2] == GGF_CKPT_SAME && q3s[5] == GGF_CKPT_SAME);
            for (uint32_t i = 0; i < NT; i++)
                if (g_home[i] == (int32_t)i && q3s[i] == GGF_CKPT_STORED && g_sizes[i] > 0)
                    ggf_save_map(g_data[i], g_sizes[i], 8, q_3[i]);
            ggf_ckpt_write("build/ckpt_db/ag_d3/manifest.mfp", 42u, 12u, 144u, NT,
                           g_names, g_sizes, g_home, q3s, a_g,
                           2164u, 0u, 0u, 0u, "ag d3", "ag.gguf");
            GgfCkptChain ch;
            int rc = ggf_ckpt_chain_open(a_3, &ch);
            CHECK("T20f: chain ใหม่สั้น — d3→ag_gc (ลึก 2 ไม่ใช่ 4)",
                  rc == 0 && ch.depth == 2);
            ggf_ckpt_chain_close(&ch);
            GgfCkptHeader hd;
            GgfCkptEntry *e = NULL;
            ggf_ckpt_read("build/ckpt_db/ag_d3/manifest.mfp", &hd, &e);
            uint64_t bytes = 0;
            uint32_t ok = 0, fail = 0, skip = 0;
            int rr = ggf_ckpt_replay(&hd, e, a_3, NULL, cmp_orig, NULL,
                                     &bytes, &ok, &fail, &skip);
            CHECK("T20g: replay d3 ผ่าน GC base — 11/11 lossless",
                  rr == 0 && ok == 11 && fail == 0);
            free(e);
            free(q3s);
        }

        /* cleanup AUTO-GC section */
        for (uint32_t i = 0; i < NT; i++) {
            remove(q_b[i]); remove(q_1[i]); remove(q_2[i]);
            remove(q_g[i]); remove(q_3[i]);
            free(q_data[i]);
        }
        remove("build/ckpt_db/ag_base/manifest.mfp");
        remove("build/ckpt_db/ag_d1/manifest.mfp");
        remove("build/ckpt_db/ag_d2/manifest.mfp");
        remove("build/ckpt_db/ag_gc/manifest.mfp");
        remove("build/ckpt_db/ag_d3/manifest.mfp");
        free(q1s); free(q2s);
    }

    /* ── cleanup ────────────────────────────────────────────────── */
    for (uint32_t i = 0; i < NT; i++) {
        remove(g_paths[i]);
        free(g_data[i]);
    }
    remove("build/ckpt_t/manifest.mfp");
    remove("build/ckpt_t_mid/mid.mfp");
    free(g_paths);

    printf("\n═══ RESULT: %d PASS / %d FAIL ═══\n", pass_count, fail_count);
    return fail_count ? 1 : 0;
}
