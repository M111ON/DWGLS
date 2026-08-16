/* cap_chain_scan.c — streaming chain over a whole folder (7.7 GB notebookLM)
 * ═══════════════════════════════════════════════════════════════════════════
 * Runs the REAL chain over every file:
 *
 *   file → chunks (16 KB) → per chunk: w = (37·rank)%144
 *     cap_admit(gate, 0, w)               (§11.6 accounting — field slots)
 *       CAP_LIFT  → rs_freeze (bond = ghost_piece) → residual_space
 *       CAP_ADMIT → pointer-home (data stays in source)
 *   streaming windows: capacity 1024 chunks (16 MB) — place → verify →
 *   teardown → next window (workspace bounded — §15.2)
 *
 * Reports per file: size, chunks, field slots, checksum (FNV-1a 64) OK/FAIL,
 * plus folder aggregates: total windows (base chain), stream windows
 * (teardowns), eviction pressure (peak rs count, forced evictions).
 *
 * The route/log layer is proven elsewhere (test_cap_chain_roundtrip) — this
 * scan goes straight to rs_freeze/thaw by bond for speed at ~480K chunks.
 *
 * BUILD: gcc -O2 -I. -Icore -Icore/infra -o build/cap_chain_scan tools/cap_chain_scan.c -lm
 * RUN:   build/cap_chain_scan [root]      (default F:/notebookLM)
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
#define WINDOW   1024u      /* chunks per stream window (16 MB) */
#define MAX_FILES 50000u

static uint8_t scale_w(uint32_t rank) {
    return (uint8_t)(((uint64_t)rank * 37u) % 144u);
}

typedef struct { char path[1100]; uint64_t size; } FileEntry;

static void walk_dir(const char *dir, FileEntry *out, uint32_t cap, uint32_t *n) {
    DIR *d = opendir(dir);
    if (!d) return;
    struct dirent *e;
    while ((e = readdir(d)) && *n < cap) {
        if (strcmp(e->d_name, ".") == 0 || strcmp(e->d_name, "..") == 0) continue;
        char full[1120];
        snprintf(full, sizeof(full), "%s/%s", dir, e->d_name);
        struct stat st;
        if (stat(full, &st) != 0) continue;
        if (S_ISDIR(st.st_mode)) { walk_dir(full, out, cap, n); continue; }
        snprintf(out[*n].path, 1100, "%s", full);
        out[*n].size = (uint64_t)st.st_size;
        (*n)++;
    }
    closedir(d);
}

