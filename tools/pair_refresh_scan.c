/* pair_refresh_scan.c — real cost of pair-table lazy refresh under write-heavy load
 * ═══════════════════════════════════════════════════════════════════════════
 * user: "Measure the real cost of lazy refresh under write-heavy load:
 *        interleave ghost_lift with ghost_read on the 7.7GB notebookLM stream
 *        with the pair table attached, and report how many rebuilds happen,
 *        their total cost, and the read latency distribution vs detached mode"
 *
 * Streams every file through the REAL chain (same as cap_chain_scan):
 *   file → chunks (16 KB) → per chunk: w = (37·rank)%144
 *     cap_admit(gate, 0, w)  → CAP_LIFT → ghost_lift (dirty flag set)
 *   interleave: BATCH lifts → BATCH ghost_reads (lazy refresh เกิดขึ้นที่นี่)
 *
 * Two modes ต่อ file:
 *   A — pair table attached  (ghost_read ใช้ตาราง, refresh เองเมื่อ dirty)
 *   B — detached            (ghost_read fallback binary search)
 *
 * วัด:
 *   - n_rebuilds            = จำนวน read ที่เจอ dirty → ต้อง rebuild
 *   - total rebuild cost    = Σ cycles ของ rebuild-reads − baseline fast-read
 *   - read latency dist     = min/p50/p95/max ของ A-fast, A-rebuild, B
 *   - lossless ทั้งสองโหมด (recon checksum)
 *
 * BUILD: gcc -O2 -I. -Icore -Icore/infra -o build/pair_refresh_scan \
 *        tools/pair_refresh_scan.c -lm
 * RUN:   build/pair_refresh_scan [root] [batch] [max_files] [parity]
 *        parity 0 = ไฟล์คู่ A-ก่อน (default) · 1 = ไฟล์คี่ A-ก่อน — ใช้
 *        สลับ parity รันซ้ำ 2 รอบ พิสูจน์ว่า wall Δ เป็น cache noise จริง
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <dirent.h>
#include <sys/stat.h>
#include <intrin.h>
#include <windows.h>
#include "../core/geo_cap_account.h"
#include "../core/geo_ghost_lift.h"

#define CHUNK_SZ 16384u
#define WINDOW   1024u      /* chunks per stream window (16 MB) */
#define MAX_FILES 50000u
#define MAX_READS 2000000u  /* latency samples cap (per mode) */

static inline uint64_t rdtsc_now(void) { return __rdtsc(); }
static inline double now_ms(void) {
    LARGE_INTEGER f, c;
    QueryPerformanceFrequency(&f);
    QueryPerformanceCounter(&c);
    return 1000.0 * (double)c.QuadPart / (double)f.QuadPart;
}

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

/* ── latency collection ── */
typedef struct {
    uint64_t *s;       /* cycles per read (raw) */
    uint64_t *tag;     /* 0 = fast, 1 = rebuild (mode A) */
    uint32_t  n;
    uint32_t  cap;
} Lat;

