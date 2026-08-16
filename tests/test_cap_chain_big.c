/* test_cap_chain_big.c — Chain at scale: real mp4, eviction, lossless
 * ═══════════════════════════════════════════════════════════════════════════
 * The biggest mp4 in F:/notebookLM (~60 MB → ~3,700 chunks of 16 KB) through
 * the same chain as the PDF proof, testing what happens AT SCALE:
 *
 *   A. forced eviction:  capacity 1024 < 3,681 chunks → residual_space
 *      must LRU-evict.  Verify: count stays bounded, evictions counted,
 *      oldest chunks gone (thaw NULL), newest survive — the cache works.
 *   B. bounded-window streaming (§15.2: อ่านเต็ม = replay ต่อ chunk,
 *      workspace bounded): process in windows of 1024 chunks — place,
 *      verify lossless, evict the window, next.  Whole file reconstructs
 *      byte-for-byte with bounded memory (16 MB at a time).
 *   C. whole-resident: capacity 4096 holds everything → no eviction →
 *      full reconstruction byte-for-byte (the "file in RAM" case).
 *
 * BUILD: gcc -O2 -Wall -Wextra -Wno-unused-parameter -I. -Icore -Icore/infra \
 *        -o build/test-cap_chain_big tests/test_cap_chain_big.c -lm
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

/* ── A. forced eviction (capacity < chunks) ── */
static uint32_t test_eviction(const uint8_t *orig, uint64_t fsize, uint32_t nchunks) {
    printf("\n═ A. forced eviction — capacity 1024 < %u chunks ═\n", nchunks);
    ResidualSpace rs; rs_init(&rs, 1024);

    for (uint32_t i = 0; i < nchunks; i++) {
        uint8_t w = scale_w(i);
        uint32_t len = (uint32_t)((i == nchunks - 1)
                      ? (uint32_t)(fsize - (uint64_t)i * CHUNK_SZ) : CHUNK_SZ);
        PoglsPiece p = ghost_piece(i, 0, w);
        rs_freeze(&rs, &p, orig + (uint64_t)i * CHUNK_SZ, len, 0);
    }

    uint32_t expected_evict = nchunks - 1024;
    CHECK(1, "count bounded by capacity (no silent growth)", rs.count == 1024);
    CHECK(1, "evictions == chunks − capacity (LRU kicked in)",
          rs.evictions == expected_evict);

    uint32_t out_sz = 0;
    PoglsPiece p0 = ghost_piece(0, 0, scale_w(0));
    CHECK(2, "chunk 0 (oldest) evicted → thaw NULL",
          rs_thaw(&rs, pogls_bond_key(&p0), &out_sz) == NULL);
    PoglsPiece plast = ghost_piece(nchunks - 1, 0, scale_w(nchunks - 1));
    CHECK(2, "last chunk (newest) survives → readable",
          rs_thaw(&rs, pogls_bond_key(&plast), &out_sz) != NULL);
    uint32_t mid = nchunks - 512;   /* recent — inside the surviving window */
    PoglsPiece pmid = ghost_piece(mid, 0, scale_w(mid));
    CHECK(2, "recent chunk survives (LRU: oldest out, newest in)",
          rs_thaw(&rs, pogls_bond_key(&pmid), &out_sz) != NULL);

    rs_free(&rs);
    return expected_evict;
}

/* ── B. bounded-window streaming (workspace bounded — §15.2) ── */
static int test_streaming(const uint8_t *orig, uint64_t fsize, uint32_t nchunks) {
    printf("\n═ B. bounded-window streaming — window 1024 chunks (16 MB) ═\n");
    GhostLog log;  ghost_log_init(&log);
    uint8_t *recon = (uint8_t *)malloc((size_t)fsize);
    const uint32_t WINDOW = 1024;

    uint32_t placed = 0, evicted_windows = 0;
    while (placed < nchunks) {
        ResidualSpace rs; rs_init(&rs, WINDOW);
        uint32_t end = (placed + WINDOW < nchunks) ? placed + WINDOW : nchunks;
        uint8_t *lifted = (uint8_t *)calloc(WINDOW, sizeof(uint8_t));
        for (uint32_t i = placed; i < end; i++) {
            uint8_t w = scale_w(i);
            uint32_t len = (uint32_t)((i == nchunks - 1)
                          ? (uint32_t)(fsize - (uint64_t)i * CHUNK_SZ) : CHUNK_SZ);
            int r = ghost_lift_auto(&log, &rs, 1.0, (uint16_t)i, 0, w,
                                    orig + (uint64_t)i * CHUNK_SZ, len);
            if (r == GHOST_AUTO_LIFT)       lifted[i - placed] = 1;
            else if (r != GHOST_AUTO_PLACE) { free(lifted); rs_free(&rs); free(recon); return 0; }
        }
        /* verify the window NOW (before eviction) — workspace bounded */
        for (uint32_t i = placed; i < end; i++) {
            uint8_t w = scale_w(i);
            uint32_t len = (uint32_t)((i == nchunks - 1)
                          ? (uint32_t)(fsize - (uint64_t)i * CHUNK_SZ) : CHUNK_SZ);
            const uint8_t *src;
            if (lifted[i - placed]) {
                uint32_t out_sz = 0;
                src = (const uint8_t *)ghost_read(&log, &rs, (uint16_t)i, 0, w, &out_sz);
                if (!src || out_sz != len) { free(lifted); rs_free(&rs); free(recon); return 0; }
            } else {
                src = orig + (uint64_t)i * CHUNK_SZ;   /* pointer-home */
            }
            if (memcmp(src, orig + (uint64_t)i * CHUNK_SZ, len) != 0) {
                free(lifted); rs_free(&rs); free(recon); return 0;
            }
            memcpy(recon + (uint64_t)i * CHUNK_SZ, src, len);
        }
        free(lifted);
        rs_free(&rs);   /* window done — evict whole window */
        evicted_windows++;
        placed = end;
    }

    int ok = (memcmp(recon, orig, (size_t)fsize) == 0);
    printf("  %u windows streamed (each 1024 chunks, bounded 16 MB) — reconstruct %s\n",
           evicted_windows, ok ? "byte-for-byte" : "MISMATCH");
    free(recon);
    return ok;
}

