/* ═══════════════════════════════════════════════════════════════════════════
 * test_geo_fs_mdim.c — GeoFS Multidimensional Native Volume tests
 * ═══════════════════════════════════════════════════════════════════════════
 *  1  volume init
 *  2  four views — flat↔coords roundtrip over the whole space
 *  3  summon / read / verify (multi-slot files + empty file)
 *  4  same bytes reachable through every view
 *  5  name bonding — 200 files, stride probes stay short
 *  6  unsummon — probe chains survive deletion
 *  7  rewrite — same file, new bytes
 *  8  timeline versions — read_at(frame)
 *  9  crash recovery — uncommitted frame rolled back
 * 10  crash recovery — corrupt committed frame = fail-loud
 * 11  save → load roundtrip (journal survives)
 * 12  ring wrap — old frames evicted, newest readable
 * 13  mmap open / read / flush / close
 * 14  multi-frame run — 100 KB file, lossless across chunks
 * 15  arbitrary-size rewrite — shrink/grow/empty + versions + unsummon
 * 16  derived bitmap — orphaned chain blocks swept on rebuild
 * 17  crash sweep — every (frame × stage) kill point of multi-frame ops
 * 18  crash stress — 600 random mid-op kills, power-cycle each time
 * 19  torn writes — fail-loud CORRUPT vs benign slack corruption
 * 20  timeline — torn committed frames fail loud on every versioned read
 * ═══════════════════════════════════════════════════════════════════════════ */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdarg.h>

/* ── crash-simulation fault injection ─────────────────────────────────────
 * The volume calls MDIM_CRASH_HOOK(v, stage) at every journal stage
 * boundary of a mutation (1 = frame written, base untouched · 2 = base
 * mutated, frame uncommitted · 3 = frame committed). We snapshot the
 * volume bytes at the kill point; the op then continues (its final state
 * is ignored). The snapshot is the on-disk state after a power loss at
 * that exact instruction boundary: everything up to the last completed
 * byte write survived, RAM state (bitmap, counters, staged changes) is
 * gone — a reload derives it all from the bytes alone. */
static void mdim_crash_hook_impl(void *vp, int stage);
#define MDIM_CRASH_HOOK(v, stage) mdim_crash_hook_impl((v), (stage))
#include "geofs_mdim.h"

static uint8_t crash_snap[MDIM_VOL_BYTES];
static int    crash_armed = 0;      /* counting down to the kill point? */
static int    crash_countdown = 0;  /* hook calls remaining before the kill */
static int    crash_hits = 0;       /* snapshots taken (≤ 1 per armed op) */

static void mdim_crash_hook_impl(void *vp, int stage) {
    (void)stage;
    if (!crash_armed) return;
    if (crash_countdown > 0) { crash_countdown--; return; }
    memcpy(crash_snap, ((MdimVolume *)vp)->bytes, MDIM_VOL_BYTES);
    crash_armed = 0;               /* one kill per op */
    crash_hits++;
}

static int tests_passed = 0;
static int tests_failed = 0;

#define TEST(name) do { \
    printf("  TEST %2d: %-52s ", tests_passed + tests_failed + 1, name); \
    } while(0)
#define PASS() do { printf("✅ PASS\n"); tests_passed++; } while(0)
#define FAIL(msg) do { printf("❌ FAIL: %s\n", msg); tests_failed++; } while(0)

#define MDIM_TEST_VOL "build/test_mdim.geofs"

/* ── 1 ─────────────────────────────────────────────────────────────────── */

static void test_volume_init(void) {
    TEST("volume init — super + regions");
    MdimVolume v;
    if (mdim_volume_init(&v, NULL) != MDIM_OK) { FAIL("init"); return; }
    if (memcmp(v.bytes, "MDIM", 4) != 0) { FAIL("magic"); mdim_volume_free(&v); return; }
    if (v.bytes[0] != 'M') { FAIL("super marker"); mdim_volume_free(&v); return; }
    if (mdim_slot(&v, MDIM_JRNL_START)->type != MDIM_T_JRNL) { FAIL("journal slot"); mdim_volume_free(&v); return; }
    if (mdim_last_frame(&v) != MDIM_FRAME_NONE) { FAIL("no frames yet"); mdim_volume_free(&v); return; }
    if (v.frame_counter != 1) { FAIL("frame counter"); mdim_volume_free(&v); return; }
    MdimStats st = mdim_stats(&v);
    if (st.blocks_free != MDIM_SLOTS - MDIM_DATA_START) { FAIL("free blocks"); mdim_volume_free(&v); return; }
    if (sizeof(MdimSlot) != MDIM_SLOT_SZ) { FAIL("slot must be 64B"); mdim_volume_free(&v); return; }
    mdim_volume_free(&v);
    PASS();
}

/* ── 2 ─────────────────────────────────────────────────────────────────── */

static void test_view_roundtrips(void) {
    TEST("four views — flat↔coords roundtrip (20736)");
    MdimVolume v;
    mdim_volume_init(&v, NULL);

    for (int view = 0; view <= MDIM_VIEW_CELL; view++) {
        uint32_t space = mdim_view_space((MdimView)view);
        for (uint32_t flat = 0; flat < space; flat++) {
            uint32_t a, b, c;
            mdim_view_coords((MdimView)view, flat, &a, &b, &c);
            uint32_t back = mdim_view_flat((MdimView)view, a, b, c);
            if (back != flat) {
                char msg[64];
                snprintf(msg, sizeof(msg), "%s %u -> %u", mdim_view_name((MdimView)view), flat, back);
                FAIL(msg);
                mdim_volume_free(&v);
                return;
            }
        }
    }
    /* known anchors */
    if (mdim_view_flat(MDIM_VIEW_RAIL, 5, 100, 0) != 5 * 1728u + 100) { FAIL("rail anchor"); mdim_volume_free(&v); return; }
    if (mdim_view_flat(MDIM_VIEW_CELL, 7, 99, 0) != 7 * 144u + 99) { FAIL("cell anchor"); mdim_volume_free(&v); return; }
    if (mdim_view_flat(MDIM_VIEW_TIME, 1, 0, 0) != 37u) { FAIL("time anchor"); mdim_volume_free(&v); return; }
    if (mdim_view_flat(MDIM_VIEW_CUBE, 2, 3, 5) != (2u | (3u << 3) | (5u << 6))) { FAIL("cube anchor"); mdim_volume_free(&v); return; }

    mdim_volume_free(&v);
    PASS();
}

/* ── 3 ─────────────────────────────────────────────────────────────────── */

static void test_summon_read(void) {
    TEST("summon / read / verify — multi-slot + empty");
    MdimVolume v;
    mdim_volume_init(&v, NULL);

    uint8_t small[2] = { 0xAB, 0xCD };
    uint8_t med[300];
    uint8_t big[1500];
    for (int i = 0; i < 300; i++) med[i] = (uint8_t)(i * 7 + 1);
    for (int i = 0; i < 1500; i++) big[i] = (uint8_t)(i * 13 + 5);

    int err = MDIM_OK;
    if (mdim_summon(&v, "small.bin", small, 2, &err) == MDIM_FRAME_NONE) { FAIL("summon small"); mdim_volume_free(&v); return; }
    if (mdim_summon(&v, "medium.bin", med, 300, &err) == MDIM_FRAME_NONE) { FAIL("summon med"); mdim_volume_free(&v); return; }
    if (mdim_summon(&v, "large.bin", big, 1500, &err) == MDIM_FRAME_NONE) { FAIL("summon big"); mdim_volume_free(&v); return; }
    if (mdim_summon(&v, "empty.bin", NULL, 0, &err) == MDIM_FRAME_NONE) { FAIL("summon empty"); mdim_volume_free(&v); return; }

    /* duplicate name must fail */
    if (mdim_summon(&v, "small.bin", small, 2, &err) != MDIM_FRAME_NONE || err != MDIM_ERR_EXISTS) {
        FAIL("duplicate must fail"); mdim_volume_free(&v); return;
    }

    uint8_t buf[1600];
    uint32_t actual = 0;
    MdimFile f;
    if (mdim_open(&v, "small.bin", &f) != MDIM_OK) { FAIL("open small"); mdim_volume_free(&v); return; }
    mdim_read(&v, &f, buf, sizeof(buf), &actual);
    if (actual != 2 || buf[0] != 0xAB || buf[1] != 0xCD) { FAIL("small bytes"); mdim_volume_free(&v); return; }

    if (mdim_open(&v, "medium.bin", &f) != MDIM_OK) { FAIL("open med"); mdim_volume_free(&v); return; }
    mdim_read(&v, &f, buf, sizeof(buf), &actual);
    if (actual != 300 || memcmp(buf, med, 300) != 0) { FAIL("med bytes"); mdim_volume_free(&v); return; }

    if (mdim_open(&v, "large.bin", &f) != MDIM_OK) { FAIL("open big"); mdim_volume_free(&v); return; }
    mdim_read(&v, &f, buf, sizeof(buf), &actual);
    if (actual != 1500 || memcmp(buf, big, 1500) != 0) { FAIL("big bytes"); mdim_volume_free(&v); return; }

    if (mdim_open(&v, "empty.bin", &f) != MDIM_OK) { FAIL("open empty"); mdim_volume_free(&v); return; }
    mdim_read(&v, &f, buf, sizeof(buf), &actual);
    if (actual != 0) { FAIL("empty bytes"); mdim_volume_free(&v); return; }

    if (mdim_verify(&v, "large.bin") != MDIM_OK) { FAIL("verify large"); mdim_volume_free(&v); return; }
    if (mdim_verify(&v, "medium.bin") != MDIM_OK) { FAIL("verify med"); mdim_volume_free(&v); return; }

    MdimStats st = mdim_stats(&v);
    if (st.n_files != 4) { FAIL("n_files"); mdim_volume_free(&v); return; }
    /* 4 entries + 4 run-links + data slots (1 + 5 + 24 + 0) = 38 used blocks */
    if (st.n_blocks_used != 4 + 4 + (1 + 5 + 24 + 0)) { FAIL("block accounting"); mdim_volume_free(&v); return; }

    mdim_volume_free(&v);
    PASS();
}

/* ── 4 ─────────────────────────────────────────────────────────────────── */