static void lat_init(Lat *l) {
    l->s = (uint64_t *)malloc(MAX_READS * sizeof(uint64_t));
    l->tag = (uint64_t *)malloc(MAX_READS * sizeof(uint64_t));
    l->n = 0; l->cap = MAX_READS;
}
static void lat_free(Lat *l) { free(l->s); free(l->tag); l->n = 0; }
static void lat_add(Lat *l, uint64_t cyc, uint64_t rebuild) {
    if (l->n >= l->cap) return;
    l->s[l->n] = cyc; l->tag[l->n] = rebuild; l->n++;
}
static int cmp_u64(const void *a, const void *b) {
    uint64_t x = *(const uint64_t *)a, y = *(const uint64_t *)b;
    return (x > y) - (x < y);
}
static void lat_report(const Lat *l, const char *name, uint64_t *rebuilds_out,
                       uint64_t *rebuild_cyc_out, uint64_t *fast_baseline_out) {
    if (l->n == 0) { printf("  %-22s (no samples)\n", name); return; }
    uint64_t *sorted = (uint64_t *)malloc(l->n * sizeof(uint64_t));
    uint64_t *rsorted = (uint64_t *)malloc(l->n * sizeof(uint64_t));
    uint32_t nf = 0, nr = 0;
    uint64_t sum_f = 0, sum_r = 0;
    for (uint32_t i = 0; i < l->n; i++) {
        if (l->tag[i] == 0) { sorted[nf++] = l->s[i]; sum_f += l->s[i]; }
        else                { rsorted[nr++] = l->s[i]; sum_r += l->s[i]; }
    }
    if (nf) qsort(sorted, nf, sizeof(uint64_t), cmp_u64);
    if (nr) qsort(rsorted, nr, sizeof(uint64_t), cmp_u64);
    uint64_t p50_f = nf ? sorted[nf / 2] : 0, p95_f = nf ? sorted[nf * 95 / 100] : 0;
    uint64_t p50_r = nr ? rsorted[nr / 2] : 0, p95_r = nr ? rsorted[nr * 95 / 100] : 0;

    printf("  %-22s reads %-9u fast %u (min %I64u / p50 %I64u / p95 %I64u cyc, mean %I64u)\n",
           name, l->n, nf,
           (unsigned long long)(nf ? sorted[0] : 0),
           (unsigned long long)p50_f, (unsigned long long)p95_f,
           (unsigned long long)(nf ? sum_f / nf : 0));
    printf("  %-22s rebuild reads %u (min %I64u / p50 %I64u / p95 %I64u cyc, mean %I64u)\n",
           "", nr,
           (unsigned long long)(nr ? rsorted[0] : 0),
           (unsigned long long)p50_r, (unsigned long long)p95_r,
           (unsigned long long)(nr ? sum_r / nr : 0));

    if (rebuilds_out) *rebuilds_out = nr;
    if (rebuild_cyc_out) {
        /* rebuild cost ≈ Σ(rebuild-read) − Σ(baseline ถ้าเป็น fast-read ทั้งหมด)
           baseline = mean fast × n_rebuilds */
        uint64_t baseline = nf ? sum_f / nf : 0;
        *rebuild_cyc_out = nr ? sum_r - (uint64_t)nr * baseline : 0;
    }
    if (fast_baseline_out) *fast_baseline_out = nf ? sum_f / nf : 0;
    free(sorted); free(rsorted);
}

static uint64_t g_builds = 0, g_skips = 0;   /* จำนวน build/skip จริง */

/* ── stream one file in one mode.  Returns 1 if lossless. ── */
static int stream_file(const char *path, uint32_t batch, int attached,
                       Lat *lat, uint64_t *lifts_out, uint64_t *reads_out,
                       double *lift_ms_out, double *read_ms_out) {
    double t_lift = 0.0, t_read = 0.0;
    uint8_t *orig = NULL;
    uint64_t fsize = 0;
    if (read_file(path, &orig, &fsize) != 0) return -1;

    uint32_t nchunks = (uint32_t)((fsize + CHUNK_SZ - 1) / CHUNK_SZ);
    uint8_t *recon = (uint8_t *)malloc((size_t)fsize);
    if (!recon) { free(orig); return -1; }
    uint64_t lifted_n = 0, reads_n = 0;
    int ok = 1;

    uint32_t placed = 0;
    while (placed < nchunks) {
        GhostLog log;  ghost_log_init(&log);
        ResidualSpace rs; rs_init(&rs, WINDOW);
        GhostPairTable t; memset(&t, 0, sizeof(t));
        if (attached) ghost_pair_attach(&log, &t);
        CapAccount acc; cap_init(&acc);

        uint32_t end = (placed + WINDOW < nchunks) ? placed + WINDOW : nchunks;

        /* BATCH lifts → BATCH reads (interleave at batch granularity) */
        uint32_t base = placed;
        while (base < end) {
            uint32_t b_end = (base + batch < end) ? base + batch : end;

            /* write phase: lift chunks ที่ cap_admit บอก CAP_LIFT */
            uint8_t *lifted = (uint8_t *)calloc(b_end - base, 1);
            double tw0 = now_ms();
            for (uint32_t i = base; i < b_end; i++) {
                uint8_t w = scale_w(i);
                uint32_t len = (uint32_t)((i == nchunks - 1)
                              ? (uint32_t)(fsize - (uint64_t)i * CHUNK_SZ) : CHUNK_SZ);
                if (cap_admit(&acc, 1.0, 0, w) == CAP_LIFT) {
                    if (ghost_lift(&log, &rs, (uint16_t)i, 0, w,
                                   orig + (uint64_t)i * CHUNK_SZ, len)
                        != RS_BOND_KEY_RESERVED) {
                        lifted[i - base] = 1; lifted_n++;
                    }
                }
            }

            t_lift += now_ms() - tw0;
            /* read phase: อ่านกลับทันที (mode A: refresh เกิดที่นี้) */
            double tr0 = now_ms();
            for (uint32_t i = base; i < b_end; i++) {
                uint8_t w = scale_w(i);
                uint32_t len = (uint32_t)((i == nchunks - 1)
                              ? (uint32_t)(fsize - (uint64_t)i * CHUNK_SZ) : CHUNK_SZ);
                const uint8_t *src;
                if (lifted[i - base]) {
                    uint32_t out_sz = 0;
                    /* นับ build/skip จริง: ก่อน read ถ้า dirty → refresh จะ
                       build (0) หรือ skip (1) — จำก่อน แล้วเปรียบเทียบหลัง */
                    uint32_t dirty_before = (attached && t.dirty) ? 1u : 0u;
                    uint32_t pile_before = (attached && t.pile) ? 1u : 0u;
                    uint64_t t0 = rdtsc_now();
                    const void *got = ghost_read(&log, &rs, (uint16_t)i, 0, w, &out_sz);
                    uint64_t t1 = rdtsc_now();
                    if (attached && dirty_before && !pile_before) g_builds++;
                    if (attached && dirty_before && pile_before && t.dirty) g_skips++;
                    uint64_t was_dirty = dirty_before;
                    lat_add(lat, t1 - t0, was_dirty);
                    reads_n++;
                    if (!got || out_sz != len) { ok = 0; break; }
                    src = (const uint8_t *)got;
                } else {
                    src = orig + (uint64_t)i * CHUNK_SZ;
                }
                if (ok) memcpy(recon + (uint64_t)i * CHUNK_SZ, src, len);
            }
            t_read += now_ms() - tr0;
            free(lifted);
            if (!ok) break;
            base = b_end;
        }

        if (attached) ghost_pair_detach(&log);
        rs_free(&rs);
        if (!ok) break;
        placed = end;
    }

    if (ok) ok = (memcmp(recon, orig, (size_t)fsize) == 0);
    *lifts_out = lifted_n;
    *reads_out = reads_n;
    if (lift_ms_out) *lift_ms_out = t_lift;
    if (read_ms_out) *read_ms_out = t_read;
    free(recon); free(orig);
    return ok;
}