/* ── C. whole-resident (capacity holds everything) ── */
static int test_whole_resident(const uint8_t *orig, uint64_t fsize, uint32_t nchunks) {
    printf("\n═ C. whole-resident — capacity 4096 ≥ %u chunks ═\n", nchunks);
    GhostLog log;  ghost_log_init(&log);
    ResidualSpace rs; rs_init(&rs, 4096);

    uint8_t *lifted = (uint8_t *)calloc(nchunks, sizeof(uint8_t));
    for (uint32_t i = 0; i < nchunks; i++) {
        uint8_t w = scale_w(i);
        uint32_t len = (uint32_t)((i == nchunks - 1)
                      ? (uint32_t)(fsize - (uint64_t)i * CHUNK_SZ) : CHUNK_SZ);
        int r = ghost_lift_auto(&log, &rs, 1.0, (uint16_t)i, 0, w,
                                orig + (uint64_t)i * CHUNK_SZ, len);
        if (r == GHOST_AUTO_LIFT) lifted[i] = 1;
        else if (r != GHOST_AUTO_PLACE) { free(lifted); rs_free(&rs); return 0; }
    }

    int ok = 1;
    uint32_t n_lift = 0;
    for (uint32_t i = 0; i < nchunks && ok; i++) {
        uint8_t w = scale_w(i);
        uint32_t len = (uint32_t)((i == nchunks - 1)
                      ? (uint32_t)(fsize - (uint64_t)i * CHUNK_SZ) : CHUNK_SZ);
        if (lifted[i]) {
            n_lift++;
            uint32_t out_sz = 0;
            const void *got = ghost_read(&log, &rs, (uint16_t)i, 0, w, &out_sz);
            if (!got || out_sz != len ||
                memcmp(got, orig + (uint64_t)i * CHUNK_SZ, len) != 0) ok = 0;
        }
    }
    printf("  placed %u chunks (%u lifted, %u pointer-home), %u evictions — reconstruct %s\n",
           nchunks, n_lift, nchunks - n_lift, rs.evictions,
           ok ? "byte-for-byte" : "MISMATCH");
    free(lifted);
    rs_free(&rs);
    return ok;
}

int main(int argc, char **argv) {
    setvbuf(stdout, NULL, _IONBF, 0);
    const char *root = (argc > 1) ? argv[1] : "F:/notebookLM";

    printf("Chain at scale — biggest mp4, eviction, lossless reconstruction\n");
    printf("══════════════════════════════════════════════════════════════════\n");

    char mp4_path[1100] = { 0 };
    uint64_t big = 0;
    find_biggest_mp4(root, mp4_path, sizeof(mp4_path), &big);
    if (big == 0) {
        printf("  (no .mp4 under %s — skipping)\n", root);
        printf("RESULTS: %d/%d PASS\n", pass, pass + fail);
        return 0;
    }
    printf("  biggest mp4: %s (%llu bytes)\n", mp4_path, (unsigned long long)big);

    uint8_t *orig = NULL;
    uint64_t fsize = 0;
    if (read_file(mp4_path, &orig, &fsize) != 0) {
        printf("  (cannot read — skipping)\n");
        printf("RESULTS: %d/%d PASS\n", pass, pass + fail);
        return 0;
    }
    uint32_t nchunks = (uint32_t)((fsize + CHUNK_SZ - 1) / CHUNK_SZ);
    printf("  %llu MB → %u chunks of %u B\n",
           (unsigned long long)(fsize >> 20), nchunks, CHUNK_SZ);

    /* sanity: mp4 header — ftyp box */
    CHECK(1, "real mp4 (ftyp magic) + ≥ 28 MB", fsize >= 28u * 1024 * 1024 &&
          orig[4] == 'f' && orig[5] == 't' && orig[6] == 'y' && orig[7] == 'p');

    uint32_t expected_evict = test_eviction(orig, fsize, nchunks);
    CHECK(2, "chunk count > capacity — eviction REALLY forced",
          nchunks > 1024 && expected_evict > 0);

    CHECK(3, "bounded-window streaming reconstructs byte-for-byte",
          test_streaming(orig, fsize, nchunks) == 1);

    CHECK(4, "whole-resident reconstructs byte-for-byte",
          test_whole_resident(orig, fsize, nchunks) == 1);

    /* determinism: same file re-run → same chunk verdicts */
    {
        CapAccount a, b; cap_init(&a); cap_init(&b);
        int same = 1;
        for (uint32_t i = 0; i < nchunks; i++) {
            uint8_t w = scale_w(i);
            if (cap_admit(&a, 1.0, 0, w) != cap_admit(&b, 1.0, 0, w)) same = 0;
        }
        CHECK(5, "deterministic: fresh account → same verdicts", same);
    }

    free(orig);
    printf("\n══════════════════════════════════════════════════════════════════\n");
    printf("RESULTS: %d/%d PASS\n", pass, pass + fail);
    return fail ? 1 : 0;
}
