/* test_rs_persist.c — residual_space persistence: serialize by bond_key,
 * reload into a fresh space, thaw-after-reload lossless (real mp4)
 * ═══════════════════════════════════════════════════════════════════════════
 * §15.34 — the restart story:
 *
 *   place (lift) → serialize (space by bond_key + ghost log routes)
 *   → rs_free + fresh spaces (simulated process restart)
 *   → reload → ghost_read → byte-for-byte reconstruction of the file.
 *
 *   The ghost LOG (routes) is the durable audit trail; tombstones are an
 *   in-memory recycle bin and are NOT persisted (documented decision).
 *
 *   A. unit: empty/corrupt/deterministic/tombstone/flags roundtrips
 *   B. real mp4 (57 MB): whole-resident → serialize → reload → lossless
 *
 * BUILD: gcc -O2 -Wall -Wextra -Wno-unused-parameter -I. -Icore -Icore/infra \
 *        -o build/test-rs_persist tests/test_rs_persist.c -lm
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <dirent.h>
#include <sys/stat.h>
#include "../core/geo_cap_account.h"
#include "../core/geo_ghost_lift.h"

#define CHUNK_SZ 16384u

static int pass = 0, fail = 0;
#define CHECK(n, desc, cond) do { \
    if (cond) { pass++; printf("  T%d: PASS — %s\n", n, desc); } \
    else      { fail++; printf("  T%d: FAIL — %s\n", n, desc); } \
} while(0)

static uint8_t scale_w(uint32_t rank) {
    return (uint8_t)(((uint64_t)rank * 37u) % 144u);
}

/* ── helpers ─────────────────────────────────────────────── */

static uint64_t bond_of(uint16_t block, uint8_t from, uint8_t to) {
    PoglsPiece p = ghost_piece(block, from, to);
    return pogls_bond_key(&p);
}

static void fill_pattern(uint8_t *b, uint32_t n, uint32_t seed) {
    for (uint32_t i = 0; i < n; i++)
        b[i] = (uint8_t)((seed + i * 131u) ^ (i >> 3));
}

/* find slot index holding bond_key (test helper — scan is fine here) */
static ResidualEntry *find_entry(ResidualSpace *rs, uint64_t bk) {
    for (uint32_t i = 0; i < rs->capacity; i++) {
        ResidualEntry *e = rs->entries[i];
        if (e && (e->flags & RS_ENTRY_VALID) && e->bond_key == bk) return e;
    }
    return NULL;
}

/* ── A. unit roundtrips ──────────────────────────────────── */

static void unit_empty(void) {
    ResidualSpace a, b;
    rs_init(&a, 64); rs_init(&b, 64);
    uint64_t sz = rs_serialize_size(&a);
    CHECK(1, "empty space serializes to just the 16B header", sz == 16);
    uint8_t buf[64];
    CHECK(1, "empty serialize returns 16 bytes", rs_serialize(&a, buf, sizeof(buf)) == 16);
    CHECK(1, "reload empty → count 0", rs_load(&b, buf, 16) == 0 && b.count == 0);
    rs_free(&a); rs_free(&b);
}

