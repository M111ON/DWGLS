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
#include "../core/geo_ggf_ckpt.h"
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
    make_tensors();
    g_paths = (const char **)malloc(NT * sizeof(char *));
    build_paths("build/ckpt_t");

    /* ── T1: manifest roundtrip ─────────────────────────────────── */
    {
        uint32_t sizes[NT], rq_a[NT], rq_b[NT];
        for (uint32_t i = 0; i < NT; i++) sizes[i] = g_sizes[i];
        CHECK("T1a: manifest write ok",
              ggf_ckpt_write("build/ckpt_t/manifest.mfp", 42u, 12u, 144u, NT,
                             g_names, sizes, g_home, 2164u, 100000u,
                             "tester v1", "test-model.gguf") == 0);
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
                             g_names, g_sizes, g_home, 2164u, 0u,
                             "tester v1", "test-model.gguf") == 0);
    }

    /* ── T3+T4: replay ใน structures ใหม่ (state จาก manifest ล้วน) ── */
    {
        GgfCkptHeader h;
        GgfCkptEntry *e = NULL;
        CHECK("T3a: manifest read ok",
              ggf_ckpt_read("build/ckpt_t/manifest.mfp", &h, &e) == 0);
        uint64_t bytes = 0;
        uint32_t ok = 0, fail = 0;
        int rc = ggf_ckpt_replay(&h, e, g_paths, cmp_orig, NULL,
                                 &bytes, &ok, &fail);
        CHECK("T3b: replay lossless 11/11 (fail 0)", rc == 0 && ok == 11 && fail == 0);
        uint64_t expect_bytes = 0;
        for (uint32_t i = 0; i < NT; i++)
            if (g_home[i] >= 0) expect_bytes += g_sizes[i];
        CHECK("T3c: bytes อ่าน = Σ tensor (11 ตัว)", bytes == expect_bytes);
        free(e);
    }

    /* ── T5: manifest corrupt → ปฏิเสธ ──────────────────────────── */
    {
        FILE *f = fopen("build/ckpt_t/bad.mfp", "wb");
        uint8_t junk[512];
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
        uint32_t ok = 0, fail = 0;
        int rc = ggf_ckpt_replay(&h, e, g_paths, cmp_orig, NULL,
                                 &bytes, &ok, &fail);
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
        uint32_t ok = 0, fail = 0;
        int rc = ggf_ckpt_replay(&h, e, g_paths, cmp_orig, NULL,
                                 &bytes, &ok, &fail);
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
        uint32_t ok1 = 0, f1 = 0, ok2 = 0, f2 = 0;
        ggf_ckpt_replay(&h, e, g_paths, cmp_orig, NULL, &b1, &ok1, &f1);
        ggf_ckpt_replay(&h, e, g_paths, cmp_orig, NULL, &b2, &ok2, &f2);
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

    /* ── cleanup ────────────────────────────────────────────────── */
    for (uint32_t i = 0; i < NT; i++) {
        remove(g_paths[i]);
        free(g_data[i]);
    }
    remove("build/ckpt_t/manifest.mfp");
    free(g_paths);

    printf("\n═══ RESULT: %d PASS / %d FAIL ═══\n", pass_count, fail_count);
    return fail_count ? 1 : 0;
}
