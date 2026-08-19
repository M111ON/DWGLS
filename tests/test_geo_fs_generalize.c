/* ═══════════════════════════════════════════════════════════════════════════
 * test_geo_fs_generalize.c — GeoFS Generalization benchmark (lossless)
 * ═══════════════════════════════════════════════════════════════════════════
 * ข้อ 2: พิสูจน์ generalization — GeoFS filesystem จริง + multi-scale views
 *
 *  Part A — GeoFS as a REAL filesystem:
 *    - directory tree (mkdir / ls / path_resolve)
 *    - real files in directories (parent_addr)
 *    - serialize → deserialize → project lossless (every byte)
 *
 *  Part B — Multi-scale views (N views จาก 1 copy):
 *    - ghost_lift ข้อมูล 1 copy ใน residual space
 *    - อ่านผ่าน geos_read_ghost ที่ walk_round (scale) ต่างกันหลายค่า
 *    - ทุก view ได้ bytes เดิม — lossless 100%
 *    - พิสูจน์ copy ไม่คูณ: rs.count == n_blocks ตลอด (ไม่ว่า view เท่าไหร่)
 *
 *  Build:
 *    gcc -O2 -Wall -Wextra -Wno-unused-parameter -Wno-format -I. -Icore -Icore/infra \
 *        -o build/test_geo_fs_generalize tests/test_geo_fs_generalize.c -lm
 * ═══════════════════════════════════════════════════════════════════════════ */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "geofs_core.h"

static int tests_passed = 0;
static int tests_failed = 0;

#define TEST(name) do { \
    printf("  TEST %2d: %-48s ", tests_passed + tests_failed + 1, name); \
    } while(0)

#define PASS() do { printf("PASS\n"); tests_passed++; } while(0)
#define FAIL(msg) do { printf("FAIL: %s\n", msg); tests_failed++; } while(0)

/* deterministic pseudo-random fill (no libc rand dependency) */
static void fill_bytes(uint8_t *buf, uint32_t n, uint32_t seed) {
    uint32_t x = seed * 2654435761u + 1u;
    for (uint32_t i = 0; i < n; i++) {
        x ^= x << 13; x ^= x >> 17; x ^= x << 5;
        buf[i] = (uint8_t)(x >> 24);
    }
}

/* ── Part A1: Directory tree + files in dirs ──────────────────────────── */