static int cmp_file(const void *a, const void *b) {
    return strcmp(((const FileEntry *)a)->path, ((const FileEntry *)b)->path);
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

static uint64_t fnv1a(const uint8_t *p, size_t n) {
    uint64_t h = UINT64_C(0xCBF29CE484222325);
    for (size_t i = 0; i < n; i++) { h ^= p[i]; h *= UINT64_C(0x100000001B3); }
    return h;
}

/* process one file through the streaming chain.  Returns 1 if lossless. */
static int scan_file(const char *path, uint64_t *lifted_out, uint64_t *field_out,
                     uint64_t *teardowns_out, uint64_t *forced_out,
                     uint64_t *peak_out) {
    uint8_t *orig = NULL;
    uint64_t fsize = 0;
    if (read_file(path, &orig, &fsize) != 0) return -1;

    uint32_t nchunks = (uint32_t)((fsize + CHUNK_SZ - 1) / CHUNK_SZ);
    uint8_t *recon = (uint8_t *)malloc((size_t)fsize);
    uint8_t *lifted = (uint8_t *)malloc(WINDOW);
    CapAccount acc; cap_init(&acc);
    uint64_t lifted_n = 0, teardowns = 0, forced = 0, peak = 0;

    uint32_t placed = 0;
    while (placed < nchunks) {
        ResidualSpace rs; rs_init(&rs, WINDOW);
        uint32_t end = (placed + WINDOW < nchunks) ? placed + WINDOW : nchunks;

        /* place the window */
        memset(lifted, 0, WINDOW);
        for (uint32_t i = placed; i < end; i++) {
            uint8_t w = scale_w(i);
            uint32_t len = (uint32_t)((i == nchunks - 1)
                          ? (uint32_t)(fsize - (uint64_t)i * CHUNK_SZ) : CHUNK_SZ);
            if (cap_admit(&acc, 1.0, 0, w) == CAP_LIFT) {
                PoglsPiece p = ghost_piece(i, 0, w);
                uint64_t bk = rs_freeze(&rs, &p, orig + (uint64_t)i * CHUNK_SZ, len, 0);
                if (bk == RS_BOND_KEY_RESERVED) forced++;   /* table thrash */
                else { lifted[i - placed] = 1; lifted_n++; }
            }
            /* CAP_ADMIT → pointer-home (data stays in source) */
        }
        if (rs.count > peak) peak = rs.count;

        /* verify the window NOW (before teardown) + reconstruct */
        for (uint32_t i = placed; i < end; i++) {
            uint8_t w = scale_w(i);
            uint32_t len = (uint32_t)((i == nchunks - 1)
                          ? (uint32_t)(fsize - (uint64_t)i * CHUNK_SZ) : CHUNK_SZ);
            const uint8_t *src;
            if (lifted[i - placed]) {
                uint32_t out_sz = 0;
                PoglsPiece p = ghost_piece(i, 0, w);
                src = (const uint8_t *)rs_thaw(&rs, pogls_bond_key(&p), &out_sz);
                if (!src || out_sz != len) { rs_free(&rs); free(lifted); free(recon); free(orig); return 0; }
            } else {
                src = orig + (uint64_t)i * CHUNK_SZ;
            }
            memcpy(recon + (uint64_t)i * CHUNK_SZ, src, len);
        }
        rs_free(&rs);
        teardowns++;
        placed = end;
    }

    int ok = memcmp(recon, orig, (size_t)fsize) == 0;
    uint64_t csum_orig = fnv1a(orig, (size_t)fsize);
    uint64_t csum_recon = fnv1a(recon, (size_t)fsize);
    (void)csum_orig; (void)csum_recon;

    *lifted_out = lifted_n;
    *field_out = acc.used;
    *teardowns_out = teardowns;
    *forced_out = forced;
    *peak_out = peak;

    free(lifted); free(recon); free(orig);
    return ok;
}

int main(int argc, char **argv) {
    setvbuf(stdout, NULL, _IONBF, 0);
    const char *root = (argc > 1) ? argv[1] : "F:/notebookLM";

    printf("cap_chain_scan — streaming chain over %s\n", root);
    printf("════════════════════════════════════════════════════════════\n");

    FileEntry *files = (FileEntry *)malloc(MAX_FILES * sizeof(FileEntry));
    uint32_t nf = 0;
    walk_dir(root, files, MAX_FILES, &nf);
    qsort(files, nf, sizeof(FileEntry), cmp_file);

    uint64_t tot_bytes = 0, tot_chunks = 0, tot_lift = 0, tot_field_slots = 0;
    uint64_t tot_base_windows = 0, tot_teardowns = 0, tot_forced = 0, peak_rs = 0;
    uint32_t n_ok = 0, n_fail = 0, n_skip = 0;

    for (uint32_t f = 0; f < nf; f++) {
        uint64_t lifted = 0, field = 0, teardowns = 0, forced = 0, peak = 0;
        int r = scan_file(files[f].path, &lifted, &field, &teardowns, &forced, &peak);
        if (r < 0) { n_skip++; continue; }
        if (r) n_ok++; else n_fail++;

        uint32_t nchunks = (uint32_t)((files[f].size + CHUNK_SZ - 1) / CHUNK_SZ);
        printf("  %6.1f MB  %6u ch  field %7llu  %s  %s\n",
               (double)files[f].size / 1048576.0, nchunks,
               (unsigned long long)field,
               r ? "OK " : "FAIL",
               strrchr(files[f].path, '/') ? strrchr(files[f].path, '/') + 1
                                            : files[f].path);

        tot_bytes += files[f].size;
        tot_chunks += nchunks;
        tot_lift += lifted;
        tot_field_slots += field;
        tot_base_windows += (files[f].size + CAP_WIN - 1) / CAP_WIN;
        tot_teardowns += teardowns;
        tot_forced += forced;
        if (peak > peak_rs) peak_rs = peak;
        if ((f + 1) % 200 == 0) printf("  ... %u/%u files done\n", f + 1, nf);
    }

    printf("\n════════════════════════════════════════════════════════════\n");
    printf("AGGREGATE\n");
    printf("  files: %u (ok %u, fail %u, skip %u)\n", nf, n_ok, n_fail, n_skip);
    printf("  bytes: %llu MB | chunks: %llu (16 KB)\n",
           (unsigned long long)(tot_bytes >> 20), (unsigned long long)tot_chunks);
    printf("  base windows (naive chain): %llu\n",
           (unsigned long long)tot_base_windows);
    printf("  stream windows (16 MB bounded, teardowns): %llu\n",
           (unsigned long long)tot_teardowns);
    printf("  field slots (Σ envelope of admitted): %llu (~%llu windows)\n",
           (unsigned long long)tot_field_slots,
           (unsigned long long)((tot_field_slots + CAP_WIN - 1) / CAP_WIN));
    printf("  lifted chunks → residual_space: %llu\n",
           (unsigned long long)tot_lift);
    printf("  eviction pressure: peak rs.count %llu / window %u | forced evictions %llu\n",
           (unsigned long long)peak_rs, WINDOW, (unsigned long long)tot_forced);
    printf("  checksum: %u/%u files byte-for-byte\n", n_ok, n_ok + n_fail);
    printf("════════════════════════════════════════════════════════════\n");
    return n_fail ? 1 : 0;
}