static void test_read_through_views(void) {
    TEST("same bytes reachable through all four views");
    MdimVolume v;
    mdim_volume_init(&v, NULL);

    uint8_t data[300];
    for (int i = 0; i < 300; i++) data[i] = (uint8_t)(i * 31 + 7);
    int err = MDIM_OK;
    if (mdim_summon(&v, "view.bin", data, 300, &err) == MDIM_FRAME_NONE) { FAIL("summon"); mdim_volume_free(&v); return; }

    MdimFile f;
    if (mdim_open(&v, "view.bin", &f) != MDIM_OK) { FAIL("open"); mdim_volume_free(&v); return; }

    uint8_t raw[64], via[64];
    uint32_t n_data = mdim_data_slots(300);
    for (int view = 0; view <= MDIM_VIEW_CELL; view++) {
        for (uint32_t k = 0; k < n_data; k++) {
            /* f.run_start = first run's LINK slot; data slots follow it */
            uint32_t flat = f.run_start + 1 + k;
            uint32_t a, b, c;
            mdim_view_coords((MdimView)view, flat, &a, &b, &c);
            if (mdim_view_read(&v, (MdimView)view, a, b, c, via, 64) != MDIM_OK) {
                FAIL("view read failed"); mdim_volume_free(&v); return;
            }
            memcpy(raw, &v.bytes[flat * MDIM_SLOT_SZ], 64);
            if (memcmp(raw, via, 64) != 0) {
                FAIL("view bytes differ"); mdim_volume_free(&v); return;
            }
            /* data payload must match the original file bytes */
            uint32_t chunk = 63;
            if (chunk > 300 - k * 63) chunk = 300 - k * 63;
            if (memcmp(via + 1, data + k * 63, chunk) != 0) {
                FAIL("data through view differs"); mdim_volume_free(&v); return;
            }
        }
    }
    mdim_volume_free(&v);
    PASS();
}

/* ── 5 ─────────────────────────────────────────────────────────────────── */

static void test_name_probe(void) {
    TEST("name bonding — 200 files, probes stay short");
    MdimVolume v;
    mdim_volume_init(&v, NULL);

    char name[24];
    uint8_t payload[3];
    for (int i = 0; i < 200; i++) {
        snprintf(name, sizeof(name), "probe%03d", i);
        payload[0] = (uint8_t)i; payload[1] = (uint8_t)(i >> 8); payload[2] = (uint8_t)(i >> 16);
        int err = MDIM_OK;
        if (mdim_summon(&v, name, payload, 3, &err) == MDIM_FRAME_NONE) {
            char msg[64]; snprintf(msg, sizeof(msg), "summon %d err=%d", i, err);
            FAIL(msg); mdim_volume_free(&v); return;
        }
    }

    uint8_t buf[4];
    for (int i = 0; i < 200; i++) {
        snprintf(name, sizeof(name), "probe%03d", i);
        MdimFile f;
        if (mdim_open(&v, name, &f) != MDIM_OK) { FAIL("open probe"); mdim_volume_free(&v); return; }
        uint32_t actual = 0;
        mdim_read(&v, &f, buf, sizeof(buf), &actual);
        if (actual != 3 || buf[0] != (uint8_t)i) {
            char msg[64]; snprintf(msg, sizeof(msg), "probe %d bytes", i);
            FAIL(msg); mdim_volume_free(&v); return;
        }
    }

    MdimStats st = mdim_stats(&v);
    if (st.n_files != 200) { FAIL("n_files"); mdim_volume_free(&v); return; }
    if (st.max_probe > 30) {
        char msg[64]; snprintf(msg, sizeof(msg), "max probe %u too long", st.max_probe);
        FAIL(msg); mdim_volume_free(&v); return;
    }
    mdim_volume_free(&v);
    PASS();
}

/* ── 6 ─────────────────────────────────────────────────────────────────── */

static void test_unsummon_chain(void) {
    TEST("unsummon — probe chains survive deletion");
    MdimVolume v;
    mdim_volume_init(&v, NULL);

    uint8_t a[3] = { 1, 2, 3 };
    uint8_t b[3] = { 4, 5, 6 };
    uint8_t c[3] = { 7, 8, 9 };
    int err = MDIM_OK;
    mdim_summon(&v, "alpha", a, 3, &err);
    mdim_summon(&v, "beta", b, 3, &err);
    mdim_summon(&v, "gamma", c, 3, &err);
    mdim_summon(&v, "delta", c, 3, &err);

    if (mdim_unsummon(&v, "beta") != MDIM_OK) { FAIL("unsummon beta"); mdim_volume_free(&v); return; }
    if (mdim_open(&v, "beta", NULL) != MDIM_ERR_NOENT) { FAIL("beta gone"); mdim_volume_free(&v); return; }

    /* re-summon into the freed chain slot */
    uint8_t d[3] = { 9, 9, 9 };
    if (mdim_summon(&v, "beta2", d, 3, &err) == MDIM_FRAME_NONE) { FAIL("beta2 summon"); mdim_volume_free(&v); return; }

    uint8_t buf[4];
    MdimFile f;
    uint32_t actual;
    mdim_open(&v, "alpha", &f); mdim_read(&v, &f, buf, 4, &actual);
    if (memcmp(buf, a, 3) != 0) { FAIL("alpha intact"); mdim_volume_free(&v); return; }
    mdim_open(&v, "gamma", &f); mdim_read(&v, &f, buf, 4, &actual);
    if (memcmp(buf, c, 3) != 0) { FAIL("gamma intact"); mdim_volume_free(&v); return; }
    mdim_open(&v, "delta", &f); mdim_read(&v, &f, buf, 4, &actual);
    if (memcmp(buf, c, 3) != 0) { FAIL("delta intact"); mdim_volume_free(&v); return; }
    mdim_open(&v, "beta2", &f); mdim_read(&v, &f, buf, 4, &actual);
    if (memcmp(buf, d, 3) != 0) { FAIL("beta2 bytes"); mdim_volume_free(&v); return; }

    if (mdim_unsummon(&v, "missing") != MDIM_ERR_NOENT) { FAIL("missing unsummon"); mdim_volume_free(&v); return; }
    mdim_volume_free(&v);
    PASS();
}

/* ── 7 ─────────────────────────────────────────────────────────────────── */

static void test_rewrite(void) {
    TEST("rewrite — same file, new bytes");
    MdimVolume v;
    mdim_volume_init(&v, NULL);

    uint8_t v1[100];
    uint8_t v2[50];
    for (int i = 0; i < 100; i++) v1[i] = (uint8_t)(i + 1);
    for (int i = 0; i < 50; i++) v2[i] = (uint8_t)(200 - i);

    int err = MDIM_OK;
    mdim_summon(&v, "rw.bin", v1, 100, &err);   /* frame 1 */
    if (mdim_write(&v, "rw.bin", v2, 50) != MDIM_OK) { FAIL("write"); mdim_volume_free(&v); return; }
    uint32_t f2 = mdim_last_frame(&v);          /* write = new chain + old free → frames 2,3 */

    uint8_t buf[120];
    uint32_t actual;
    MdimFile f;
    mdim_open(&v, "rw.bin", &f);
    mdim_read(&v, &f, buf, sizeof(buf), &actual);
    if (actual != 50 || memcmp(buf, v2, 50) != 0) { FAIL("new bytes"); mdim_volume_free(&v); return; }
    if (mdim_verify(&v, "rw.bin") != MDIM_OK) { FAIL("verify"); mdim_volume_free(&v); return; }

    /* grow is allowed now — full chain re-layout */
    if (mdim_write(&v, "rw.bin", v1, 100) != MDIM_OK) { FAIL("grow must succeed"); mdim_volume_free(&v); return; }
    mdim_open(&v, "rw.bin", &f);
    mdim_read(&v, &f, buf, sizeof(buf), &actual);
    if (actual != 100 || memcmp(buf, v1, 100) != 0) { FAIL("grown bytes"); mdim_volume_free(&v); return; }
    if (f2 != 3) { FAIL("commit frame"); mdim_volume_free(&v); return; }

    mdim_volume_free(&v);
    PASS();
}

/* ── 8 ─────────────────────────────────────────────────────────────────── */

static void test_timeline_versions(void) {
    TEST("timeline — versioned reads (file@frame)");
    MdimVolume v;
    mdim_volume_init(&v, NULL);

    const char *s1 = "version-one";
    const char *s2 = "version-two";
    int err = MDIM_OK;
    mdim_summon(&v, "doc.txt", (const uint8_t *)s1, (uint32_t)strlen(s1) + 1, &err);   /* frame 1 */
    mdim_write(&v, "doc.txt", (const uint8_t *)s2, (uint32_t)strlen(s2) + 1);           /* new chain + old free → frames 2,3 */
    uint32_t f2 = mdim_last_frame(&v);
    mdim_summon(&v, "other.bin", (const uint8_t *)"zz", 2, &err);                       /* frame 4 */

    uint8_t buf[64];
    uint32_t actual = 0;
    if (mdim_read_at(&v, "doc.txt", 1, buf, sizeof(buf), &actual) != MDIM_OK ||
        strcmp((const char *)buf, s1) != 0) { FAIL("doc@1"); mdim_volume_free(&v); return; }
    /* frames 2,3: entry switched at frame 2 → doc already holds v2 */
    if (mdim_read_at(&v, "doc.txt", 2, buf, sizeof(buf), &actual) != MDIM_OK ||
        strcmp((const char *)buf, s2) != 0) { FAIL("doc@2"); mdim_volume_free(&v); return; }
    if (mdim_read_at(&v, "doc.txt", 3, buf, sizeof(buf), &actual) != MDIM_OK ||
        strcmp((const char *)buf, s2) != 0) { FAIL("doc@3"); mdim_volume_free(&v); return; }

    /* other.bin did not exist at frame 2 or 3 */
    if (mdim_read_at(&v, "other.bin", 2, buf, sizeof(buf), &actual) != MDIM_ERR_NOENT) {
        FAIL("other@2 must be absent"); mdim_volume_free(&v); return;
    }
    if (mdim_read_at(&v, "other.bin", 3, buf, sizeof(buf), &actual) != MDIM_ERR_NOENT) {
        FAIL("other@3 must be absent"); mdim_volume_free(&v); return;
    }
    if (mdim_read_at(&v, "other.bin", 4, buf, sizeof(buf), &actual) != MDIM_OK ||
        buf[0] != 'z') { FAIL("other@4"); mdim_volume_free(&v); return; }

    /* full state snapshot at frame 1 must not contain other.bin */
    uint8_t *snap = (uint8_t *)malloc(MDIM_VOL_BYTES);
    if (mdim_state_at(&v, 1, snap) != MDIM_OK) { FAIL("state@1"); free(snap); mdim_volume_free(&v); return; }
    MdimVolume tmp; memset(&tmp, 0, sizeof(tmp)); tmp.bytes = snap;
    if (mdim_find_slot(&tmp, "other.bin", NULL) != MDIM_FRAME_NONE) {
        FAIL("other.bin present at frame 1"); free(snap); mdim_volume_free(&v); return;
    }
    free(snap);

    if (f2 != 3) { FAIL("frame numbering"); mdim_volume_free(&v); return; }
    mdim_volume_free(&v);
    PASS();
}