int main(int argc, char **argv) {
    setvbuf(stdout, NULL, _IONBF, 0);
    const char *root = (argc > 1) ? argv[1] : "F:/notebookLM";
    uint32_t batch = (argc > 2) ? (uint32_t)atoi(argv[2]) : 4u;
    uint32_t max_files = (argc > 3) ? (uint32_t)atoi(argv[3]) : 0u;
    int parity = (argc > 4) ? atoi(argv[4]) : 0;   /* 0: even A-first, 1: odd A-first */

    printf("pair_refresh_scan — lazy refresh cost ภายใต้ write-heavy load\n");
    printf("════════════════════════════════════════════════════════════\n");
    printf("root %s | batch %u lifts→%u reads | %s\n", root, batch, batch,
           max_files ? "max-files limit" : "full folder");

    FileEntry *files = (FileEntry *)malloc(MAX_FILES * sizeof(FileEntry));
    uint32_t nf = 0;
    walk_dir(root, files, MAX_FILES, &nf);
    qsort(files, nf, sizeof(FileEntry), cmp_file);
    if (max_files && nf > max_files) nf = max_files;

    Lat latA, latB;
    lat_init(&latA); lat_init(&latB);

    uint64_t A_lifts = 0, A_reads = 0, B_lifts = 0, B_reads = 0;
    uint32_t nA_ok = 0, nB_ok = 0, n_skip = 0;
    uint64_t tot_bytes = 0;
    double wallA = 0.0, wallB = 0.0;
    double liftA = 0.0, readA = 0.0, liftB = 0.0, readB = 0.0;

    for (uint32_t f = 0; f < nf; f++) {
        /* สลับลำดับ A/B ต่อไฟล์ — กัน OS file-cache bias: ตาม parity
           (0: ไฟล์คู่ A-ก่อน · 1: ไฟล์คี่ A-ก่อน) — รัน parity กลับกัน 2 รอบ
           เพื่อพิสูจน์ว่า wall Δ เป็น cache noise ไม่ใช่ cost ของตาราง */
        int order[2];
        if ((f + parity) % 2 == 0) { order[0] = 1; order[1] = 0; }
        else                       { order[0] = 0; order[1] = 1; }

        for (int p = 0; p < 2; p++) {
            int isA = order[p];
            uint64_t lf = 0, rd = 0;
            double tl = 0, tr = 0;
            double t0 = now_ms();
            int r = stream_file(files[f].path, batch, isA,
                                isA ? &latA : &latB, &lf, &rd, &tl, &tr);
            if (isA) {
                wallA += now_ms() - t0;
                liftA += tl; readA += tr;
                if (r < 0) { n_skip++; goto next_file; }
                if (r) nA_ok++; else printf("  (A lossless FAIL: %s)\n", files[f].path);
                A_lifts += lf; A_reads += rd;
            } else {
                wallB += now_ms() - t0;
                liftB += tl; readB += tr;
                if (r < 0) { n_skip++; goto next_file; }
                if (r > 0) nB_ok++; else printf("  (B lossless FAIL: %s)\n", files[f].path);
                B_lifts += lf; B_reads += rd;
            }
        }
        tot_bytes += files[f].size;
next_file: ;

        if ((f + 1) % 100 == 0) {
            uint64_t rebuilds_so_far = 0;
            for (uint32_t i = 0; i < latA.n; i++) rebuilds_so_far += latA.tag[i];
            printf("  ... %u/%u files (%.0f MB) | A rebuilds so far %I64u\n",
                   f + 1, nf, (double)tot_bytes / 1048576.0,
                   (unsigned long long)rebuilds_so_far);
        }
    }

    printf("\n════════════════════════════════════════════════════════════\n");
    printf("files %u (ok A %u / ok B %u / skip %u) | bytes %.1f MB | batch %u\n",
           nf, nA_ok, nB_ok, n_skip, (double)tot_bytes / 1048576.0, batch);
    printf("lifts: A %I64u | B %I64u | reads: A %I64u | B %I64u\n",
           (unsigned long long)A_lifts, (unsigned long long)B_lifts,
           (unsigned long long)A_reads, (unsigned long long)B_reads);
    printf("wall: A (attached) %.1f ms | B (detached) %.1f ms | Δ %.1f ms (%.1f%%)\n",
           wallA, wallB, wallA - wallB,
           100.0 * (wallA - wallB) / (wallB ? wallB : 1.0));
    printf("  A: lift %.1f ms | read %.1f ms | other %.1f ms\n",
           liftA, readA, wallA - liftA - readA);
    printf("  B: lift %.1f ms | read %.1f ms | other %.1f ms\n",
           liftB, readB, wallB - liftB - readB);

    uint64_t n_rebuilds = 0, rebuild_cyc = 0, fast_base = 0;
    printf("\n── MODE A — pair table attached (lazy refresh) ──\n");
    lat_report(&latA, "A reads", &n_rebuilds, &rebuild_cyc, &fast_base);
    printf("\n── MODE B — detached (binary search fallback) ──\n");
    lat_report(&latB, "B reads", NULL, NULL, NULL);

    uint64_t meanB = 0, sumB = 0;
    for (uint32_t i = 0; i < latB.n; i++) sumB += latB.s[i];
    meanB = latB.n ? sumB / latB.n : 0;
    uint64_t sumA = 0;
    for (uint32_t i = 0; i < latA.n; i++) sumA += latA.s[i];
    uint64_t meanA = latA.n ? sumA / latA.n : 0;

    printf("\n════════════════════════════════════════════════════════════\n");
    printf("ACTUAL refresh: builds %I64u | skips %I64u (dirty-but-binary)\n",
           (unsigned long long)g_builds, (unsigned long long)g_skips);
    printf("REBUILDS: %I64u (%.1f%% ของ reads — batch %u → 1 rebuild ต่อ batch)\n",
           (unsigned long long)n_rebuilds,
           latA.n ? 100.0 * (double)n_rebuilds / (double)latA.n : 0.0, batch);
    printf("TOTAL REBUILD COST: %I64u cycles ≈ %.3f ms (at ~3.5 GHz)\n",
           (unsigned long long)rebuild_cyc, (double)rebuild_cyc / 3.5e9 * 1e3);
    printf("  = %.1f× ของ fast-read baseline (fast mean %I64u cyc × %I64u reads)\n",
           (double)rebuild_cyc / (double)(fast_base ? fast_base * n_rebuilds : 1),
           (unsigned long long)fast_base, (unsigned long long)n_rebuilds);
    printf("READ LATENCY: A mean %I64u cyc | B mean %I64u cyc (A/B = %.2f)\n",
           (unsigned long long)meanA, (unsigned long long)meanB,
           meanB ? (double)meanA / (double)meanB : 0.0);
    printf("LOSSLESS: A %u/%u | B %u/%u — byte-for-byte\n",
           nA_ok, nf - n_skip, nB_ok, nf - n_skip);

    lat_free(&latA); lat_free(&latB);
    free(files);
    printf("════════════════════════════════════════════════════════════\n");
    return (nA_ok != nf - n_skip || nB_ok != nf - n_skip) ? 1 : 0;
}