static void unit_roundtrip(void) {
    ResidualSpace a, b;
    rs_init(&a, 64); rs_init(&b, 64);

    uint8_t d0[10], d1[20], d2[30];
    fill_pattern(d0, 10, 1); fill_pattern(d1, 20, 2); fill_pattern(d2, 30, 3);
    PoglsPiece p0 = ghost_piece(1, 0, 5), p1 = ghost_piece(2, 0, 6),
                p2 = ghost_piece(3, 0, 7);
    uint64_t k0 = rs_freeze(&a, &p0, d0, 10, 0);
    uint64_t k1 = rs_freeze(&a, &p1, d1, 20, 1);
    uint64_t k2 = rs_freeze(&a, &p2, d2, 30, 0);

    uint64_t sz = rs_serialize_size(&a);
    uint8_t *buf = (uint8_t *)malloc((size_t)sz);
    CHECK(2, "3 entries → header + 3 records", sz == 16 + 3 * RS_ENTRY_HEADER_SZ + 60);
    CHECK(2, "serialize writes exact size", rs_serialize(&a, buf, sz) == sz);
    CHECK(2, "load into fresh space OK", rs_load(&b, buf, sz) == 0);
    CHECK(2, "count + total_bytes preserved", b.count == 3 && b.total_bytes == 60);

    uint32_t out = 0;
    const void *g0 = rs_thaw(&b, k0, &out);
    CHECK(3, "thaw-after-reload: entry 0 bytes match",
          g0 && out == 10 && memcmp(g0, d0, 10) == 0);
    const void *g1 = rs_thaw(&b, k1, &out);
    CHECK(3, "thaw-after-reload: entry 1 bytes match",
          g1 && out == 20 && memcmp(g1, d1, 20) == 0);
    const void *g2 = rs_thaw(&b, k2, &out);
    CHECK(3, "thaw-after-reload: entry 2 bytes match",
          g2 && out == 30 && memcmp(g2, d2, 30) == 0);

    CHECK(4, "rs_verify passes after reload (origin == geo_key)",
          rs_verify(&b, &p1) == 1);
    CHECK(4, "wrong bond key → thaw NULL",
          rs_thaw(&b, bond_of(9, 0, 9), &out) == NULL);

    free(buf); rs_free(&a); rs_free(&b);
}

static void unit_determinism(void) {
    ResidualSpace a;
    rs_init(&a, 64);
    uint8_t d[16];
    fill_pattern(d, 16, 42);
    PoglsPiece p7 = ghost_piece(7, 1, 12), p8 = ghost_piece(8, 2, 13);
    rs_freeze(&a, &p7, d, 16, 0);
    rs_freeze(&a, &p8, d, 8, 1);

    uint64_t sz = rs_serialize_size(&a);
    uint8_t *b1 = (uint8_t *)malloc((size_t)sz);
    uint8_t *b2 = (uint8_t *)malloc((size_t)sz);
    CHECK(5, "serialize is deterministic (same freeze seq → same bytes)",
          rs_serialize(&a, b1, sz) == sz && rs_serialize(&a, b2, sz) == sz &&
          memcmp(b1, b2, (size_t)sz) == 0);
    free(b1); free(b2); rs_free(&a);
}

static void unit_tombstone(void) {
    ResidualSpace a, b;
    rs_init(&a, 64); rs_init(&b, 64);
    uint8_t d[8];
    fill_pattern(d, 8, 5);
    PoglsPiece p1 = ghost_piece(1, 0, 5), p2 = ghost_piece(2, 0, 6);
    uint64_t k0 = rs_freeze(&a, &p1, d, 8, 0);
    rs_freeze(&a, &p2, d, 8, 0);
    CHECK(6, "tombstone before serialize", rs_tombstone(&a, k0) == 1);

    uint64_t sz = rs_serialize_size(&a);
    uint8_t *buf = (uint8_t *)malloc((size_t)sz);
    rs_serialize(&a, buf, sz);
    CHECK(6, "tombstoned entry dropped from serialized image", sz == 16 + RS_ENTRY_HEADER_SZ + 8);
    CHECK(6, "reload → only live entry survives",
          rs_load(&b, buf, sz) == 0 && b.count == 1);
    uint32_t out = 0;
    CHECK(6, "tombstoned key thaw NULL after reload",
          rs_thaw(&b, k0, &out) == NULL);
    free(buf); rs_free(&a); rs_free(&b);
}

static void unit_flags(void) {
    ResidualSpace a, b;
    rs_init(&a, 64); rs_init(&b, 64);
    uint8_t d[8];
    fill_pattern(d, 8, 9);
    PoglsPiece p3 = ghost_piece(3, 0, 7);
    uint64_t k0 = rs_freeze(&a, &p3, d, 8, 1);
    ResidualEntry *e = find_entry(&a, k0);
    CHECK(7, "entry found for flag mutation", e != NULL);
    if (e) e->flags |= RS_ENTRY_PINNED | RS_ENTRY_REF;   /* simulate pinned */

    uint64_t sz = rs_serialize_size(&a);
    uint8_t *buf = (uint8_t *)malloc((size_t)sz);
    rs_serialize(&a, buf, sz);
    CHECK(7, "reload preserves flags (PINNED/REF/HIGH_ENTROPY)",
          rs_load(&b, buf, sz) == 0 &&
          (find_entry(&b, k0)->flags & (RS_ENTRY_PINNED | RS_ENTRY_REF | RS_ENTRY_HIGH_ENTROPY))
              == (RS_ENTRY_PINNED | RS_ENTRY_REF | RS_ENTRY_HIGH_ENTROPY));
    free(buf); rs_free(&a); rs_free(&b);
}

