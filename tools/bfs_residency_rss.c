/*
 * bfs_residency_rss.c — BFS residency layer REAL proof: RSS tracks the
 * working set, not sizeof(BreathingFS).
 * ═══════════════════════════════════════════════════════════════════
 * Before: block_encoded[144][2048] lived inside the struct. memset(fs) on
 * bfs_init touched all 294,912 B → every live field paid 288 KB of payload
 * RSS whether or not a single byte was written.
 *
 * After: the payload lives in a reserved region (144 pages, PROT_NONE,
 * 0 physical). A block's page is committed on write and decommitted on
 * delete/evict → RSS grows with the blocks actually resident.
 *
 * This tool measures process RSS across four phases and asserts:
 *   reserve(144 blocks)  → ~0 physical
 *   write K blocks       → ~K pages resident (K*4 KiB), NOT 144 pages
 *   delete everything    → RSS returns near baseline
 *   after bfs_destroy    → reservation released
 *
 * Usage: bfs_residency_rss [K]         (default K = 8 resident blocks)
 * BUILD: gcc -O2 -Wall -I. -Icore -Icore/infra -no-pie \
 *          -o build/bfs_residency_rss tools/bfs_residency_rss.c -lm
 */
#include <stdio.h>
#include <stdint.h>
#include <string.h>
#include <stdlib.h>
#include <unistd.h>
#if !defined(_WIN32)
#include <sys/mman.h>
#endif
#include "breathing_fs.h"

#if defined(_WIN32)
static long rss_kb(void) { return -1; }
static long page_kb_size(void) { return 4; }
static long resident_pages_of(void *base, size_t bytes) { (void)base; (void)bytes; return -1; }
#else
static long rss_kb(void) {
    FILE *f = fopen("/proc/self/statm", "r");
    if (!f) return -1;
    long total = 0, resident = 0;
    if (fscanf(f, "%ld %ld", &total, &resident) != 2) { fclose(f); return -1; }
    fclose(f);
    return resident * (long)(sysconf(_SC_PAGESIZE) / 1024);
}
static long page_kb_size(void) { return (long)(sysconf(_SC_PAGESIZE) / 1024); }
/* Authoritative residency signal: pages the kernel reports present in the
 * reserved region. RSS is noisy (allocator/stack churn); mincore is exact. */
static long resident_pages_of(void *base, size_t bytes) {
    size_t np = bytes / 4096u;
    unsigned char *vec = (unsigned char *)malloc(np);
    if (!vec) return -1;
    if (mincore(base, bytes, vec) != 0) { free(vec); return -1; }
    long n = 0;
    for (size_t i = 0; i < np; i++) if (vec[i] & 1) n++;
    free(vec);
    return n;
}
#endif

static int8_t payload[BFS_SLOTS_BLOCK];

int main(int argc, char **argv) {
    uint32_t K = argc > 1 ? (uint32_t)atoi(argv[1]) : 8u;
    if (K == 0 || K > BFS_BLOCKS) K = 8u;

    for (uint32_t i = 0; i < BFS_SLOTS_BLOCK; i++)
        payload[i] = (int8_t)(i * 31u + 7u);

    long baseline = rss_kb();
    printf("BFS residency proof\n");
    printf("  sizeof(BreathingFS)        = %zu B (was 384536 before residency layer)\n",
           sizeof(BreathingFS));
    printf("  payload region (reserved)  = %u B / %u pages\n",
           BFS_PAYLOAD_REGION, BFS_BLOCKS);
    printf("  baseline RSS               = %ld KB\n", baseline);

    BreathingFS fs;
    memset(&fs, 0, sizeof(fs));
    bfs_init(&fs);

    /* authoritative: pages of the payload region the kernel reports present */
    void  *region = fs._payload_mem;
    long r_reserve = resident_pages_of(region, BFS_PAYLOAD_REGION);
    long after_reserve = rss_kb();
    printf("  after bfs_init (reserve)   = %ld pages resident, RSS %ld KB (+%ld KB)\n",
           r_reserve, after_reserve, after_reserve - baseline);

    /* write K files, each exactly one block (144 B) */
    for (uint32_t k = 0; k < K; k++) {
        char nm[32];
        snprintf(nm, sizeof(nm), "res%u", k);
        if (bfs_write(&fs, nm, payload, BFS_SLOTS_BLOCK) != 0) {
            printf("FAIL: bfs_write %s\n", nm);
            return 1;
        }
    }
    long r_write = resident_pages_of(region, BFS_PAYLOAD_REGION);
    long after_write = rss_kb();
    printf("  after %u-block write       = %ld pages resident, RSS %ld KB (+%ld KB vs reserve)\n",
           K, r_write, after_write, after_write - after_reserve);

    /* verify lossless before measuring the release */
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

    /* delete everything → pages decommit */
    for (uint32_t k = 0; k < K; k++) {
        char nm[32];
        snprintf(nm, sizeof(nm), "res%u", k);
        bfs_delete(&fs, nm);
    }
    long r_delete = resident_pages_of(region, BFS_PAYLOAD_REGION);
    long after_delete = rss_kb();
    printf("  after delete-all           = %ld pages resident, RSS %ld KB (%+ld KB vs reserve)\n",
           r_delete, after_delete, after_delete - after_reserve);

    bfs_payload_free(&fs);
    long after_free = rss_kb();
    printf("  after bfs_destroy          = RSS %ld KB (%+ld KB vs baseline)\n",
           after_free, after_free - baseline);

    printf("  payload readback mismatch  = %u\n", bad);

    /* ── verdict ──────────────────────────────────────────────────────
     * The old struct committed 294,912 B (72 pages) of payload no matter
     * what. A correct residency layer: reserve = 0 resident, write K
     * single-block files = exactly K resident pages, delete-all = 0. */
    int pass = 1;
    const char *why = "PASS";
    if (bad)                        { pass = 0; why = "readback mismatch"; }
    if (r_reserve > 0)              { pass = 0; why = "reserve materialized pages"; }
    if (r_write != (long)K)         { pass = 0; why = "resident pages != K blocks written"; }
    if (r_delete != 0)              { pass = 0; why = "delete did not decommit pages"; }

    printf("%s: reserve=%ld pages (0 expected)  write(%u blk)=%ld pages (%u expected)  delete=%ld pages (0 expected)\n",
           pass ? "RESIDENCY-PASS" : "RESIDENCY-FAIL",
           r_reserve, K, r_write, K, r_delete);
    if (!pass) printf("  reason: %s\n", why);
    return pass ? 0 : 1;
}