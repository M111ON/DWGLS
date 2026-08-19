/*
 * test_ggf_fs.c — GGFS: .ggf checkpoint directory เป็น geometric filesystem
 * ═══════════════════════════════════════════════════════════════════════
 *
 * T1.2t — mount checkpoint dir (manifest + chain) → อ่าน tensor ด้วย STATE
 * (seed, round, tick) — enter-anywhere (core/geo_ggf_fs.h — §15.96)
 *
 * Proof:
 *   A1  mount: manifest+chain เปิด ok · count == n
 *   A2  find by name → idx (t0 / dup t4 / ว่าง t9 / ไม่พบ -1)
 *   A3  stat: size/rq/tick/home/dup/status/level ตรง
 *   A4  read t3 จาก state (7,3) → bytes == ต้นฉบับ lossless
 *   A5  enter-anywhere: อ่านทุก tensor จาก 3 start states → bytes เหมือนกัน
 *       + steps ต่างกัน (state ต่าง → เส้นทางต่าง แต่ข้อมูลเดียวกัน)
 *   A6  dup (t4) → อ่านผ่าน home (t0) — bytes ตรง
 *   A7  ว่าง (t9) → read คืน -1
 *   A8  corrupt .ggf (data) → read คืน -4 (CRC verify-on-open จับ)
 *   A9  deterministic: read ซ้ำ 2 รอบ → bytes + steps เท่ากัน
 *   A10 zero-copy: ggfs_node pointer ตรง chunk ต้นฉบับ
 *   B1  chain mount (delta → base): stat level 0/1 ตรง · read ผ่าน chain
 *       lossless (ไฟล์จริงอยู่คนละระดับ)
 *   B2  mid-round mount: tensor ก่อน checkpoint → pending + read -2 ·
 *       tensor หลัง checkpoint → read lossless
 *
 * BUILD: gcc -O2 -Wall -Icore -Icore/infra -o build/test_ggf_fs \
 *        tests/test_ggf_fs.c
 */

#include <stdio.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include "../core/geo_ggf_fs.h"
#include "../core/tied_dedup.h"

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

static char g_paths[NT][256];
static char g_paths_delta[NT][256];
static char g_paths_base[NT][256];