/* ── 9 ─────────────────────────────────────────────────────────────────── */

static void test_crash_recovery_open_frame(void) {
    TEST("crash recovery — uncommitted frame rolled back");
    MdimVolume v;
    mdim_volume_init(&v, NULL);

    const char *orig = "stable-content";
    int err = MDIM_OK;
    mdim_summon(&v, "stable.txt", (const uint8_t *)orig, (uint32_t)strlen(orig) + 1, &err);  /* frame 1 */

    /* stage a mutation via the write-ahead primitives, crash BEFORE commit */
    const char *intr = "intruder-data";
    MdimFile f;
    if (mdim_open(&v, "stable.txt", &f) != MDIM_OK) { FAIL("open"); mdim_volume_free(&v); return; }
    uint32_t link = f.run_start, d = mdim_data_slots(f.size);
    mdim_pending_begin(&v);
    mdim_pending_add(&v, f.entry);
    mdim_pending_add(&v, link);
    for (uint32_t i = 0; i < d; i++) mdim_pending_add(&v, link + 1 + i);
    mdim_frame_write(&v);
    /* apply the mutation to the base — then "crash" without committing */
    mdim_file_store_run(&v, link, (const uint8_t *)intr, (uint32_t)strlen(intr) + 1, 0);
    MdimSlot *es = mdim_slot(&v, f.entry);
    es->size = (uint32_t)strlen(intr) + 1;
    es->crc32 = mdim_crc32((const uint8_t *)intr, (uint32_t)strlen(intr) + 1);

    uint32_t rolled = 0;
    if (mdim_recover(&v, &rolled) != MDIM_OK) { FAIL("recover rc"); mdim_volume_free(&v); return; }
    if (rolled != 1) { FAIL("expected 1 rollback"); mdim_volume_free(&v); return; }

    uint8_t buf[64];
    uint32_t actual;
    mdim_open(&v, "stable.txt", &f);
    mdim_read(&v, &f, buf, sizeof(buf), &actual);
    if (strcmp((const char *)buf, orig) != 0) { FAIL("rolled back"); mdim_volume_free(&v); return; }

    /* volume is usable again */
    if (mdim_summon(&v, "post-crash.bin", (const uint8_t *)"ok", 2, &err) == MDIM_FRAME_NONE) {
        FAIL("post-recovery summon"); mdim_volume_free(&v); return;
    }
    mdim_volume_free(&v);
    PASS();
}

/* ── 10 ────────────────────────────────────────────────────────────────── */

static void test_crash_recovery_corrupt(void) {
    TEST("crash recovery — corrupt committed frame = fail-loud");
    MdimVolume v;
    mdim_volume_init(&v, NULL);

    uint8_t data[64];
    memset(data, 0x55, sizeof(data));
    int err = MDIM_OK;
    mdim_summon(&v, "good.bin", data, 64, &err);   /* frame 1 */

    uint32_t head = v.journal_head;
    if (head == MDIM_FRAME_NONE) { FAIL("no frame"); mdim_volume_free(&v); return; }
    /* flip a byte inside the frame's first change slot */
    v.bytes[(head + 2) * MDIM_SLOT_SZ + 10] ^= 0xFF;

    uint32_t rolled = 0;
    if (mdim_recover(&v, &rolled) != MDIM_ERR_CORRUPT) {
        FAIL("must be fail-loud CORRUPT"); mdim_volume_free(&v); return;
    }
    mdim_volume_free(&v);
    PASS();
}

/* ── 11 ────────────────────────────────────────────────────────────────── */

static void test_save_load_roundtrip(void) {
    TEST("save → load roundtrip (journal survives)");
    MdimVolume v;
    mdim_volume_init(&v, NULL);

    uint8_t big[1500];
    for (int i = 0; i < 1500; i++) big[i] = (uint8_t)(i * 3 + 9);
    int err = MDIM_OK;
    mdim_summon(&v, "persist.bin", big, 1500, &err);            /* frame 1 */
    mdim_summon(&v, "second.bin", (const uint8_t *)"two", 3, &err); /* frame 2 */
    mdim_write(&v, "persist.bin", big, 1500);                   /* auto-commit → frame 3 */
    uint32_t f3 = mdim_last_frame(&v);

    if (mdim_volume_save(&v, MDIM_TEST_VOL) != MDIM_OK) { FAIL("save"); mdim_volume_free(&v); return; }
    mdim_volume_free(&v);

    MdimVolume v2;
    if (mdim_volume_load(&v2, MDIM_TEST_VOL) != MDIM_OK) { FAIL("load"); return; }

    uint8_t buf[1600];
    uint32_t actual;
    MdimFile f;
    mdim_open(&v2, "persist.bin", &f);
    mdim_read(&v2, &f, buf, sizeof(buf), &actual);
    if (actual != 1500 || memcmp(buf, big, 1500) != 0) { FAIL("data after load"); mdim_volume_free(&v2); return; }
    if (mdim_verify(&v2, "persist.bin") != MDIM_OK) { FAIL("verify after load"); mdim_volume_free(&v2); return; }
    if (mdim_verify(&v2, "second.bin") != MDIM_OK) { FAIL("verify second"); mdim_volume_free(&v2); return; }

    /* journal (history) survives the roundtrip — frame 1 may have been
     * evicted by the rewrite's frames (ring = 128 slots), so accept either */
    int rc1 = mdim_read_at(&v2, "persist.bin", 1, buf, sizeof(buf), &actual);
    if (rc1 != MDIM_OK && rc1 != MDIM_ERR_EVICTED) {
        FAIL("read@1 after load"); mdim_volume_free(&v2); return;
    }
    if (mdim_last_frame(&v2) != f3) { FAIL("last frame after load"); mdim_volume_free(&v2); return; }

    mdim_volume_free(&v2);
    PASS();
}

/* ── 12 ────────────────────────────────────────────────────────────────── */

static void test_ring_wrap_eviction(void) {
    TEST("ring wrap — old frames evicted, newest readable");
    MdimVolume v;
    mdim_volume_init(&v, NULL);

    uint8_t one = 0;
    int err = MDIM_OK;
    mdim_summon(&v, "ev.bin", &one, 1, &err);              /* frame 1 */
    for (uint32_t i = 2; i <= 50; i++) {
        one = (uint8_t)i;
        /* each write = new chain frame + old-chain-free frame → 2 frames */
        if (mdim_write(&v, "ev.bin", &one, 1) != MDIM_OK) {
            FAIL("eviction write"); mdim_volume_free(&v); return;
        }
    }

    MdimStats st = mdim_stats(&v);
    if (st.last_frame != 1 + 49 * 2) { FAIL("last frame"); mdim_volume_free(&v); return; }
    if (st.checkpoint_frame == 0) { FAIL("expected eviction"); mdim_volume_free(&v); return; }

    uint8_t buf[4];
    uint32_t actual;
    /* frame 1 was evicted → fail-loud EVICTED */
    if (mdim_read_at(&v, "ev.bin", 1, buf, sizeof(buf), &actual) != MDIM_ERR_EVICTED) {
        FAIL("frame 1 must be evicted"); mdim_volume_free(&v); return;
    }
    /* newest frame is readable and holds the last byte */
    if (mdim_read_at(&v, "ev.bin", 99, buf, sizeof(buf), &actual) != MDIM_OK || buf[0] != 50) {
        FAIL("newest frame must read"); mdim_volume_free(&v); return;
    }
    if (buf[0] != 50) { FAIL("newest frame bytes"); mdim_volume_free(&v); return; }
    mdim_volume_free(&v);
    PASS();
}

/* ── 13 ────────────────────────────────────────────────────────────────── */

static void test_mmap_roundtrip(void) {
    TEST("mmap open — page-cache volume");
    MdimVolume v;
    mdim_volume_init(&v, NULL);

    uint8_t data[1000];
    for (int i = 0; i < 1000; i++) data[i] = (uint8_t)(i * 5);
    int err = MDIM_OK;
    mdim_summon(&v, "mmap.bin", data, 1000, &err);
    if (mdim_volume_save(&v, MDIM_TEST_VOL) != MDIM_OK) { FAIL("save"); mdim_volume_free(&v); return; }
    mdim_volume_free(&v);

    MdimVolume mv;
    if (mdim_volume_mmap_open(MDIM_TEST_VOL, &mv) != MDIM_OK) {
        FAIL("mmap open"); return;
    }
    uint8_t buf[1100];
    uint32_t actual;
    MdimFile f;
    if (mdim_open(&mv, "mmap.bin", &f) != MDIM_OK) { FAIL("mmap open file"); mdim_volume_free(&mv); return; }
    mdim_read(&mv, &f, buf, sizeof(buf), &actual);
    if (actual != 1000 || memcmp(buf, data, 1000) != 0) { FAIL("mmap data"); mdim_volume_free(&mv); return; }
    mdim_volume_mmap_flush(&mv);
    mdim_volume_free(&mv);
    PASS();
}

/* ── 14 ────────────────────────────────────────────────────────────────── */

