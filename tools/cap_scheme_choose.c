/* cap_scheme_choose.c — adaptive scheme decision on a real folder
 * ═══════════════════════════════════════════════════════════════════════════
 * Computes both placement schemes' field cost for a whole folder (default
 * F:/notebookLM — 7.7 GB, 1,035 files) and picks:
 *
 *   PER_FILE — every file restarts ranks at 0 → first chunk at w=0 full
 *              price (ค่าแรกเข้า × จำนวนไฟล์)
 *   GLOBAL   — one rank sequence + targeted assignment (§15.33) → field
 *              ranks get the smallest chunks
 *
 * Uses REAL chunk sizes (including partial last chunks), size model
 * (1 byte = 1 slot), gate 1.0 (k_max = 5).  Decision at margins 0/50/100.
 *
 * BUILD: gcc -O2 -I. -Icore -Icore/infra -o build/cap_scheme_choose tools/cap_scheme_choose.c -lm
 * RUN:   build/cap_scheme_choose [root]
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <dirent.h>
#include <sys/stat.h>
#include "../core/geo_placement_choose.h"
#include "../core/geo_ghost_envelope.h"

#define CHUNK_SZ 16384u
#define MAX_FILES 50000u
#define MAX_CHUNKS 2000000u

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

static int cmp_u64(const void *a, const void *b) {
    uint64_t x = *(const uint64_t *)a, y = *(const uint64_t *)b;
    return (x > y) - (x < y);
}

int main(int argc, char **argv) {
    setvbuf(stdout, NULL, _IONBF, 0);
    const char *root = (argc > 1) ? argv[1] : "F:/notebookLM";

    printf("Adaptive scheme decision — %s\n", root);
    printf("════════════════════════════════════════════════════════\n");

    FileEntry *files = (FileEntry *)malloc(MAX_FILES * sizeof(FileEntry));
    uint32_t nf = 0;
    walk_dir(root, files, MAX_FILES, &nf);

    uint64_t *all = (uint64_t *)malloc(MAX_CHUNKS * sizeof(uint64_t));
    uint64_t N = 0, per_file = 0, tot_bytes = 0;

    for (uint32_t f = 0; f < nf; f++) {
        uint64_t sz = files[f].size;
        tot_bytes += sz;
        uint32_t nchunks = (uint32_t)((sz + CHUNK_SZ - 1) / CHUNK_SZ);

        /* per-file cost — exact, real chunk sizes */
        uint64_t pf_file = 0;
        for (uint32_t r = 0; r < nchunks; r++) {
            uint64_t cs = (r == nchunks - 1)
                        ? sz - (uint64_t)r * CHUNK_SZ : CHUNK_SZ;
            uint8_t w = pc_scale_w(r);
            if (w <= 5u) pf_file += pc_view_of(cs, w);
            all[N++] = cs;
        }
        per_file += pf_file;
    }

    /* global — sort all chunk sizes, targeted assignment */
    qsort(all, N, sizeof(uint64_t), cmp_u64);
    uint64_t global = pc_global_cost(all, (uint32_t)N, 5u);

    printf("  files: %u | bytes: %llu MB | chunks: %llu\n",
           nf, (unsigned long long)(tot_bytes >> 20), (unsigned long long)N);
    printf("  k_max (gate 1.0) = %u | chunk = %u slots\n\n", 5u, CHUNK_SZ);
    printf("  per-file field cost : %llu slots (~%llu windows)\n",
           (unsigned long long)per_file, (unsigned long long)((per_file + 20735) / 20736));
    printf("  global   field cost : %llu slots (~%llu windows)\n",
           (unsigned long long)global, (unsigned long long)((global + 20735) / 20736));
    if (global > 0)
        printf("  ratio per-file/global: %.1f×\n", (double)per_file / (double)global);

    printf("\n  decision (margin_pct = 0  → %s)\n",
           pc_choose(per_file, global, 0)  == PC_SCHEME_GLOBAL ? "GLOBAL" : "PER_FILE");
    printf("  decision (margin_pct = 50 → %s)  [default]\n",
           pc_choose(per_file, global, 50) == PC_SCHEME_GLOBAL ? "GLOBAL" : "PER_FILE");
    printf("  decision (margin_pct = 100→ %s)\n",
           pc_choose(per_file, global, 100) == PC_SCHEME_GLOBAL ? "GLOBAL" : "PER_FILE");

    uint64_t saved = per_file > global ? per_file - global : 0;
    if (saved > 0)
        printf("  switching saves %llu slots (~%llu windows)\n",
               (unsigned long long)saved,
               (unsigned long long)((saved + 20735) / 20736));

    free(all); free(files);
    printf("════════════════════════════════════════════════════════\n");
    return 0;
}