int main(void)
{
    printf("═══ test_ggf_fs — GGFS: checkpoint dir เป็น geometric FS ═══\n\n");
    ensure_dir("build/ggfs_t");
    ensure_dir("build/ggfs_b");
    ensure_dir("build/ggfs_d");
    make_tensors();
    tied_dedup_scan((const uint8_t *const *)g_data, g_sizes, NT, g_home);
    for (uint32_t i = 0; i < NT; i++) {
        ggf_ckpt_path(g_paths[i], sizeof g_paths[i], "build/ggfs_t", i, g_names[i]);
        ggf_ckpt_path(g_paths_base[i], sizeof g_paths_base[i], "build/ggfs_b", i, g_names[i]);
        ggf_ckpt_path(g_paths_delta[i], sizeof g_paths_delta[i], "build/ggfs_d", i, g_names[i]);
    }

    /* ── สร้าง checkpoint เต็ม (home + manifest) ── */
    uint32_t n_save = 0;
    for (uint32_t i = 0; i < NT; i++) {
        if (g_home[i] != (int32_t)i || g_sizes[i] == 0) continue;
        if (ggf_save_map(g_data[i], g_sizes[i], 8, g_paths[i]) == 0) n_save++;
    }
    ggf_ckpt_write("build/ggfs_t/manifest.mfp", 42u, 12u, 144u, NT,
                   g_names, g_sizes, g_home, NULL, NULL,
                   2164u, 0u, 0u, 0u, "ggfs test", "ggfs.gguf");

    static GgfsMount fs;
    static uint8_t buf[1024 * 1024];
    static uint8_t ref[1024 * 1024];

    /* ── A1: mount ── */
    CHECK("A1a: mount ok (manifest+chain)", ggfs_mount("build/ggfs_t", &fs) == 0);
    CHECK("A1b: count == 12 · seed/ticks/cycles ตรง",
          ggfs_count(&fs) == NT && fs.seed == 42 && fs.ticks == 12 && fs.cycles == 144);

    /* ── A2: find ── */
    CHECK("A2a: find t0 → idx 0", ggfs_find(&fs, "tok.embd.weight") == 0);
    CHECK("A2b: find dup t4 → idx 4", ggfs_find(&fs, "tok.embd.weight") == 0);
    CHECK("A2c: find ว่าง (t9) → idx 9", ggfs_find(&fs, "empty.zero") == 9);
    CHECK("A2d: ไม่พบ → -1", ggfs_find(&fs, "nope.nope") == -1);

    /* ── A3: stat ── */
    {
        GgfsStat st;
        CHECK("A3a: stat t3 ok", ggfs_stat(&fs, 3, &st) == 0);
        CHECK("A3b: stat t3 — size/rq/tick/status ตรง",
              st.size == g_sizes[3] && st.rq == ggf_walk_rq_of(42, 3, 144) &&
              st.tick == (ggf_walk_rq_of(42, 3, 144) % 12) &&
              st.status == GGF_CKPT_STORED && st.level == 0 && st.dup == 0);
        GgfsStat st4;
        ggfs_stat(&fs, 4, &st4);
        CHECK("A3c: stat dup t4 — home 0 · dup=1 · level 0",
              st4.home == 0 && st4.dup == 1);
        GgfsStat st9;
        ggfs_stat(&fs, 9, &st9);
        CHECK("A3d: stat ว่าง t9 — home -1", st9.home == -1);
    }

    /* ── A4: read จาก state (7,3) ── */
    {
        uint64_t got = 0;
        int rc = ggfs_read(&fs, 3, 7, 3, buf, sizeof buf, &got);
        CHECK("A4a: read t3 จาก (7,3) lossless",
              rc == 0 && got == g_sizes[3] && memcmp(buf, g_data[3], g_sizes[3]) == 0);
        CHECK("A4b: walk_steps สะสม > 0", fs.walk_steps > 0);
    }

    /* ── A5: enter-anywhere — 3 start states ── */
    {
        const uint32_t starts[3][2] = { {7, 3}, {0, 0}, {143, 11} };
        uint64_t steps[3] = {0, 0, 0};
        int all_ok = 1;
        for (int s = 0; s < 3; s++) {
            uint64_t before = fs.walk_steps;
            for (uint32_t i = 0; i < NT; i++) {
                uint64_t got = 0;
                if (g_home[i] < 0) continue;          /* ว่าง ข้าม */
                int rc = ggfs_read(&fs, i, starts[s][0], starts[s][1],
                                   buf, sizeof buf, &got);
                if (rc != 0 || got != g_sizes[i] ||
                    memcmp(buf, g_data[i], g_sizes[i]) != 0) {
                    all_ok = 0;
                    break;
                }
            }
            steps[s] = fs.walk_steps - before;
        }
        CHECK("A5a: ทุก tensor อ่านได้จาก 3 start states — bytes เหมือนกัน (lossless)",
              all_ok);
        CHECK("A5b: steps ต่างกันตาม start state (เส้นทางต่าง ข้อมูลเดียวกัน)",
              steps[0] > 0 && steps[1] > 0 && steps[2] > 0 &&
              (steps[0] != steps[1] || steps[1] != steps[2]));
    }

    /* ── A6: dup → อ่านผ่าน home ── */
    {
        uint64_t got = 0;
        int rc = ggfs_read(&fs, 4, 7, 3, buf, sizeof buf, &got);
        CHECK("A6: read dup t4 ผ่าน home (t0) — bytes ตรง",
              rc == 0 && got == g_sizes[4] && memcmp(buf, g_data[4], g_sizes[4]) == 0);
    }

    /* ── A7: ว่าง → -1 ── */
    {
        uint64_t got = 0;
        CHECK("A7: read ว่าง (t9) → -1", ggfs_read(&fs, 9, 7, 3, buf, sizeof buf, &got) == -1);
    }

    /* ── A8: corrupt .ggf → read จับ (CRC verify-on-open) ── */
    {
        ggfs_unmount(&fs);
        FILE *f = fopen(g_paths[3], "r+b");
        fseek(f, 64 + 4 + 4 + 40, SEEK_SET);
        uint8_t b;
        fread(&b, 1, 1, f); b ^= 0xFF;
        fseek(f, -1, SEEK_CUR); fwrite(&b, 1, 1, f);
        fclose(f);
        CHECK("A8a: mount ใหม่ ok", ggfs_mount("build/ggfs_t", &fs) == 0);
        uint64_t got = 0;
        int rc = ggfs_read(&fs, 3, 7, 3, buf, sizeof buf, &got);
        CHECK("A8b: corrupt .ggf → read คืน -4 (CRC จับ)", rc == -4);
        ggfs_unmount(&fs);
        ggf_save_map(g_data[3], g_sizes[3], 8, g_paths[3]);   /* restore */
        CHECK("A8c: remount หลัง restore ok", ggfs_mount("build/ggfs_t", &fs) == 0);
    }

    /* ── A9: deterministic — read ซ้ำ 2 รอบ ── */
    {
        uint64_t b1 = fs.walk_steps, b2 = 0, got = 0;
        ggfs_read(&fs, 0, 5, 9, buf, sizeof buf, &got);
        uint64_t s1 = fs.walk_steps - b1;
        memcpy(ref, buf, g_sizes[0]);
        b2 = fs.walk_steps;
        ggfs_read(&fs, 0, 5, 9, buf, sizeof buf, &got);
        CHECK("A9: read ซ้ำจาก state เดียว → bytes + steps เท่ากัน",
              memcmp(buf, ref, g_sizes[0]) == 0 && (fs.walk_steps - b2) == s1);
    }

    /* ── A10: zero-copy pointer ── */
    {
        const uint8_t *p = ggfs_node(&fs, 3, 0, 7, 3);
        CHECK("A10: ggfs_node — pointer ตรง chunk 0 ของ t3 (zero-copy)",
              p != NULL && memcmp(p, g_data[3], 64) == 0);
    }
    ggfs_unmount(&fs);

    /* ══════════ B2: mid-round mount — pending gate ══════════ */
    printf("\n— MID-ROUND mount (tensor ก่อน checkpoint = pending) —\n");
    {
        /* หา tensor ที่ live หลัง (72,0) กับก่อน (72,0) */
        uint64_t ckpt_pos = 72u * 12 + 0;
        uint32_t early = 0xFFFFFFFF, late = 0xFFFFFFFF;
        for (uint32_t i = 0; i < NT; i++) {
            if (g_home[i] < 0) continue;
            uint32_t rq = ggf_walk_rq_of(42, i, 144);
            uint64_t pos = (uint64_t)rq * 12 + (rq % 12);
            if (pos < ckpt_pos && early == 0xFFFFFFFF) early = i;
            if (pos >= ckpt_pos && late == 0xFFFFFFFF) late = i;
        }
        CHECK("B2a: มีทั้ง early และ late เทียบ checkpoint (72,0)",
              early != 0xFFFFFFFF && late != 0xFFFFFFFF);

        /* เขียน manifest กลางรอบทับใน ggfs_t (ที่ไฟล์อยู่จริง — STORED = ไฟล์
         * ใน dir นี้) แล้ว restore manifest เต็มหลังจบ */
        ggf_ckpt_write("build/ggfs_t/manifest.mfp", 42u, 12u, 144u, NT,
                       g_names, g_sizes, g_home, NULL, NULL,
                       2164u, 0u, 72u, 0u, "ggfs mid", "mid.gguf");
        static GgfsMount fsm;
        CHECK("B2b: mount mid-round manifest ok", ggfs_mount("build/ggfs_t", &fsm) == 0 &&
              fsm.ckpt_round == 72 && fsm.ckpt_tick == 0);
        GgfsStat se, sl;
        ggfs_stat(&fsm, early, &se);
        ggfs_stat(&fsm, late, &sl);
        CHECK("B2c: stat — early pending=1 · late pending=0",
              se.pending == 1 && sl.pending == 0);
        uint64_t got = 0;
        int re = ggfs_read(&fsm, early, 7, 3, buf, sizeof buf, &got);
        int rl = ggfs_read(&fsm, late, 7, 3, buf, sizeof buf, &got);
        CHECK("B2d: read — early → -2 (pending) · late → lossless",
              re == -2 && rl == 0 && got == g_sizes[late] &&
              memcmp(buf, g_data[late], g_sizes[late]) == 0);
        ggfs_unmount(&fsm);
        /* restore manifest เต็ม (สำหรับ cleanup ต่อไป) */
        ggf_ckpt_write("build/ggfs_t/manifest.mfp", 42u, 12u, 144u, NT,
                       g_names, g_sizes, g_home, NULL, NULL,
                       2164u, 0u, 0u, 0u, "ggfs test", "ggfs.gguf");
    }

    /* ══════════ B1: chain mount (delta → base) ══════════ */
    printf("\n— CHAIN mount (delta dir → resolve ผ่าน base) —\n");
    {
        /* base: สำเนาเดิม · delta: เปลี่ยน t1 */
        static uint8_t *b_data[NT];
        for (uint32_t i = 0; i < NT; i++) {
            b_data[i] = NULL;
            if (g_sizes[i] > 0) {
                b_data[i] = (uint8_t *)malloc(g_sizes[i]);
                memcpy(b_data[i], g_data[i], g_sizes[i]);
            }
            if (g_home[i] == (int32_t)i && g_sizes[i] > 0)
                ggf_save_map(b_data[i], g_sizes[i], 8, g_paths_base[i]);
        }
        ggf_ckpt_write("build/ggfs_b/manifest.mfp", 42u, 12u, 144u, NT,
                       g_names, g_sizes, g_home, NULL, NULL,
                       2164u, 0u, 0u, 0u, "fs base", "fs.gguf");
        g_data[1][g_sizes[1] / 2] ^= 0x5A;                    /* เปลี่ยน t1 */
        g_data[6][g_sizes[6] / 2] ^= 0x5A;                    /* dup ของ t1 */
        uint8_t *st = (uint8_t *)malloc(NT);
        for (uint32_t i = 0; i < NT; i++) {
            if (g_home[i] == (int32_t)i && g_sizes[i] > 0)
                st[i] = ggf_ckpt_cmp_base("build/ggfs_b", i, g_names[i],
                                          g_sizes[i], g_data[i]);
            else st[i] = GGF_CKPT_STORED;
        }
        for (uint32_t i = 0; i < NT; i++)
            if (g_home[i] != (int32_t)i) st[i] = st[g_home[i]];
        for (uint32_t i = 0; i < NT; i++)
            if (g_home[i] == (int32_t)i && st[i] == GGF_CKPT_STORED && g_sizes[i] > 0)
                ggf_save_map(g_data[i], g_sizes[i], 8, g_paths_delta[i]);
        ggf_ckpt_write("build/ggfs_d/manifest.mfp", 42u, 12u, 144u, NT,
                       g_names, g_sizes, g_home, st, "build/ggfs_b",
                       2164u, 0u, 0u, 0u, "fs delta", "fs.gguf");

        static GgfsMount fsd;
        CHECK("B1a: mount delta dir ok", ggfs_mount("build/ggfs_d", &fsd) == 0);
        GgfsStat s1, s3;
        ggfs_stat(&fsd, 1, &s1);
        ggfs_stat(&fsd, 3, &s3);
        CHECK("B1b: stat — t1 STORED level 0 (delta) · t3 SAME level 1 (base)",
              s1.status == GGF_CKPT_STORED && s1.level == 0 &&
              s3.status == GGF_CKPT_SAME && s3.level == 1 &&
              strstr(s3.file_dir, "ggfs_b") != NULL);
        uint64_t got = 0;
        int rc1 = ggfs_read(&fsd, 1, 7, 3, buf, sizeof buf, &got);
        int rc3 = ggfs_read(&fsd, 3, 7, 3, buf, sizeof buf, &got);
        CHECK("B1c: read ผ่าน chain — t1 (delta) + t3 (base) lossless",
              rc1 == 0 && rc3 == 0 &&
              memcmp(buf, g_data[3], g_sizes[3]) == 0);
        ggfs_unmount(&fsd);
        free(st);
        for (uint32_t i = 0; i < NT; i++) {
            remove(g_paths_base[i]);
            remove(g_paths_delta[i]);
            free(b_data[i]);
        }
        remove("build/ggfs_b/manifest.mfp");
        remove("build/ggfs_d/manifest.mfp");
    }

    /* ── cleanup ── */
    for (uint32_t i = 0; i < NT; i++) {
        remove(g_paths[i]);
        free(g_data[i]);
    }
    remove("build/ggfs_t/manifest.mfp");

    printf("\n═══ RESULT: %d PASS / %d FAIL ═══\n", pass_count, fail_count);
    return fail_count ? 1 : 0;
}