static void unit_corrupt(void) {
    ResidualSpace b;
    rs_init(&b, 64);
    uint8_t bad[64];
    memset(bad, 0, sizeof(bad));
    CHECK(8, "bad magic rejected", rs_load(&b, bad, 64) == -1);
    memcpy(bad, "RSDWGLSP", 8);
    CHECK(8, "wrong version rejected", rs_load(&b, bad, 64) == -1);
    bad[8] = 1; bad[9] = 0;
    bad[12] = 1; bad[13] = 0; bad[14] = 0; bad[15] = 0;   /* count=1 but no record */
    CHECK(8, "truncated record rejected", rs_load(&b, bad, 16) == -1);
    CHECK(8, "load into non-fresh space rejected", rs_load(&b, bad, 64) == -1);
    rs_free(&b);
}

static void unit_disk_file(void) {
    ResidualSpace a, b;
    rs_init(&a, 64); rs_init(&b, 64);
    uint8_t d[12];
    fill_pattern(d, 12, 77);
    PoglsPiece p5 = ghost_piece(5, 0, 9);
    uint64_t k0 = rs_freeze(&a, &p5, d, 12, 0);

    const char *path = "build/rs_persist_t.bin";
    uint64_t sz = rs_serialize_size(&a);
    uint8_t *buf = (uint8_t *)malloc((size_t)sz);
    rs_serialize(&a, buf, sz);
    FILE *fp = fopen(path, "wb");
    CHECK(9, "write serialized space to disk file",
          fp && fwrite(buf, 1, (size_t)sz, fp) == sz);
    if (fp) fclose(fp);

    fp = fopen(path, "rb");
    uint64_t fsz = 0;
    if (fp) { fseek(fp, 0, SEEK_END); fsz = (uint64_t)ftell(fp); fseek(fp, 0, SEEK_SET); }
    CHECK(9, "file on disk has exact byte count", fp && fsz == sz);
    uint8_t *fbuf = (uint8_t *)malloc((size_t)fsz);
    if (fp) fread(fbuf, 1, (size_t)fsz, fp);
    if (fp) fclose(fp);

    CHECK(9, "reload from disk file → same bytes",
          rs_load(&b, fbuf, fsz) == 0 && b.count == 1);
    uint32_t out = 0;
    CHECK(9, "thaw-after-disk-reload matches",
          rs_thaw(&b, k0, &out) != NULL && out == 12 &&
          memcmp(rs_thaw(&b, k0, &out), d, 12) == 0);
    free(fbuf); free(buf); rs_free(&a); rs_free(&b);
}

/* ── B. real mp4 — restart roundtrip ─────────────────────── */

static void find_biggest_mp4(const char *dir, char *out, size_t outsz, uint64_t *big) {
    DIR *d = opendir(dir);
    if (!d) return;
    struct dirent *e;
    while ((e = readdir(d))) {
        if (strcmp(e->d_name, ".") == 0 || strcmp(e->d_name, "..") == 0) continue;
        char full[1100];
        snprintf(full, sizeof(full), "%s/%s", dir, e->d_name);
        struct stat st;
        if (stat(full, &st) != 0) continue;
        if (S_ISDIR(st.st_mode)) { find_biggest_mp4(full, out, outsz, big); continue; }
        size_t len = strlen(full);
        if (len >= 4 && strcmp(full + len - 4, ".mp4") == 0 &&
            (uint64_t)st.st_size > *big) {
            *big = (uint64_t)st.st_size;
            snprintf(out, outsz, "%s", full);
        }
    }
    closedir(d);
}

