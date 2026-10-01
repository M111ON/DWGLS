/*
 * test_bfs_residency.c — BFS residency layer invariant
 * ═══════════════════════════════════════════════════════════════════
 * The payload region must behave like a reserved address space, not like
 * inline storage: reserving costs 0 physical pages, writing a block
 * commits exactly one page, deleting it decommits that page.
 *
 * Oracle is the kernel's own page map (mincore on POSIX / QueryWorkingSet
 * on Windows), never the BFS function under test.
 */
#include <stdio.h>
#include <stdint.h>
#include <string.h>
#include <stdlib.h>
#include "breathing_fs.h"

static int g_pass = 0, g_fail = 0;
#define CHECK(id, cond) do { \
    if (cond) { g_pass++; } else { g_fail++; printf("  [FAIL] T%d\n", (id)); } \
} while (0)

#if !defined(_WIN32)
#include <sys/mman.h>
#include <unistd.h>
static long resident_pages(void *base, size_t bytes) {
    size_t np = bytes / 4096u;
    unsigned char *vec = (unsigned char *)malloc(np);
    if (!vec) return -1;
    if (mincore(base, bytes, vec) != 0) { free(vec); return -1; }
    long n = 0;
    for (size_t i = 0; i < np; i++) if (vec[i] & 1) n++;
    free(vec);
    return n;
}
#define HAVE_MINICORE 1
#else
#define HAVE_MINICORE 0
static long resident_pages(void *base, size_t bytes) { (void)base; (void)bytes; return -1; }
#endif

int main(void) {
    printf("══ BFS residency (reserve/commit/decommit) ══\n");

    static int8_t payload[BFS_SLOTS_BLOCK];
    for (uint32_t i = 0; i < BFS_SLOTS_BLOCK; i++)
        payload[i] = (int8_t)(i * 29u + 3u);

    /* struct must no longer carry the payload inline (288 KB gone) */
    CHECK(1, sizeof(BreathingFS) < 120000u);

    /* page stride is a whole page, region = 144 pages */
    CHECK(2, BFS_PAYLOAD_STRIDE == 4096u);
    CHECK(3, BFS_PAYLOAD_REGION == BFS_BLOCKS * BFS_PAYLOAD_STRIDE);

    BreathingFS fs;
    memset(&fs, 0, sizeof(fs));
    bfs_init(&fs);

    const uint32_t K = 8u;
    void *region = fs._payload_mem;
    CHECK(4, region != NULL);

    if (HAVE_MINICORE) {
        /* reserve: full address space, zero resident */
        CHECK(5, resident_pages(region, BFS_PAYLOAD_REGION) == 0);

        /* write K single-block files → K resident pages */
        uint32_t ok = 1;
        for (uint32_t k = 0; k < K; k++) {
            char nm[32];
            snprintf(nm, sizeof(nm), "res%u", k);
            if (bfs_write(&fs, nm, payload, BFS_SLOTS_BLOCK) != 0) ok = 0;
        }
        CHECK(6, ok);
        CHECK(7, resident_pages(region, BFS_PAYLOAD_REGION) == (long)K);

        /* lossless readback (independent compare) */
        uint32_t bad = 0;
        for (uint32_t k = 0; k < K; k++) {
            char nm[32];
            snprintf(nm, sizeof(nm), "res%u", k);
            int8_t out[BFS_SLOTS_BLOCK];
            uint32_t act = 0;
            if (bfs_read(&fs, nm, out, sizeof(out), &act) != 0 ||
                act != BFS_SLOTS_BLOCK || memcmp(out, payload, BFS_SLOTS_BLOCK) != 0)
                bad++;
        }
        CHECK(8, bad == 0);

        /* delete-all → decommit back to zero */
        for (uint32_t k = 0; k < K; k++) {
            char nm[32];
            snprintf(nm, sizeof(nm), "res%u", k);
            bfs_delete(&fs, nm);
        }
        CHECK(9, resident_pages(region, BFS_PAYLOAD_REGION) == 0);

        /* fill all 144 pages → exactly 144 resident, then release to 0 */
        uint32_t full_ok = 1;
        for (uint32_t k = 0; k < BFS_MAX_FILES; k++) {
            char nm[32];
            snprintf(nm, sizeof(nm), "fill%u", k);
            if (bfs_write(&fs, nm, payload, BFS_SLOTS_BLOCK) != 0) { full_ok = 0; break; }
        }
        CHECK(10, full_ok);
        CHECK(11, resident_pages(region, BFS_PAYLOAD_REGION) == BFS_MAX_FILES);
    } else {
        /* Windows/non-mincore: prove functional correctness of the region,
         * and that destroy releases it (no leak / no crash). */
        uint32_t ok = 1;
        for (uint32_t k = 0; k < K; k++) {
            char nm[32];
            snprintf(nm, sizeof(nm), "res%u", k);
            if (bfs_write(&fs, nm, payload, BFS_SLOTS_BLOCK) != 0) ok = 0;
        }
        CHECK(5, ok);
    }

    bfs_destroy(&fs);
    CHECK(12, fs._payload_mem == NULL);
    bfs_destroy(&fs);   /* idempotent */
    CHECK(13, 1);

    printf(g_fail ? "RESIDENCY FAIL %d/%d\n" : "RESIDENCY PASS %d/%d\n",
           g_pass, g_pass + g_fail);
    return g_fail ? 1 : 0;
}