static void test_multiframe_run(void) {
    TEST("multi-frame run — 100 KB file, lossless across chunks");
    MdimVolume v;
    mdim_volume_init(&v, NULL);

    uint32_t big_sz = 100 * 1024;   /* 102400 B — 26× the 3843 B single-frame cap */
    uint8_t *big = (uint8_t *)malloc(big_sz);
    for (uint32_t i = 0; i < big_sz; i++) big[i] = (uint8_t)(i * 31 + 7);

    int err = MDIM_OK;
    uint32_t e = mdim_summon(&v, "big.bin", big, big_sz, &err);
    if (e == MDIM_FRAME_NONE || err != MDIM_OK) {
        FAIL("summon big"); free(big); mdim_volume_free(&v); return;
    }

    MdimSlot *s = mdim_slot(&v, e);
    uint32_t expect_runs = mdim_n_runs(big_sz);
    if (s->n_runs != expect_runs || !(s->flags & MDIM_F_CHAIN)) {
        FAIL("run-span header"); free(big); mdim_volume_free(&v); return;
    }
    if (s->n_data_slots != mdim_data_slots(big_sz)) {
        FAIL("n_data_slots"); free(big); mdim_volume_free(&v); return;
    }

    uint8_t *back = (uint8_t *)malloc(big_sz);
    MdimFile f;
    uint32_t actual = 0;
    if (mdim_open(&v, "big.bin", &f) != MDIM_OK) { FAIL("open big"); free(back); free(big); mdim_volume_free(&v); return; }
    mdim_read(&v, &f, back, big_sz, &actual);
    if (actual != big_sz || memcmp(back, big, big_sz) != 0) {
        FAIL("lossless across chunks"); free(back); free(big); mdim_volume_free(&v); return;
    }
    if (mdim_verify(&v, "big.bin") != MDIM_OK) {
        FAIL("verify big"); free(back); free(big); mdim_volume_free(&v); return;
    }

    /* chain integrity: every LINK points to the next until the terminator */
    uint32_t link = s->prev, runs = 0;
    while (link != 0 && runs < MDIM_MAX_RUNS) {
        MdimSlot *ls = mdim_slot(&v, link);
        if (ls->type != MDIM_T_LINK) { FAIL("chain corrupt"); free(back); free(big); mdim_volume_free(&v); return; }
        link = ls->prev;
        runs++;
    }
    if (runs != expect_runs) { FAIL("chain length"); free(back); free(big); mdim_volume_free(&v); return; }

    /* accounting: 1 entry + n_runs links + n_data_slots data */
    MdimStats st = mdim_stats(&v);
    if (st.n_files != 1 || st.n_blocks_used != 1 + s->n_runs + s->n_data_slots) {
        FAIL("block accounting"); free(back); free(big); mdim_volume_free(&v); return;
    }

    free(back);
    free(big);
    mdim_volume_free(&v);
    PASS();
}

/* ── 15 ────────────────────────────────────────────────────────────────── */

static void test_multiframe_rewrite(void) {
    TEST("arbitrary-size rewrite — shrink/grow/empty + versions + unsummon");
    MdimVolume v;
    mdim_volume_init(&v, NULL);

    uint32_t sz1 = 104000;   /* 28 runs (> one frame) */
    uint32_t sz2 = 50 * 1024;/* 14 runs — different run-span */
    uint32_t sz3 = 100 * 1024;/* 28 runs — grow back */
    uint8_t *v1 = (uint8_t *)malloc(sz1);
    uint8_t *v2 = (uint8_t *)malloc(sz2);
    uint8_t *v3 = (uint8_t *)malloc(sz3);
    for (uint32_t i = 0; i < sz1; i++) v1[i] = (uint8_t)(i * 3 + 11);
    for (uint32_t i = 0; i < sz2; i++) v2[i] = (uint8_t)(i * 5 + 29);
    for (uint32_t i = 0; i < sz3; i++) v3[i] = (uint8_t)(i * 7 + 3);

    int err = MDIM_OK;
    uint32_t e = mdim_summon(&v, "big.bin", v1, sz1, &err);
    if (e == MDIM_FRAME_NONE) { FAIL("summon"); free(v1); free(v2); free(v3); mdim_volume_free(&v); return; }
    MdimSlot *s = mdim_slot(&v, e);
    if (s->n_runs != mdim_n_runs(sz1)) { FAIL("runs v1"); free(v1); free(v2); free(v3); mdim_volume_free(&v); return; }

    /* shrink across run boundaries: 28 runs → 14 runs */
    if (mdim_write(&v, "big.bin", v2, sz2) != MDIM_OK) { FAIL("shrink"); free(v1); free(v2); free(v3); mdim_volume_free(&v); return; }
    s = mdim_slot(&v, e);
    if (s->n_runs != mdim_n_runs(sz2)) { FAIL("shrink runs"); free(v1); free(v2); free(v3); mdim_volume_free(&v); return; }

    uint8_t *back = (uint8_t *)malloc(sz3);
    MdimFile f;
    uint32_t actual = 0;
    if (mdim_open(&v, "big.bin", &f) != MDIM_OK) { FAIL("open"); free(back); free(v1); free(v2); free(v3); mdim_volume_free(&v); return; }
    mdim_read(&v, &f, back, sz2, &actual);
    if (actual != sz2 || memcmp(back, v2, sz2) != 0) { FAIL("shrink bytes"); free(back); free(v1); free(v2); free(v3); mdim_volume_free(&v); return; }
    if (mdim_verify(&v, "big.bin") != MDIM_OK) { FAIL("verify after shrink"); free(back); free(v1); free(v2); free(v3); mdim_volume_free(&v); return; }

    /* grow back across run boundaries: 14 runs → 28 runs */
    if (mdim_write(&v, "big.bin", v3, sz3) != MDIM_OK) { FAIL("grow"); free(back); free(v1); free(v2); free(v3); mdim_volume_free(&v); return; }
    if (mdim_open(&v, "big.bin", &f) != MDIM_OK) { FAIL("reopen after grow"); free(back); free(v1); free(v2); free(v3); mdim_volume_free(&v); return; }
    mdim_read(&v, &f, back, sz3, &actual);
    if (actual != sz3 || memcmp(back, v3, sz3) != 0) { FAIL("grow bytes"); free(back); free(v1); free(v2); free(v3); mdim_volume_free(&v); return; }
    if (mdim_verify(&v, "big.bin") != MDIM_OK) { FAIL("verify after grow"); free(back); free(v1); free(v2); free(v3); mdim_volume_free(&v); return; }

    /* newest timeline frame holds the grown bytes */
    uint32_t last = mdim_last_frame(&v);
    if (mdim_read_at(&v, "big.bin", last, back, sz3, &actual) != MDIM_OK ||
        actual != sz3 || memcmp(back, v3, sz3) != 0) {
        FAIL("versioned read @last"); free(back); free(v1); free(v2); free(v3); mdim_volume_free(&v); return;
    }

    /* rewrite to empty */
    if (mdim_write(&v, "big.bin", NULL, 0) != MDIM_OK) { FAIL("to empty"); free(back); free(v1); free(v2); free(v3); mdim_volume_free(&v); return; }
    mdim_open(&v, "big.bin", &f);
    mdim_read(&v, &f, back, 1, &actual);
    if (actual != 0) { FAIL("empty read"); free(back); free(v1); free(v2); free(v3); mdim_volume_free(&v); return; }

    /* unsummon frees the whole chain */
    if (mdim_unsummon(&v, "big.bin") != MDIM_OK) { FAIL("unsummon"); free(back); free(v1); free(v2); free(v3); mdim_volume_free(&v); return; }
    if (mdim_open(&v, "big.bin", NULL) != MDIM_ERR_NOENT) { FAIL("gone"); free(back); free(v1); free(v2); free(v3); mdim_volume_free(&v); return; }
    MdimStats st = mdim_stats(&v);
    if (st.n_files != 0 || st.n_blocks_used != 0) { FAIL("blocks freed"); free(back); free(v1); free(v2); free(v3); mdim_volume_free(&v); return; }

    free(back); free(v1); free(v2); free(v3);
    mdim_volume_free(&v);
    PASS();
}

/* ── 16 ────────────────────────────────────────────────────────────────── */

static void test_orphan_sweep(void) {
    TEST("derived bitmap — orphaned chain blocks swept on rebuild");
    MdimVolume v;
    mdim_volume_init(&v, NULL);

    uint32_t sz = 100 * 1024;
    uint8_t *big = (uint8_t *)malloc(sz);
    for (uint32_t i = 0; i < sz; i++) big[i] = (uint8_t)(i * 7 + 3);

    int err = MDIM_OK;
    uint32_t e = mdim_summon(&v, "big.bin", big, sz, &err);
    if (e == MDIM_FRAME_NONE) { FAIL("summon"); free(big); mdim_volume_free(&v); return; }
    MdimSlot *s = mdim_slot(&v, e);
    if (s->n_runs <= 1) { FAIL("expected multi-run"); free(big); mdim_volume_free(&v); return; }

    /* simulate a crash that lost the entry (e.g. died before the last frame)
     * but left the chain committed — blocks reachable from no entry */
    memset(s, 0, sizeof(*s));
    s->type = MDIM_T_TOMB;

    /* load() does recover + rebuild — the sweep must free the orphans */
    mdim_recover(&v, NULL);
    mdim_rebuild(&v);
    MdimStats st = mdim_stats(&v);
    if (st.n_files != 0 || st.n_blocks_used != 0) {
        FAIL("sweep must free orphans"); free(big); mdim_volume_free(&v); return;
    }

    /* the swept space is immediately reusable */
    uint32_t e2 = mdim_summon(&v, "big2.bin", big, sz, &err);
    if (e2 == MDIM_FRAME_NONE) { FAIL("reuse after sweep"); free(big); mdim_volume_free(&v); return; }
    MdimSlot *s2 = mdim_slot(&v, e2);
    MdimStats st2 = mdim_stats(&v);
    if (st2.n_blocks_used != 1 + s2->n_runs + s2->n_data_slots) {
        FAIL("reuse accounting"); free(big); mdim_volume_free(&v); return;
    }

    free(big);
    mdim_volume_free(&v);
    PASS();
}

/* ═══════════════════════════════════════════════════════════════════════════
   CRASH-SIMULATION TESTS
   ═══════════════════════════════════════════════════════════════════════════
   A "kill" is a snapshot of the volume bytes at a journal stage boundary
   taken by MDIM_CRASH_HOOK. Loading the snapshot exercises the real
   recovery path (super → recover → rebuild) exactly as a power cycle
   would. The oracle — for ANY kill point inside the op:
     · summon    killed → target absent OR byte-exact (never torn)
     · rewrite   killed → target old OR new, byte-exact (never torn)
     · unsummon  killed → target present OR absent (never torn)
     · unrelated files always byte-identical
     · counters (n_files / n_blocks_used) and the derived bitmap always
       agree with the FILE entries present                               */

#define CRASH_MAX_MODEL 8
#define CRASH_OP_SUMMON   0
#define CRASH_OP_WRITE    1
#define CRASH_OP_UNSUMMON 2

/* op-history trace (defined later, used by crash_check) */
static void crash_trace_add(const char *fmt, ...);
static void crash_trace_dump(void);

typedef struct {
    char     name[MDIM_MAX_NAME];
    uint8_t *data;
    uint32_t size;
} CrashFile;

static CrashFile crash_model[CRASH_MAX_MODEL];
static int crash_model_n = 0;

static int crash_model_find(const char *name) {
    for (int i = 0; i < crash_model_n; i++)
        if (strncmp(crash_model[i].name, name, MDIM_MAX_NAME) == 0) return i;
    return -1;
}