static void test_real_fs_tree(void) {
    TEST("filesystem จริง — mkdir tree + files in dirs");
    GeosVolume vol;
    geos_volume_init(&vol);
    GeosDirTable dt;
    geos_dir_table_init(&dt);

    /* tree: / (0) → models(1), docs(3); models → qwen(2), llama(4); docs → archive(5) */
    if (geos_mkdir(&vol, &dt, "models", 0) != 0) { FAIL("mkdir models"); geos_volume_free(&vol); return; }
    if (geos_mkdir(&vol, &dt, "qwen", 1) != 0)   { FAIL("mkdir qwen");   geos_volume_free(&vol); return; }
    if (geos_mkdir(&vol, &dt, "docs", 0) != 0)   { FAIL("mkdir docs");   geos_volume_free(&vol); return; }
    if (geos_mkdir(&vol, &dt, "llama", 1) != 0)  { FAIL("mkdir llama");  geos_volume_free(&vol); return; }
    if (geos_mkdir(&vol, &dt, "archive", 3) != 0){ FAIL("mkdir archive");geos_volume_free(&vol); return; }
    if (dt.dir_count != 6) { FAIL("dir_count != 6 (root + 5)"); geos_volume_free(&vol); return; }

    /* files: readme@root(0), qwen_model@models(1), note@docs(3), old@archive(5) */
    uint8_t data[512];
    fill_bytes(data, sizeof(data), 7);
    GeosInode *f1 = geos_summon(&vol, "readme.txt", 200, data, 0, 0, 0);
    GeosInode *f2 = geos_summon(&vol, "qwen_model.bin", 512, data, 0, 1, 0);
    GeosInode *f3 = geos_summon(&vol, "note.md", 128, data, 0, 3, 0);
    GeosInode *f4 = geos_summon(&vol, "old.bin", 64, data, 0, 5, 0);
    if (!f1 || !f2 || !f3 || !f4) { FAIL("summon"); geos_volume_free(&vol); return; }
    f1->parent_addr = 0;  /* root */
    f2->parent_addr = 1;  /* models */
    f3->parent_addr = 3;  /* docs */
    f4->parent_addr = 5;  /* docs/archive */
    if (vol.n_files != 4) { FAIL("n_files != 4"); geos_volume_free(&vol); return; }

    /* ls root: 2 dirs (models, docs) */
    const char *names[16];
    int is_dirs[16];
    int n = geos_ls(&vol, &dt, 0, names, is_dirs, 16);
    if (n != 3) { FAIL("root ls != 3 entries"); geos_volume_free(&vol); return; }
    int dirs_seen = 0, file_seen = 0;
    for (int i = 0; i < n; i++) {
        if (is_dirs[i]) dirs_seen++;
        else if (strcmp(names[i], "readme.txt") == 0) file_seen++;
    }
    if (dirs_seen != 2 || file_seen != 1) { FAIL("root contents wrong"); geos_volume_free(&vol); return; }

    /* ls models: 2 dirs (qwen, llama) + 1 file (qwen_model.bin) */
    n = geos_ls(&vol, &dt, 1, names, is_dirs, 16);
    if (n != 3) { FAIL("models ls != 3"); geos_volume_free(&vol); return; }
    int md = 0, mf = 0;
    for (int i = 0; i < n; i++) { if (is_dirs[i]) md++; else mf++; }
    if (md != 2 || mf != 1) { FAIL("models contents wrong"); geos_volume_free(&vol); return; }

    /* ls docs: 1 dir (archive) + 1 file (note.md) */
    n = geos_ls(&vol, &dt, 3, names, is_dirs, 16);
    if (n != 2) { FAIL("docs ls != 2"); geos_volume_free(&vol); return; }

    /* ls archive: 1 file (old.bin) */
    n = geos_ls(&vol, &dt, 5, names, is_dirs, 16);
    if (n != 1 || is_dirs[0]) { FAIL("archive ls wrong"); geos_volume_free(&vol); return; }

    geos_volume_free(&vol);
    PASS();
}

/* ── Part A2: path_resolve + serialize→deserialize→project lossless ─── */

static void test_real_fs_persist(void) {
    TEST("filesystem จริง — path_resolve + persist lossless");
    GeosVolume vol;
    geos_volume_init(&vol);
    GeosDirTable dt;
    geos_dir_table_init(&dt);

    geos_mkdir(&vol, &dt, "data", 0);

    uint8_t src[1024];
    fill_bytes(src, sizeof(src), 99);
    geos_summon(&vol, "payload.bin", 1024, src, 0, 1, 0);

    /* path_resolve */
    uint32_t idx = 0xFFFFFFFF;
    int rc = geos_path_resolve(&vol, &dt, "payload.bin", &idx);
    if (rc != 0 || idx == 0xFFFFFFFF) { FAIL("path_resolve"); geos_volume_free(&vol); return; }
    if (strcmp(vol.inodes[idx].name, "payload.bin") != 0) { FAIL("resolve name"); geos_volume_free(&vol); return; }

    /* serialize */
    if (geos_serialize(&vol, "build/test_geo_fs_generalize.geofs") != 0) {
        FAIL("serialize"); geos_volume_free(&vol); return;
    }
    geos_volume_free(&vol);

    /* deserialize fresh */
    GeosVolume vol2;
    memset(&vol2, 0, sizeof(vol2));
    if (geos_deserialize(&vol2, "build/test_geo_fs_generalize.geofs") != 0) {
        FAIL("deserialize"); return;
    }
    if (vol2.inode_count != 1) { FAIL("inode_count after deser"); geos_volume_free(&vol2); return; }

    /* project + lossless byte compare */
    uint32_t size = 0;
    const uint8_t *ptr = geos_project_by_name(&vol2, "payload.bin", &size);
    if (!ptr || size != 1024) { FAIL("project"); geos_volume_free(&vol2); return; }
    if (memcmp(ptr, src, 1024) != 0) { FAIL("data mismatch after persist"); geos_volume_free(&vol2); return; }

    geos_volume_free(&vol2);
    PASS();
}