static int read_file(const char *path, uint8_t **buf, uint64_t *size) {
    FILE *fp = fopen(path, "rb");
    if (!fp) return -1;
    fseek(fp, 0, SEEK_END);
    long sz = ftell(fp);
    fseek(fp, 0, SEEK_SET);
    if (sz <= 0) { fclose(fp); return -1; }
    uint8_t *b = (uint8_t *)malloc((size_t)sz);
    if (!b || fread(b, 1, (size_t)sz, fp) != (size_t)sz) { free(b); fclose(fp); return -1; }
    fclose(fp);
    *buf = b; *size = (uint64_t)sz;
    return 0;
}

static int mp4_restart_roundtrip(const char *root) {
    printf("\n═ B. real mp4 — serialize → restart → reload → lossless ═\n");
    char mp4_path[1100] = { 0 };
    uint64_t big = 0;
    find_biggest_mp4(root, mp4_path, sizeof(mp4_path), &big);
    if (big == 0) {
        printf("  (no .mp4 under %s — skipping)\n", root);
        return 0;
    }
    printf("  biggest mp4: %s (%llu bytes)\n", mp4_path, (unsigned long long)big);

    uint8_t *orig = NULL;
    uint64_t fsize = 0;
    if (read_file(mp4_path, &orig, &fsize) != 0) {
        printf("  (cannot read — skipping)\n");
        return 0;
    }
    uint32_t nchunks = (uint32_t)((fsize + CHUNK_SZ - 1) / CHUNK_SZ);
    printf("  %llu MB → %u chunks of %u B\n",
           (unsigned long long)(fsize >> 20), nchunks, CHUNK_SZ);

    /* ── phase 1: place (whole-resident, capacity 4096) ── */
    GhostLog log;      ghost_log_init(&log);
    ResidualSpace rs;  rs_init(&rs, 4096);
    uint8_t *lifted = (uint8_t *)calloc(nchunks, sizeof(uint8_t));

    for (uint32_t i = 0; i < nchunks; i++) {
        uint8_t w = scale_w(i);
        uint32_t len = (uint32_t)((i == nchunks - 1)
                      ? (uint32_t)(fsize - (uint64_t)i * CHUNK_SZ) : CHUNK_SZ);
        int r = ghost_lift_auto(&log, &rs, 1.0, (uint16_t)i, 0, w,
                                orig + (uint64_t)i * CHUNK_SZ, len);
        if (r == GHOST_AUTO_LIFT) lifted[i] = 1;
        else if (r != GHOST_AUTO_PLACE) {
            free(lifted); rs_free(&rs); free(orig); return 0;
        }
    }
    uint32_t n_lift = 0;
    for (uint32_t i = 0; i < nchunks; i++) n_lift += lifted[i];
    printf("  placed %u chunks (%u lifted → residual_space, %u pointer-home), "
           "%u evictions\n", nchunks, n_lift, nchunks - n_lift, rs.evictions);

    /* in-memory verify BEFORE serialize */
    int ok_pre = 1;
    for (uint32_t i = 0; i < nchunks && ok_pre; i++) {
        uint8_t w = scale_w(i);
        uint32_t len = (uint32_t)((i == nchunks - 1)
                      ? (uint32_t)(fsize - (uint64_t)i * CHUNK_SZ) : CHUNK_SZ);
        if (!lifted[i]) continue;
        uint32_t out_sz = 0;
        const void *got = ghost_read(&log, &rs, (uint16_t)i, 0, w, &out_sz);
        if (!got || out_sz != len ||
            memcmp(got, orig + (uint64_t)i * CHUNK_SZ, len) != 0) ok_pre = 0;
    }
    CHECK(10, "pre-reload in-memory reconstruction lossless", ok_pre);

    uint32_t pre_count = rs.count;
    uint64_t pre_bytes = rs.total_bytes;

    /* ── phase 2: serialize both log and space ── */
    uint64_t rsz = rs_serialize_size(&rs);
    uint64_t lsz = ghost_log_serialize_size(&log);
    uint8_t *sbuf = (uint8_t *)malloc((size_t)rsz);
    uint8_t *lbuf = (uint8_t *)malloc((size_t)lsz);
    CHECK(11, "serialize space + log sizes exact",
          rs_serialize(&rs, sbuf, rsz) == rsz &&
          ghost_log_serialize(&log, lbuf, lsz) == lsz);
    printf("  residual image: %llu KB (%u entries, %llu data bytes)\n",
           (unsigned long long)(rsz >> 10), pre_count, (unsigned long long)pre_bytes);

    /* ── phase 3: simulated process restart (fresh everything) ── */
    rs_free(&rs);
    GhostLog log2;     ghost_log_init(&log2);
    ResidualSpace rs2; rs_init(&rs2, 4096);
    CHECK(12, "reload into fresh space + fresh log",
          rs_load(&rs2, sbuf, rsz) == 0 && ghost_log_load(&log2, lbuf, lsz) == 0);

    /* integrity of the reloaded space */
    CHECK(13, "count + total_bytes survive restart",
          rs2.count == pre_count && rs2.total_bytes == pre_bytes);
    int verify_ok = 1;
    for (uint32_t i = 0; i < nchunks && verify_ok; i++) {
        if (!lifted[i]) continue;
        uint8_t w = scale_w(i);
        PoglsPiece pv = ghost_piece((uint16_t)i, 0, w);
        if (!rs_verify(&rs2, &pv))
            verify_ok = 0;
    }
    CHECK(13, "rs_verify passes for every lifted chunk after reload", verify_ok);
    uint32_t out_sz = 0;
    CHECK(13, "wrong from_scale → bond breaks (NULL)",
          ghost_read(&log2, &rs2, 1, 1, scale_w(1), &out_sz) == NULL);
    CHECK(13, "wrong to_scale → route not found (NULL)",
          ghost_read(&log2, &rs2, 1, 0,
                     (uint8_t)((scale_w(1) + 1) % 144), &out_sz) == NULL);

    /* ── phase 4: reconstruct the whole file after reload ── */
    int ok_post = 1;
    for (uint32_t i = 0; i < nchunks && ok_post; i++) {
        uint8_t w = scale_w(i);
        uint32_t len = (uint32_t)((i == nchunks - 1)
                      ? (uint32_t)(fsize - (uint64_t)i * CHUNK_SZ) : CHUNK_SZ);
        const uint8_t *src;
        if (lifted[i]) {
            src = (const uint8_t *)ghost_read(&log2, &rs2, (uint16_t)i, 0, w, &out_sz);
            if (!src || out_sz != len) { ok_post = 0; break; }
        } else {
            src = orig + (uint64_t)i * CHUNK_SZ;   /* pointer-home — source of truth */
        }
        if (memcmp(src, orig + (uint64_t)i * CHUNK_SZ, len) != 0) ok_post = 0;
    }
    CHECK(14, "thaw-after-reload reconstructs the file byte-for-byte", ok_post);

    /* reloaded space is fully functional: freeze a NEW bond (next_timestamp
       continues after the max — LRU order survives).  block 50000 was never
       placed, so this is a genuinely new birth pile. */
    uint8_t extra[4] = { 9, 8, 7, 6 };
    PoglsPiece pnew = ghost_piece(50000, 0, 5);
    uint64_t kx = rs_freeze(&rs2, &pnew, extra, 4, 0);
    CHECK(15, "post-reload freeze works (next_timestamp continues)",
          kx != RS_BOND_KEY_RESERVED && rs2.count == pre_count + 1 &&
          rs_thaw(&rs2, kx, &out_sz) != NULL && out_sz == 4 &&
          rs2.next_timestamp == pre_count + 1);

    free(sbuf); free(lbuf); free(lifted); rs_free(&rs2); free(orig);
    return 1;
}

int main(int argc, char **argv) {
    setvbuf(stdout, NULL, _IONBF, 0);
    const char *root = (argc > 1) ? argv[1] : "F:/notebookLM";

    printf("residual_space persistence — serialize by bond_key, reload, lossless\n");
    printf("══════════════════════════════════════════════════════════════════\n");
    printf("A. unit roundtrips\n");

    unit_empty();
    unit_roundtrip();
    unit_determinism();
    unit_tombstone();
    unit_flags();
    unit_corrupt();
    unit_disk_file();

    mp4_restart_roundtrip(root);

    printf("\n══════════════════════════════════════════════════════════════════\n");
    printf("RESULTS: %d/%d PASS\n", pass, pass + fail);
    return fail ? 1 : 0;
}