static void crash_model_add(const char *name, const uint8_t *data, uint32_t size) {
    int i = crash_model_find(name);
    if (i < 0) {
        if (crash_model_n >= CRASH_MAX_MODEL) return;
        i = crash_model_n++;
    } else {
        free(crash_model[i].data);
    }
    strncpy(crash_model[i].name, name, MDIM_MAX_NAME - 1);
    crash_model[i].name[MDIM_MAX_NAME - 1] = 0;
    crash_model[i].size = size;
    crash_model[i].data = (uint8_t *)malloc(size ? size : 1);
    if (crash_model[i].data && size)
        memcpy(crash_model[i].data, data, size);
}

static void crash_model_remove(const char *name) {
    int i = crash_model_find(name);
    if (i < 0) return;
    free(crash_model[i].data);
    crash_model[i] = crash_model[crash_model_n - 1];
    crash_model_n--;
}

static void crash_model_free_all(void) {
    while (crash_model_n > 0) crash_model_remove(crash_model[0].name);
}

/* does the volume hold exactly these bytes under `name`? */
static int crash_file_matches(MdimVolume *v, const char *name,
                              const uint8_t *want, uint32_t want_sz) {
    MdimFile f;
    if (mdim_open(v, name, &f) != MDIM_OK) return 0;
    if (f.size != want_sz) return 0;
    uint8_t *buf = (uint8_t *)malloc(want_sz ? want_sz : 1);
    if (!buf) return 0;
    uint32_t actual = 0;
    mdim_read(v, &f, buf, want_sz, &actual);
    int ok = (actual == want_sz) && memcmp(buf, want, want_sz) == 0;
    free(buf);
    return ok;
}

/* fresh volume over the crash snapshot bytes — the same path as
 * mdim_volume_load minus the file IO: read super → recover → rebuild. */
static int crash_load_from_snap(MdimVolume *out) {
    memset(out, 0, sizeof(*out));
    out->bytes = (uint8_t *)malloc(MDIM_VOL_BYTES);
    if (!out->bytes) return MDIM_ERR_IO;
    out->owns_bytes = 1;
    memcpy(out->bytes, crash_snap, MDIM_VOL_BYTES);
    if (memcmp(out->bytes, "MDIM", 4) != 0 ||
        mdim_u16(out->bytes, MDIM_SUPER_VERSION) != 2) {
        free(out->bytes); out->bytes = NULL;
        return MDIM_ERR_CORRUPT;
    }
    out->journal_head = mdim_u32(out->bytes, MDIM_SUPER_JRNL_HEAD);
    out->checkpoint_frame = mdim_u32(out->bytes, MDIM_SUPER_CKPT);
    out->frame_counter = mdim_u32(out->bytes, MDIM_SUPER_FRAME);
    int rc = mdim_recover(out, NULL);
    if (rc != MDIM_OK) { free(out->bytes); out->bytes = NULL; return rc; }
    mdim_rebuild(out);
    return MDIM_OK;
}

/* arm one kill at hook call #countdown (0-indexed) */
static void crash_arm(int countdown) {
    crash_armed = 1;
    crash_countdown = countdown;
    crash_hits = 0;
}

/* expected hook counts — must match the op's actual frame staging */
static uint32_t crash_hooks_summon(uint32_t size)   { return mdim_n_runs(size) * 3u; }
static uint32_t crash_hooks_write(uint32_t old_sz, uint32_t new_sz) {
    return (mdim_n_runs(old_sz) + mdim_n_runs(new_sz)) * 3u;
}
static uint32_t crash_hooks_unsummon(uint32_t size) { return 3u + mdim_n_runs(size) * 3u; }

/* verify a recovered volume. Returns NULL when consistent, else a static
 * error string. old/new describe the op that was killed. */
static const char *crash_check(MdimVolume *r, int op_kind, const char *target,
                               const uint8_t *old_data, uint32_t old_sz,
                               const uint8_t *new_data, uint32_t new_sz) {
    /* 1) every unrelated model file must be byte-identical */
    for (int i = 0; i < crash_model_n; i++) {
        if (strncmp(crash_model[i].name, target, MDIM_MAX_NAME) == 0) continue;
        if (!crash_file_matches(r, crash_model[i].name, crash_model[i].data, crash_model[i].size))
            return "unrelated file corrupted by the crash";
    }
    /* 2) the killed op must resolve atomically — never a torn file */
    int present = mdim_open(r, target, NULL) == MDIM_OK;
    if (op_kind == CRASH_OP_SUMMON) {
        if (present && !crash_file_matches(r, target, new_data, new_sz))
            return "summon left a torn file";
    } else if (op_kind == CRASH_OP_WRITE) {
        int old_ok = crash_file_matches(r, target, old_data, old_sz);
        int new_ok = crash_file_matches(r, target, new_data, new_sz);
        if (!old_ok && !new_ok)
            return "rewrite left a torn file (neither old nor new)";
    } else {
        if (present && !crash_file_matches(r, target, old_data, old_sz))
            return "unsummon left a torn file";
    }
    /* 3) derived truth: counters + bitmap must agree with the entries */
    uint32_t entries = 0, expected_blocks = 0, pop = 0, rebuilt = r->n_blocks_used;
    uint32_t dbg_entry = 0, dbg_walk = 0, dbg_runs = 0;
    for (uint32_t i = MDIM_DATA_START; i < MDIM_SLOTS; i++) {
        MdimSlot *s = mdim_slot(r, i);
        if (s->type == MDIM_T_FILE && s->name[0]) {
            entries++;
            expected_blocks += 1 + s->n_runs + s->n_data_slots;
            if (r->n_blocks_used != expected_blocks && dbg_entry == 0) {
                dbg_entry = i;
                uint32_t link = s->prev, guard = 0;
                dbg_walk = 1; dbg_runs = 0;
                while (link != 0 && guard < MDIM_MAX_RUNS) {
                    MdimSlot *ls = mdim_slot(r, link);
                    dbg_walk += 1 + mdim_data_slots(ls->size ? ls->size : s->size);
                    dbg_runs++;
                    link = ls->prev;
                    guard++;
                }
            }
        }
        if (mdim_bit_get(r, i)) pop++;
    }
    if (entries != r->n_files) return "n_files counter mismatch";
    if (r->n_blocks_used != expected_blocks) {
        MdimSlot *es = mdim_slot(r, dbg_entry);
        MdimSlot *l = mdim_slot(r, es->prev);
        printf("\n[dbg] entry=%u \"%s\" size=%u n_runs=%u n_data=%u prev=%u "
               "claims=%u rebuild=%u target=%s\n",
               dbg_entry, es->name, es->size, es->n_runs, es->n_data_slots, es->prev,
               expected_blocks, rebuilt, target);
        printf("[dbg] link slot %u: type=%u flags=%u size=%u prev=%u\n",
               es->prev, l->type, l->flags, l->size, l->prev);
        printf("[dbg] super: head=%u ckpt=%u frame=%u n_files=%u n_used=%u\n",
               r->journal_head, r->checkpoint_frame, r->frame_counter,
               r->n_files, r->n_blocks_used);
        if (r->journal_head != MDIM_FRAME_NONE) {
            uint8_t *hh = mdim_frame_hdr(r, r->journal_head);
            printf("[dbg] head frame: no=%u flags=%u n=%u span=%u prev=%u\n",
                   mdim_u32(hh, MDIM_FH_FRAME_NO), hh[MDIM_FH_FLAGS],
                   mdim_frame_n_changes(hh), mdim_frame_span(mdim_frame_n_changes(hh)),
                   mdim_u32(hh, MDIM_FH_PREV));
            uint32_t cur = r->journal_head + mdim_frame_span(mdim_frame_n_changes(hh));
            if (cur < MDIM_JRNL_END) {
                uint8_t *th = mdim_frame_hdr(r, cur);
                printf("[dbg] tail frame @%u: magic=%u flags=%u n=%u\n",
                       cur, th[MDIM_FH_MAGIC], th[MDIM_FH_FLAGS],
                       mdim_frame_n_changes(th));
            } else {
                printf("[dbg] tail: cursor %u >= JRNL_END\n", cur);
            }
        }
        /* walk ALL entries with their chain state */
        for (uint32_t i = MDIM_DATA_START; i < MDIM_SLOTS; i++) {
            MdimSlot *s = mdim_slot(r, i);
            if (s->type == MDIM_T_FILE && s->name[0]) {
                uint32_t link = s->prev, n = 0;
                while (link != 0 && n < MDIM_MAX_RUNS) {
                    MdimSlot *ls = mdim_slot(r, link);
                    if (ls->type != MDIM_T_LINK) break;
                    link = ls->prev; n++;
                }
                printf("[dbg]   file %u \"%s\" prev=%u runs_ok=%u\n",
                       i, s->name, s->prev, n);
            }
        }
        crash_trace_dump();
        return "n_blocks_used mismatch";
    }
    if (pop != expected_blocks) return "bitmap/chain mismatch";
    return NULL;
}

/* ── 17 ────────────────────────────────────────────────────────────────── */