/* ── Part B1: multi-scale views — 1 copy, N scales, lossless ─────────── */

static void test_multiscale_views(void) {
    TEST("multi-scale views — ghost 1 copy, N scales lossless");
    GeosVolume vol;
    geos_volume_init(&vol);

    /* 4 blocks = 256 bytes */
    uint8_t src[256];
    fill_bytes(src, sizeof(src), 42);
    GeosInode *inode = geos_summon(&vol, "view.bin", 256, src, 0, 0, 0);
    if (!inode || inode->block_count != 4) { FAIL("summon blocks"); geos_volume_free(&vol); return; }

    /* ghost lift: 1 copy ต่อ block → residual space */
    GhostLog log; ghost_log_init(&log);
    ResidualSpace rs; rs_init(&rs, 32);

    uint8_t block[GEOS_BLOCK_SZ];
    for (uint32_t b = 0; b < inode->block_count; b++) {
        uint32_t blk = inode->block_start + b;
        memcpy(block, &vol.data[blk * GEOS_BLOCK_SZ], GEOS_BLOCK_SZ);
        uint64_t bk = ghost_lift(&log, &rs, (uint16_t)blk, 0, cap_rule_scale(blk),
                                 block, GEOS_BLOCK_SZ);
        if (bk == RS_BOND_KEY_RESERVED) { FAIL("ghost_lift"); geos_volume_free(&vol); rs_free(&rs); return; }
    }
    if (rs.count != inode->block_count) {
        FAIL("residual count != blocks (copy multiplied?)");
        geos_volume_free(&vol); rs_free(&rs); return;
    }

    /* อ่านที่หลาย scale (walk_round ต่างกัน) → lossless ทุก view */
    const uint32_t scales[] = { 0, 7, 17, 36, 59, 83, 101, 119, 143, 71 };
    for (size_t s = 0; s < sizeof(scales)/sizeof(scales[0]); s++) {
        GeosVolume v2 = vol;                    /* copy state, data store เดียวกัน */
        v2.walk_round = scales[s];
        v2.walk_tick  = scales[s] % 12u;

        uint8_t out[256];
        int got = geos_read_ghost(&v2, &log, &rs, "view.bin", out, sizeof(out));
        if (got != 256) {
            char msg[64]; snprintf(msg, sizeof(msg), "read_ghost scale %u (got %d)", scales[s], got);
            FAIL(msg); geos_volume_free(&vol); rs_free(&rs); return;
        }
        if (memcmp(out, src, 256) != 0) {
            char msg[64]; snprintf(msg, sizeof(msg), "lossless fail scale %u", scales[s]);
            FAIL(msg); geos_volume_free(&vol); rs_free(&rs); return;
        }
    }

    /* N views จาก 1 copy — copy ยังไม่คูณ (ยัง = n_blocks) */
    if (rs.count != inode->block_count) { FAIL("copy multiplied after N views"); geos_volume_free(&vol); rs_free(&rs); return; }

    geos_volume_free(&vol);
    rs_free(&rs);
    PASS();
}

/* ── Part B2: read_ghost vs project ตรงกัน (fallback + ghost 2 path) ─── */

static void test_ghost_vs_project(void) {
    TEST("ghost path == project path (2 อ่านทาง, bytes เดียวกัน)");
    GeosVolume vol;
    geos_volume_init(&vol);

    uint8_t src[192];
    fill_bytes(src, sizeof(src), 3);
    geos_summon(&vol, "cmp.bin", 192, src, 0, 0, 0);

    /* ghost path — lift แล้วอ่าน */
    GhostLog log; ghost_log_init(&log);
    ResidualSpace rs; rs_init(&rs, 16);
    GeosInode *inode = geos_find(&vol, "cmp.bin");
    uint8_t block[GEOS_BLOCK_SZ];
    for (uint32_t b = 0; b < inode->block_count; b++) {
        uint32_t blk = inode->block_start + b;
        memcpy(block, &vol.data[blk * GEOS_BLOCK_SZ], GEOS_BLOCK_SZ);
        ghost_lift(&log, &rs, (uint16_t)blk, 0, cap_rule_scale(blk), block, GEOS_BLOCK_SZ);
    }

    GeosVolume v2 = vol;
    v2.walk_round = 143; v2.walk_tick = 11;
    uint8_t ghost_out[192];
    int got = geos_read_ghost(&v2, &log, &rs, "cmp.bin", ghost_out, sizeof(ghost_out));

    uint32_t psize = 0;
    const uint8_t *pp = geos_project_by_name(&vol, "cmp.bin", &psize);

    if (got != 192 || psize != 192) { FAIL("read sizes"); geos_volume_free(&vol); rs_free(&rs); return; }
    if (memcmp(ghost_out, pp, 192) != 0) { FAIL("ghost != project"); geos_volume_free(&vol); rs_free(&rs); return; }

    geos_volume_free(&vol);
    rs_free(&rs);
    PASS();
}

/* ── Part B3: walk evidence — ต่าง scale = เดินไกลต่างกัน ─────────────── */

static void test_walk_evidence(void) {
    TEST("walk evidence — scale ต่างกัน steps ต่างกัน (ไม่ใช่ shortcut)");
    GeosVolume vol;
    geos_volume_init(&vol);

    uint8_t src[64];
    fill_bytes(src, sizeof(src), 5);
    geos_summon(&vol, "w.bin", 64, src, 0, 0, 0);

    GhostLog log; ghost_log_init(&log);
    ResidualSpace rs; rs_init(&rs, 8);
    GeosInode *inode = geos_find(&vol, "w.bin");
    uint32_t blk = inode->block_start;
    uint8_t block[GEOS_BLOCK_SZ];
    memcpy(block, &vol.data[blk * GEOS_BLOCK_SZ], GEOS_BLOCK_SZ);
    ghost_lift(&log, &rs, (uint16_t)blk, 0, cap_rule_scale(blk), block, GEOS_BLOCK_SZ);

    uint64_t s0 = 0, s1 = 0;
    uint8_t out[64];
    GeosVolume a = vol; a.walk_round = 0; a.walk_tick = 0;
    geos_read_ghost(&a, &log, &rs, "w.bin", out, 64);
    s0 = a.walk_steps;

    GeosVolume b = vol; b.walk_round = 90; b.walk_tick = 90 % 12;
    geos_read_ghost(&b, &log, &rs, "w.bin", out, 64);
    s1 = b.walk_steps;

    if (s0 == 0 || s1 == 0) { FAIL("no walk steps"); geos_volume_free(&vol); rs_free(&rs); return; }
    if (s0 == s1) { FAIL("steps identical across scales"); geos_volume_free(&vol); rs_free(&rs); return; }

    geos_volume_free(&vol);
    rs_free(&rs);
    PASS();
}

/* ═══════════════════════════════════════════════════════════════════════════
   MAIN
   ═══════════════════════════════════════════════════════════════════════════ */

int main(void) {
    printf("╔══════════════════════════════════════════════════════════╗\n");
    printf("║  GeoFS Generalization — filesystem จริง + multi-scale    ║\n");
    printf("║  views (N views จาก 1 copy) — lossless benchmark        ║\n");
    printf("╚══════════════════════════════════════════════════════════╝\n\n");

    test_real_fs_tree();
    test_real_fs_persist();
    test_multiscale_views();
    test_ghost_vs_project();
    test_walk_evidence();

    printf("\n───────────────────────────────────────\n");
    printf("PASS: %d / %d  FAIL: %d\n", tests_passed, tests_passed + tests_failed, tests_failed);
    printf("═══════════════════════════════════════\n");

    return tests_failed > 0 ? 1 : 0;
}