static void test_crash_sweep(void) {
    TEST("crash sweep — every (frame × stage) kill point, multi-frame ops");
    const uint32_t seed_sz  = 30 * 1024;    /* 8 runs — stays unrelated */
    const uint32_t big_sz   = 100 * 1024;   /* 28 runs */
    const uint32_t grow_sz  = 160 * 1024;   /* 44 runs */
    const uint32_t small_sz = 2000;         /* 1 run */
    uint8_t *seed  = (uint8_t *)malloc(seed_sz);
    uint8_t *big   = (uint8_t *)malloc(big_sz);
    uint8_t *grow  = (uint8_t *)malloc(grow_sz);
    uint8_t *small = (uint8_t *)malloc(small_sz);
    for (uint32_t i = 0; i < seed_sz; i++)  seed[i]  = (uint8_t)(i * 11 + 1);
    for (uint32_t i = 0; i < big_sz; i++)   big[i]   = (uint8_t)(i * 13 + 5);
    for (uint32_t i = 0; i < grow_sz; i++)  grow[i]  = (uint8_t)(i * 17 + 9);
    for (uint32_t i = 0; i < small_sz; i++) small[i] = (uint8_t)(i * 3 + 7);

    crash_model_n = 0;
    crash_model_add("seed.bin", seed, seed_sz);

    /* each row: the file state before the op, then the op's payload */
    struct { int kind; const char *name;
             const uint8_t *pre_data; uint32_t pre_sz;
             const uint8_t *op_data;  uint32_t op_sz; } plan[] = {
        { CRASH_OP_SUMMON,   "big.bin",  NULL,  0,        big,   big_sz },
        { CRASH_OP_WRITE,    "big.bin",  big,   big_sz,   grow,  grow_sz },
        { CRASH_OP_WRITE,    "big.bin",  grow,  grow_sz,  small, small_sz },
        { CRASH_OP_UNSUMMON, "big.bin",  small, small_sz, NULL,  0 },
        { CRASH_OP_SUMMON,   "sm.bin",   NULL,  0,        small, small_sz },
        { CRASH_OP_WRITE,    "sm.bin",   small, small_sz, small, 512 },
        { CRASH_OP_UNSUMMON, "sm.bin",   small, 512,      NULL,  0 },
    };

    uint32_t total_points = 0, checked = 0;
    int failed = 0;
    for (size_t p = 0; p < sizeof(plan) / sizeof(plan[0]) && !failed; p++) {
        int kind = plan[p].kind;
        const char *name = plan[p].name;
        uint32_t hooks = (kind == CRASH_OP_SUMMON)  ? crash_hooks_summon(plan[p].op_sz)
                       : (kind == CRASH_OP_WRITE)   ? crash_hooks_write(plan[p].pre_sz, plan[p].op_sz)
                       : crash_hooks_unsummon(plan[p].pre_sz);
        total_points += hooks;
        for (uint32_t t = 0; t < hooks && !failed; t++) {
            MdimVolume v;
            if (mdim_volume_init(&v, NULL) != MDIM_OK) { FAIL("init"); failed = 1; break; }
            int err = MDIM_OK;
            if (mdim_summon(&v, "seed.bin", seed, seed_sz, &err) == MDIM_FRAME_NONE) {
                FAIL("seed summon"); mdim_volume_free(&v); failed = 1; break;
            }
            if (plan[p].pre_data &&
                mdim_summon(&v, name, plan[p].pre_data, plan[p].pre_sz, &err) == MDIM_FRAME_NONE) {
                FAIL("setup summon"); mdim_volume_free(&v); failed = 1; break;
            }

            crash_arm((int)t);
            if (kind == CRASH_OP_SUMMON)
                mdim_summon(&v, name, plan[p].op_data, plan[p].op_sz, &err);
            else if (kind == CRASH_OP_WRITE)
                mdim_write(&v, name, plan[p].op_data, plan[p].op_sz);
            else
                mdim_unsummon(&v, name);
            if (crash_hits == 0) { mdim_volume_free(&v); continue; }  /* aborted early */


            MdimVolume r;
            int rc = crash_load_from_snap(&r);
            if (rc != MDIM_OK) {        /* a consistent kill must never fail load */
                char msg[80];
                snprintf(msg, sizeof(msg), "reload rc=%d at point %u", rc, t);
                FAIL(msg); mdim_volume_free(&v); failed = 1; break;
            }
            const char *msg = crash_check(&r, kind, name,
                                          plan[p].pre_data, plan[p].pre_sz,
                                          plan[p].op_data, plan[p].op_sz);
            if (msg) { FAIL(msg); mdim_volume_free(&r); mdim_volume_free(&v); failed = 1; break; }

            /* the recovered volume must be fully operational */
            if (mdim_summon(&r, "post.bin", small, 64, &err) == MDIM_FRAME_NONE ||
                !crash_file_matches(&r, "post.bin", small, 64) ||
                mdim_unsummon(&r, "post.bin") != MDIM_OK) {
                FAIL("recovered volume not operational");
                mdim_volume_free(&r); mdim_volume_free(&v); failed = 1; break;
            }
            mdim_volume_free(&r);
            mdim_volume_free(&v);
            checked++;
        }
    }
    free(seed); free(big); free(grow); free(small);
    crash_model_free_all();
    if (failed) return;
    if (checked != total_points) {
        char msg[80]; snprintf(msg, sizeof(msg), "%u/%u points checked", checked, total_points);
        FAIL(msg); return;
    }
    printf("                (%u kill points, all consistent)\n", checked);
    PASS();
}

/* ── 18 ────────────────────────────────────────────────────────────────── */

static uint32_t crash_rng_state = 0x9E3779B9u;
static uint32_t crash_rand(void) {           /* xorshift32 — deterministic */
    crash_rng_state ^= crash_rng_state << 13;
    crash_rng_state ^= crash_rng_state >> 17;
    crash_rng_state ^= crash_rng_state << 5;
    return crash_rng_state;
}

static const char *CRASH_MMAP_A = "build/test_mdim_crash_mmap.geofs";
static const char *CRASH_MMAP_B = "build/test_mdim_crash_mmap2.geofs";
static const char *CRASH_LOAD   = "build/test_mdim_crash_load.geofs";
static int crash_mmap_toggle = 0;

/* op trace for debugging (ring of recent ops) */
#define CRASH_TRACE_N 40
static char crash_trace[CRASH_TRACE_N][96];
static uint32_t crash_trace_pos = 0;
static void crash_trace_add(const char *fmt, ...) {
    char *dst = crash_trace[crash_trace_pos % CRASH_TRACE_N];
    va_list ap; va_start(ap, fmt);
    vsnprintf(dst, 96, fmt, ap);
    va_end(ap);
    crash_trace_pos++;
}
static void crash_trace_dump(void) {
    printf("[trc] pos=%u\n", crash_trace_pos);
    uint32_t start = crash_trace_pos > CRASH_TRACE_N ? crash_trace_pos - CRASH_TRACE_N : 0;
    for (uint32_t i = start; i < crash_trace_pos; i++)
        printf("[trc] %s\n", crash_trace[i % CRASH_TRACE_N]);
}

/* pick a random op against the model. Returns 0 with the op filled in
 * (caller owns *new_data), nonzero to skip. mix: 50% summon, 40% write,
 * 10% unsummon — summons are 50% small (1-run) so the crash population
 * can actually grow (a killed big-file summon almost never reaches its
 * entry frame, so only small summons survive kills). */
static int crash_pick_op(int *kind, char *target,
                         uint8_t **new_data, uint32_t *new_sz,
                         const uint8_t **old_data, uint32_t *old_sz,
                         uint32_t *hooks) {
    *new_data = NULL;
    uint32_t roll = crash_rand() % 100;
    if ((roll < 50 && crash_model_n < CRASH_MAX_MODEL - 1) || crash_model_n == 0) {
        *kind = CRASH_OP_SUMMON;
        int ok_name = 0;
        for (int tries = 0; tries < 8 && !ok_name; tries++) {
            snprintf(target, MDIM_MAX_NAME, "f%04u.bin", crash_rand() % 9000);
            if (crash_model_find(target) < 0) ok_name = 1;
        }
        if (!ok_name) return 1;
        *new_sz = (crash_rand() % 2 == 0) ? 1000 + crash_rand() % 3000
                : 40 * 1024 + crash_rand() % (140 * 1024);
        *new_data = (uint8_t *)malloc(*new_sz);
        for (uint32_t i = 0; i < *new_sz; i++)
            (*new_data)[i] = (uint8_t)(crash_rand() ^ (i * 29));
        *hooks = crash_hooks_summon(*new_sz);
    } else if (roll < 90) {
        *kind = CRASH_OP_WRITE;
        int mi = crash_rand() % crash_model_n;
        strncpy(target, crash_model[mi].name, MDIM_MAX_NAME - 1);
        *old_sz = crash_model[mi].size;
        *old_data = crash_model[mi].data;
        *new_sz = (crash_rand() % 4 == 1) ? 0
                : (crash_rand() % 4 == 0) ? 1000 + crash_rand() % 3000
                : 40 * 1024 + crash_rand() % (140 * 1024);
        *new_data = (uint8_t *)malloc(*new_sz ? *new_sz : 1);
        for (uint32_t i = 0; i < *new_sz; i++)
            (*new_data)[i] = (uint8_t)(crash_rand() ^ (i * 61));
        *hooks = crash_hooks_write(*old_sz, *new_sz);
    } else {
        *kind = CRASH_OP_UNSUMMON;
        int mi = crash_rand() % crash_model_n;
        strncpy(target, crash_model[mi].name, MDIM_MAX_NAME - 1);
        *old_sz = crash_model[mi].size;
        *old_data = crash_model[mi].data;
        *hooks = crash_hooks_unsummon(*old_sz);
    }
    return 0;
}

/* simulate a torn 4KB page write on top of the power loss: the journal
 * ring (slots 1..128 = exactly two 4 KB pages) was mid-write at the kill.
 * Flip 1..4 bytes in a random ring page — a torn page may hit committed
 * frames (must fail loud), the uncommitted crash frame (must fail loud),
 * a frame header, or the dead zone (may recover clean). */
static void crash_tear_page(void) {
    uint32_t page = crash_rand() % 2;                       /* slots 1..64 | 65..128 */
    uint32_t slot0 = MDIM_JRNL_START + page * 64;
    uint32_t flips = 1 + crash_rand() % 4;
    for (uint32_t k = 0; k < flips; k++) {
        uint32_t slot = slot0 + crash_rand() % 64;
        uint32_t off  = crash_rand() % MDIM_SLOT_SZ;
        crash_snap[slot * MDIM_SLOT_SZ + off] ^= (uint8_t)(1u << (crash_rand() % 8));
    }
}

/* sync the model to a COMPLETED (never crashed) op */
static void crash_sync(MdimVolume *v, int kind, const char *target,
                       const uint8_t *new_data, uint32_t new_sz) {
    int present = mdim_open(v, target, NULL) == MDIM_OK;
    if (kind == CRASH_OP_SUMMON) {
        if (present) crash_model_add(target, new_data, new_sz);
    } else if (kind == CRASH_OP_UNSUMMON) {
        if (!present) crash_model_remove(target);
    } else {
        if (present) crash_model_add(target, new_data, new_sz);
    }
}

/* one full power-cycle stress run: `iters` random kills, a clean settle
 * phase, final verification. Returns 0 on success. */
static int crash_stress_one_seed(uint32_t seed, uint32_t iters,
                                 uint32_t *out_kills, uint32_t *out_tears,
                                 uint32_t *out_corrupt) {
    crash_rng_state = seed;
    crash_trace_pos = 0;
    crash_model_n = 0;
    MdimVolume live;
    if (mdim_volume_init(&live, NULL) != MDIM_OK) return 1;

    uint32_t kills = 0, tears = 0, corrupt = 0;
    int failed = 0;
    for (uint32_t it = 0; it < iters && !failed; it++) {
        int kind; char target[MDIM_MAX_NAME];
        uint8_t *new_data = NULL; uint32_t new_sz = 0, old_sz = 0, hooks = 0;
        const uint8_t *old_data = NULL;
        if (crash_pick_op(&kind, target, &new_data, &new_sz, &old_data, &old_sz, &hooks))
            continue;
        uint32_t cd = crash_rand() % hooks;
        crash_trace_add("%u: %s %s new=%u hooks=%u kill@%u", it,
                        kind == CRASH_OP_SUMMON ? "summon" : kind == CRASH_OP_WRITE ? "write" : "unsummon",
                        target, new_sz, hooks, cd);
        crash_arm((int)cd);
        int err = MDIM_OK;
        int op_ok = 1;
        if (kind == CRASH_OP_SUMMON) {
            if (mdim_summon(&live, target, new_data, new_sz, &err) == MDIM_FRAME_NONE) op_ok = 0;
        } else if (kind == CRASH_OP_WRITE) {
            if (mdim_write(&live, target, new_data, new_sz) != MDIM_OK) op_ok = 0;
        } else {
            if (mdim_unsummon(&live, target) != MDIM_OK) op_ok = 0;
        }
        if (!op_ok || crash_hits == 0) {   /* NOSPC/NOENT — nothing to crash */
            free(new_data);
            continue;
        }

        kills++;
        /* ~30% of kills also tear a 4 KB page of the journal ring (a torn
         * sector write on top of the power loss). The reload must then
         * EITHER recover to a consistent state OR fail loud CORRUPT —
         * never load OK with torn data. */
        int tore = 0;
        if (crash_rand() % 100 < 30) {
            crash_tear_page();
            tears++;
            tore = 1;
        }
        /* reload the crash snapshot — cycle RAM / mmap / file open paths */
        MdimVolume r;
        int rc;
        if (kills % 20 == 0) {
            const char *path = (crash_mmap_toggle ^= 1) ? CRASH_MMAP_A : CRASH_MMAP_B;
            MdimVolume tmp; memset(&tmp, 0, sizeof(tmp)); tmp.bytes = crash_snap;
            if (mdim_volume_save(&tmp, path) != MDIM_OK) { FAIL("save snap"); failed = 1; break; }
            rc = mdim_volume_mmap_open(path, &r);
        } else if (kills % 20 == 10) {
            MdimVolume tmp; memset(&tmp, 0, sizeof(tmp)); tmp.bytes = crash_snap;
            if (mdim_volume_save(&tmp, CRASH_LOAD) != MDIM_OK) { FAIL("save snap"); failed = 1; break; }
            rc = mdim_volume_load(&r, CRASH_LOAD);
        } else {
            rc = crash_load_from_snap(&r);
        }
        mdim_volume_free(&live);           /* the post-crash live state is dead */
        if (rc == MDIM_ERR_CORRUPT) {
            /* fail-loud: the torn page made the on-disk state unrecoverable.
             * The volume must NOT be used — restore from backup (here: a
             * fresh volume, model reset) and keep going. */
            crash_trace_add("    -> FAIL-LOUD CORRUPT (torn page) — backup restore");
            corrupt++;
            crash_model_free_all();
            if (mdim_volume_init(&live, NULL) != MDIM_OK) { FAIL("re-init"); failed = 1; break; }
            free(new_data);
            continue;
        }
        if (rc != MDIM_OK) {
            char msg[96];
            snprintf(msg, sizeof(msg), "reload rc=%d after kill #%u%s", rc, kills,
                     tore ? " (torn page)" : "");
            FAIL(msg); free(new_data); failed = 1; break;
        }
        /* a torn page that did NOT fail loud must have recovered to a fully
         * consistent volume — crash_check is the "never silently serves
         * torn data" assertion */
        const char *msg = crash_check(&r, kind, target, old_data, old_sz, new_data, new_sz);
        if (msg) {
            char m2[192];
            snprintf(m2, sizeof(m2), "kill #%u (%s %s): %s", kills,
                     kind == CRASH_OP_SUMMON ? "summon" : kind == CRASH_OP_WRITE ? "write" : "unsummon",
                     target, msg);
            FAIL(m2); free(new_data); mdim_volume_free(&r); failed = 1; break;
        }
        crash_trace_add("    -> recovered ok (model=%d)", crash_model_n);
        /* sync the model to the recovered state */
        int present = mdim_open(&r, target, NULL) == MDIM_OK;
        if (kind == CRASH_OP_SUMMON && present) crash_model_add(target, new_data, new_sz);
        else if (kind == CRASH_OP_UNSUMMON && !present) crash_model_remove(target);
        else if (kind == CRASH_OP_WRITE) {
            if (crash_file_matches(&r, target, new_data, new_sz))
                crash_model_add(target, new_data, new_sz);   /* recovery kept the new bytes */
            /* else the volume kept the old bytes — the model already holds them */
        }
        live = r;
        free(new_data);
    }

    /* settle: 40 clean ops (no kills) so the volume ends with real files */
    for (uint32_t s = 0; s < 40 && !failed; s++) {
        int kind; char target[MDIM_MAX_NAME];
        uint8_t *new_data = NULL; uint32_t new_sz = 0, old_sz = 0, hooks = 0;
        const uint8_t *old_data = NULL;
        if (crash_pick_op(&kind, target, &new_data, &new_sz, &old_data, &old_sz, &hooks))
            continue;
        int err = MDIM_OK;
        if (kind == CRASH_OP_SUMMON) {
            if (mdim_summon(&live, target, new_data, new_sz, &err) != MDIM_FRAME_NONE)
                crash_sync(&live, kind, target, new_data, new_sz);
        } else if (kind == CRASH_OP_WRITE) {
            if (mdim_write(&live, target, new_data, new_sz) == MDIM_OK)
                crash_sync(&live, kind, target, new_data, new_sz);
        } else {
            if (mdim_unsummon(&live, target) == MDIM_OK)
                crash_sync(&live, kind, target, NULL, 0);
        }
        free(new_data);
    }

    if (!failed) {
        /* final: no more kills — the volume must be a clean, fully readable FS */
        char names[CRASH_MAX_MODEL][MDIM_MAX_NAME];
        for (int i = 0; i < crash_model_n; i++) {
            if (!crash_file_matches(&live, crash_model[i].name, crash_model[i].data, crash_model[i].size)) {
                FAIL("final: model file mismatch"); failed = 1; break;
            }
        }
        if (!failed && mdim_ls(&live, names, CRASH_MAX_MODEL) != (uint32_t)crash_model_n) {
            FAIL("final: ls mismatch"); failed = 1;
        }
        if (!failed && live.n_files != (uint32_t)crash_model_n) {
            FAIL("final: n_files mismatch"); failed = 1;
        }
        /* clean power cycle: save + reload the settled volume, re-verify */
        if (!failed && mdim_volume_save(&live, CRASH_LOAD) == MDIM_OK) {
            MdimVolume r2;
            if (mdim_volume_load(&r2, CRASH_LOAD) == MDIM_OK) {
                for (int i = 0; i < crash_model_n; i++) {
                    if (!crash_file_matches(&r2, crash_model[i].name, crash_model[i].data, crash_model[i].size)) {
                        FAIL("final: reload mismatch"); failed = 1; break;
                    }
                }
                mdim_volume_free(&r2);
            }
        }
        if (!failed) {
            char msg[96];
            snprintf(msg, sizeof(msg), "(%u kills, %u files survived, all consistent)",
                     kills, crash_model_n);
            printf("                %s\n", msg);
        }
    }
    crash_model_free_all();
    mdim_volume_free(&live);
    if (out_kills)   *out_kills   = kills;
    if (out_tears)   *out_tears   = tears;
    if (out_corrupt) *out_corrupt = corrupt;
    return failed ? 1 : 0;
}

static void test_crash_stress(void) {
    TEST("crash stress — 1200 kills + torn pages across 3 seeds");
    static const uint32_t seeds[3] = { 0x9E3779B9u, 0x1234ABCDu, 0xC0FFEE42u };
    uint32_t total_kills = 0, total_tears = 0, total_corrupt = 0;
    for (int s = 0; s < 3; s++) {
        uint32_t kills = 0, tears = 0, corrupt = 0;
        if (crash_stress_one_seed(seeds[s], 400, &kills, &tears, &corrupt)) {
            FAIL("stress seed failed");
            return;
        }
        total_kills += kills; total_tears += tears; total_corrupt += corrupt;
    }
    if (total_tears == 0 || total_corrupt == 0 || total_kills - total_tears == 0) {
        /* both outcomes must actually occur: clean recoveries AND fail-loud */
        char msg[96];
        snprintf(msg, sizeof(msg), "tear mix off: kills=%u tears=%u corrupt=%u",
                 total_kills, total_tears, total_corrupt);
        FAIL(msg); return;
    }
    printf("                (%u kills, %u torn pages, %u fail-loud, all consistent)\n",
           total_kills, total_tears, total_corrupt);
    PASS();
}

/* ── 19 ────────────────────────────────────────────────────────────────── */

static void test_crash_torn_writes(void) {
    TEST("torn writes — fail-loud CORRUPT vs benign slack corruption");
    uint8_t data[3000];                       /* 48 slots — one frame (≤ 62 changes) */
    for (int i = 0; i < 3000; i++) data[i] = (uint8_t)(i * 19 + 3);

    /* case A: a torn byte inside a committed frame — the LOAD must fail
     * loud (MDIM_ERR_CORRUPT), never silently serve a torn volume */
    {
        MdimVolume v;
        mdim_volume_init(&v, NULL);
        int err = MDIM_OK;
        if (mdim_summon(&v, "torn.bin", data, 3000, &err) == MDIM_FRAME_NONE) {
            FAIL("summon A"); mdim_volume_free(&v); return;
        }
        uint32_t head = v.journal_head;
        uint32_t span = mdim_frame_span(mdim_frame_n_changes(mdim_frame_hdr(&v, head)));
        if (head + span > MDIM_JRNL_END) { FAIL("frame span"); mdim_volume_free(&v); return; }
        v.bytes[(head + 1) * MDIM_SLOT_SZ + 8] ^= 0xFF;   /* 1st change record */
        if (mdim_volume_save(&v, CRASH_LOAD) != MDIM_OK) { FAIL("save A"); mdim_volume_free(&v); return; }
        mdim_volume_free(&v);

        MdimVolume r;
        if (mdim_volume_load(&r, CRASH_LOAD) != MDIM_ERR_CORRUPT) {
            FAIL("torn committed frame must fail loud"); return;
        }
    }

    /* case B: a torn byte in the ring's unused slack is ignored */
    {
        MdimVolume v;
        mdim_volume_init(&v, NULL);
        int err = MDIM_OK;
        if (mdim_summon(&v, "torn.bin", data, 3000, &err) == MDIM_FRAME_NONE) {
            FAIL("summon B"); mdim_volume_free(&v); return;
        }
        /* last ring slot — past the single frame (span 101) and its tail */
        mdim_slot(&v, MDIM_JRNL_END - 1)->pad ^= 0x5A5A;
        if (mdim_volume_save(&v, CRASH_LOAD) != MDIM_OK) { FAIL("save B"); mdim_volume_free(&v); return; }
        mdim_volume_free(&v);

        MdimVolume r;
        if (mdim_volume_load(&r, CRASH_LOAD) != MDIM_OK) {
            FAIL("slack corruption must load clean"); return;
        }
        if (!crash_file_matches(&r, "torn.bin", data, 3000)) {
            FAIL("file after slack corruption"); mdim_volume_free(&r); return;
        }
        mdim_volume_free(&r);
    }
    PASS();
}

/* ── 20 ────────────────────────────────────────────────────────────────── */

/* tear 1..3 bytes inside a committed frame's change records (slot A: slot
 * index + before[0..59]; slot B: before[60..63] + per-change CRC) */
static void timeline_tear_frame(MdimVolume *v, uint32_t fslot) {
    uint32_t n = mdim_frame_n_changes(mdim_frame_hdr(v, fslot));
    uint32_t flips = 1 + crash_rand() % 3;
    for (uint32_t k = 0; k < flips; k++) {
        uint32_t ca = fslot + 1 + 2 * (crash_rand() % n) + (crash_rand() % 2);
        v->bytes[ca * MDIM_SLOT_SZ + crash_rand() % MDIM_SLOT_SZ] ^=
            (uint8_t)(1u << (crash_rand() % 8));
    }
}

/* ring slot holding the committed frame with frame_no `fn` */
static uint32_t timeline_frame_slot(MdimVolume *v, uint32_t fn) {
    uint32_t f = v->journal_head;
    uint32_t prev_fn = MDIM_FRAME_NONE;
    while (f != MDIM_FRAME_NONE) {
        uint8_t *hdr = mdim_frame_hdr(v, f);
        if (hdr[MDIM_FH_MAGIC] != MDIM_JMAGIC) return MDIM_FRAME_NONE;
        uint32_t g = mdim_u32(hdr, MDIM_FH_FRAME_NO);
        if (prev_fn != MDIM_FRAME_NONE && g >= prev_fn) return MDIM_FRAME_NONE;
        if (g == fn) return f;
        prev_fn = g;
        f = mdim_u32(hdr, MDIM_FH_PREV);
    }
    return MDIM_FRAME_NONE;
}

static void test_timeline_torn_frames(void) {
    TEST("timeline — torn committed frames fail loud on versioned reads");
    crash_rng_state = 0x7E57A11u;

    const char *one = "version-one";
    const char *two = "version-two";
    const char *thr = "version-three";
    const char *bee = "bee-one";
    const char *expect[7] = { NULL, one, two, two, thr, thr, thr };

    MdimVolume v;
    if (mdim_volume_init(&v, NULL) != MDIM_OK) { FAIL("init"); return; }
    int err = MDIM_OK;
    mdim_summon(&v, "doc.txt", (const uint8_t *)one, (uint32_t)strlen(one) + 1, &err); /* f1 */
    mdim_write(&v, "doc.txt", (const uint8_t *)two, (uint32_t)strlen(two) + 1);         /* f2 f3 */
    mdim_write(&v, "doc.txt", (const uint8_t *)thr, (uint32_t)strlen(thr) + 1);         /* f4 f5 */
    mdim_summon(&v, "bee.txt", (const uint8_t *)bee, (uint32_t)strlen(bee) + 1, &err);  /* f6 */
    uint32_t last = mdim_last_frame(&v);
    if (last != 6) { FAIL("frame numbering"); mdim_volume_free(&v); return; }

    if (mdim_volume_save(&v, MDIM_TEST_VOL) != MDIM_OK) { FAIL("save"); mdim_volume_free(&v); return; }
    mdim_volume_free(&v);
    if (mdim_volume_load(&v, MDIM_TEST_VOL) != MDIM_OK) { FAIL("load"); return; }

    /* sanity: every versioned read is byte-correct BEFORE any tear */
    uint8_t buf[64]; uint32_t actual = 0;
    for (uint32_t f = 1; f <= last; f++) {
        if (mdim_read_at(&v, "doc.txt", f, buf, sizeof(buf), &actual) != MDIM_OK ||
            strcmp((const char *)buf, expect[f]) != 0) {
            FAIL("pre-tear versioned read"); mdim_volume_free(&v); return;
        }
    }
    if (mdim_read_at(&v, "bee.txt", 6, buf, sizeof(buf), &actual) != MDIM_OK ||
        strcmp((const char *)buf, bee) != 0) { FAIL("pre-tear bee@6"); mdim_volume_free(&v); return; }
    if (mdim_read_at(&v, "bee.txt", 5, buf, sizeof(buf), &actual) != MDIM_ERR_NOENT) {
        FAIL("pre-tear bee absent"); mdim_volume_free(&v); return;
    }

    /* case A: tear the NEWEST committed frame — EVERY versioned read in the
     * retained range must fail loud (never wrong data) */
    {
        timeline_tear_frame(&v, v.journal_head);
        for (uint32_t f = 1; f <= last; f++) {
            if (mdim_read_at(&v, "doc.txt", f, buf, sizeof(buf), &actual) != MDIM_ERR_CORRUPT ||
                mdim_read_at(&v, "bee.txt", f, buf, sizeof(buf), &actual) != MDIM_ERR_CORRUPT) {
                FAIL("head tear: read_at must fail loud"); mdim_volume_free(&v); return;
            }
        }
        uint8_t *snap = (uint8_t *)malloc(MDIM_VOL_BYTES);
        if (!snap) { FAIL("malloc"); mdim_volume_free(&v); return; }
        if (mdim_state_at(&v, last - 1, snap) != MDIM_ERR_CORRUPT ||
            mdim_state_at(&v, last, snap) != MDIM_ERR_CORRUPT) {
            FAIL("head tear: state_at must fail loud"); free(snap); mdim_volume_free(&v); return;
        }
        free(snap);
        /* the CURRENT base is untouched — direct reads still serve real data */
        if (mdim_verify(&v, "doc.txt") != MDIM_OK ||
            mdim_verify(&v, "bee.txt") != MDIM_OK) {
            FAIL("head tear: base reads must survive"); mdim_volume_free(&v); return;
        }
    }

    /* case B: tear a MIDDLE frame (f3) — reads that must consult it fail
     * loud; reads strictly above it stay byte-correct (never wrong data) */
    {
        mdim_volume_free(&v);
        if (mdim_volume_load(&v, MDIM_TEST_VOL) != MDIM_OK) { FAIL("reload"); return; }
        uint32_t mid = timeline_frame_slot(&v, 3);
        if (mid == MDIM_FRAME_NONE) { FAIL("frame 3 not found"); mdim_volume_free(&v); return; }
        timeline_tear_frame(&v, mid);
        for (uint32_t f = 1; f <= last; f++) {
            int rc = mdim_read_at(&v, "doc.txt", f, buf, sizeof(buf), &actual);
            if (f <= 3) {
                if (rc != MDIM_ERR_CORRUPT) {
                    FAIL("middle tear: read_at must fail loud"); mdim_volume_free(&v); return;
                }
            } else {
                if (rc != MDIM_OK || strcmp((const char *)buf, expect[f]) != 0) {
                    FAIL("middle tear: read above must stay correct"); mdim_volume_free(&v); return;
                }
            }
            rc = mdim_read_at(&v, "bee.txt", f, buf, sizeof(buf), &actual);
            if (f <= 3) {
                if (rc != MDIM_ERR_CORRUPT) {
                    FAIL("middle tear: bee must fail loud"); mdim_volume_free(&v); return;
                }
            } else if (f == 6) {
                if (rc != MDIM_OK || strcmp((const char *)buf, bee) != 0) {
                    FAIL("middle tear: bee@6 wrong"); mdim_volume_free(&v); return;
                }
            } else {
                if (rc != MDIM_ERR_NOENT) {
                    FAIL("middle tear: bee absent"); mdim_volume_free(&v); return;
                }
            }
        }
    }
    /* case C: tear a change record's SLOT-INDEX field (bytes 0..3 of slot A)
     * — the per-change CRC does NOT cover the index, so only the frame-level
     * CRC can catch this. Without it, the undo would silently write the
     * before-image to the WRONG slot. */
    {
        mdim_volume_free(&v);
        if (mdim_volume_load(&v, MDIM_TEST_VOL) != MDIM_OK) { FAIL("reload C"); return; }
        uint32_t head = v.journal_head;
        uint32_t n = mdim_frame_n_changes(mdim_frame_hdr(&v, head));
        uint32_t ca = head + 1 + 2 * (crash_rand() % n);     /* slot A of a record */
        v.bytes[ca * MDIM_SLOT_SZ + (crash_rand() % 4)] ^= 0xFF;   /* the index */
        if (mdim_read_at(&v, "doc.txt", last, buf, sizeof(buf), &actual) != MDIM_ERR_CORRUPT ||
            mdim_read_at(&v, "doc.txt", last - 1, buf, sizeof(buf), &actual) != MDIM_ERR_CORRUPT) {
            FAIL("torn slot index must fail loud (frame CRC)"); mdim_volume_free(&v); return;
        }
    }
    mdim_volume_free(&v);
    PASS();
}

/* ═══════════════════════════════════════════════════════════════════════════
   MAIN
   ═══════════════════════════════════════════════════════════════════════════ */

int main(void) {
    printf("╔══════════════════════════════════════════════════════════════╗\n");
    printf("║  GeoFS MDIM — Multidimensional Native Volume tests          ║\n");
    printf("╚══════════════════════════════════════════════════════════════╝\n\n");

    test_volume_init();
    test_view_roundtrips();
    test_summon_read();
    test_read_through_views();
    test_name_probe();
    test_unsummon_chain();
    test_rewrite();
    test_timeline_versions();
    test_crash_recovery_open_frame();
    test_crash_recovery_corrupt();
    test_save_load_roundtrip();
    test_ring_wrap_eviction();
    test_mmap_roundtrip();
    test_multiframe_run();
    test_multiframe_rewrite();
    test_orphan_sweep();
    test_crash_sweep();
    test_crash_stress();
    test_crash_torn_writes();
    test_timeline_torn_frames();

    printf("\n───────────────────────────────────────\n");
    printf("PASS: %d / %d  FAIL: %d\n", tests_passed, tests_passed + tests_failed, tests_failed);
    printf("═══════════════════════════════════════\n");
    return tests_failed > 0 ? 1 : 0;
}
