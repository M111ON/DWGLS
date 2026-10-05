/* tools/ann_pipe_real.c — ANN-storage pipe on REAL SIFT data.
 *
 * Same 4 stages as ann_pipe_bench.c (rank -> gate -> descend -> fault),
 * but: base/query vectors from *.fvecs, GT from *.ivecs, KVCB payload =
 * first 64 bytes of each base vector (byte-exact verify). Reports
 * recall@1/@10 vs GT plus per-stage us/q and QPS.
 *
 * Recall semantics: routed candidates (anchor TOPB buckets + Wang gate)
 * ranked by L2 -> top10 -> recall@10 vs GT[:10]; top1 vs GT[0] is
 * recall@1. Frustum descend + KVCB fault run on top1 (byte proof).
 *
 * BUILD: gcc -O2 -I. -Icore -o build/ann_pipe_real tools/ann_pipe_real.c -lpsapi -lm
 * RUN:   ./build/ann_pipe_real build/siftsmall/siftsmall/siftsmall_base.fvecs \
 *          build/siftsmall/siftsmall/siftsmall_query.fvecs \
 *          build/siftsmall/siftsmall/siftsmall_groundtruth.ivecs [--k 64] [--topb 4]
 */
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#ifdef _WIN32
#include <windows.h>
#include <psapi.h>
#endif

#include "core/kv_cold_base.h"
#include "core/geo_light_index.h"
#include "core/anchor_route.h"
#include "core/mm_wang.h"
#include "core/frustum_route.h"

#define BLKSZ   64
#define MAXN    1100000
#define MAXQ    10000
#define MAXK    512
#define MAXDIM  128
#define PATH    "build/ann_pipe_real.kvcb"

static uint64_t g_sink = 0;

static double pc_freq(void) {
#ifdef _WIN32
    LARGE_INTEGER f; QueryPerformanceFrequency(&f); return (double)f.QuadPart;
#else
    return (double)CLOCKS_PER_SEC;
#endif
}
static uint64_t pc_now(void) {
#ifdef _WIN32
    LARGE_INTEGER t; QueryPerformanceCounter(&t); return (uint64_t)t.QuadPart;
#else
    return (uint64_t)clock();
#endif
}

static float *g_base = NULL; static int g_nb = 0, g_dim = 0;
static float *g_q = NULL;    static int g_nq = 0;
static int *g_gt = NULL;     static int g_gtk = 0;

static int load_fvecs(const char *path, float **out, int *n, int *d) {
    FILE *f = fopen(path, "rb");
    if (!f) return -1;
    int32_t dim = 0;
    if (fread(&dim, 4, 1, f) != 1 || dim < 1 || dim > MAXDIM) { fclose(f); return -1; }
    fseek(f, 0, SEEK_END);
    long sz = ftell(f);
    int count = (int)(sz / (4 + (long)dim * 4));
    fseek(f, 0, SEEK_SET);
    float *X = (float *)malloc((size_t)count * dim * 4);
    if (!X) { fclose(f); return -1; }
    for (int i = 0; i < count; i++) {
        int32_t dd = 0;
        if (fread(&dd, 4, 1, f) != 1 || dd != dim) { free(X); fclose(f); return -1; }
        if (fread(X + (size_t)i * dim, 4, (size_t)dim, f) != (size_t)dim) { free(X); fclose(f); return -1; }
    }
    fclose(f);
    *out = X; *n = count; *d = dim;
    return 0;
}
static int load_ivecs(const char *path, int **out, int *n, int *k) {
    FILE *f = fopen(path, "rb");
    if (!f) return -1;
    int32_t kk = 0;
    if (fread(&kk, 4, 1, f) != 1 || kk < 1 || kk > 1000) { fclose(f); return -1; }
    fseek(f, 0, SEEK_END);
    long sz = ftell(f);
    int count = (int)(sz / (4 + (long)kk * 4));
    fseek(f, 0, SEEK_SET);
    int *G = (int *)malloc((size_t)count * kk * 4);
    if (!G) { fclose(f); return -1; }
    for (int i = 0; i < count; i++) {
        int32_t dd = 0;
        if (fread(&dd, 4, 1, f) != 1 || dd != kk) { free(G); fclose(f); return -1; }
        if (fread(G + (size_t)i * kk, 4, (size_t)kk, f) != (size_t)kk) { free(G); fclose(f); return -1; }
    }
    fclose(f);
    *out = G; *n = count; *k = kk;
    return 0;
}

static double dist2_cap(const float *a, const float *b, int d, double cap) {
    double s = 0;
    for (int j = 0; j < d; j++) {
        double e = (double)a[j] - b[j]; s += e * e;
        /* terms are >= 0, sum monotone: s >= cap now => full sum >= cap too,
         * so the candidate is rejected anyway — cut the rest of the loop */
        if (s >= cap) return s;
    }
    return s;
}
static MVNode bnode(uint32_t bucket, uint32_t slide) {
    MVNode n;
    n.node = bucket; n.slide = slide; n.layer = 0; n.entry = 0; n.exit = 0;
    return n;
}

int main(int argc, char **argv) {
    if (argc < 4) {
        printf("usage: ann_pipe_real base.fvecs query.fvecs gt.ivecs [--k K] [--topb T] [--mode 0|1|2|3]\n");
        return 2;
    }
    int KANCH = 64, TOPB = 4, MODE = 0, NQ = 1000;
    for (int i = 4; i < argc; i++) {
        if (!strcmp(argv[i], "--k") && i + 1 < argc) KANCH = atoi(argv[++i]);
        else if (!strcmp(argv[i], "--topb") && i + 1 < argc) TOPB = atoi(argv[++i]);
        else if (!strcmp(argv[i], "--mode") && i + 1 < argc) MODE = atoi(argv[++i]);
        else if (!strcmp(argv[i], "--nq") && i + 1 < argc) NQ = atoi(argv[++i]);
    }
    if (KANCH < 1 || KANCH > MAXK || TOPB < 1 || TOPB > KANCH) return 2;

    if (load_fvecs(argv[1], &g_base, &g_nb, &g_dim) != 0 || g_nb > MAXN) { printf("FAIL: base\n"); return 1; }
    int qd = 0;
    if (load_fvecs(argv[2], &g_q, &g_nq, &qd) != 0 || qd != g_dim || g_nq > MAXQ) { printf("FAIL: query\n"); return 1; }
    int gtn = 0;
    if (load_ivecs(argv[3], &g_gt, &gtn, &g_gtk) != 0 || gtn != g_nq || g_gtk < 10) { printf("FAIL: gt\n"); return 1; }
    printf("base=%d dim=%d nq=%d gtk=%d kanch=%d topb=%d\n", g_nb, g_dim, g_nq, g_gtk, KANCH, TOPB);

    /* ── KVCB: payload = first 64 bytes of each base vector ── */
    static uint8_t blob[MAXN * BLKSZ];
    for (int i = 0; i < g_nb; i++)
        memcpy(blob + (size_t)i * BLKSZ, g_base + (size_t)i * g_dim, BLKSZ);
    KVColdBase base; kvcb_init(&base);
    base.blob = blob; base.size = (uint64_t)g_nb * BLKSZ; base.n_tokens = 128; base.have = 1;
    if (kvcb_save(&base, PATH) != 0) { printf("FAIL: save\n"); return 1; }

    /* ── light index entries ── */
    static LIXEntry entries[MAXN];
    char nb[32];
    for (int i = 0; i < g_nb; i++) {
        snprintf(nb, sizeof(nb), "sift-%07d", i);
        entries[i].name_hash  = lix_hash(nb, (uint32_t)strlen(nb));
        entries[i].field_slot = lix_slot_of((uint32_t)i);
        entries[i].hj_cluster = lix_cluster_of(entries[i].field_slot);
        entries[i].store_off  = (uint64_t)sizeof(KVCBFileHdr) + (uint64_t)i * BLKSZ;
        entries[i].store_size = BLKSZ;
        entries[i].access     = (uint32_t)(i % 5);
        entries[i].flags      = 0;
    }
    static float C[MAXK * MAXDIM];
    static int lab[MAXN];
    if (anch_train(g_base, g_nb, g_dim, KANCH, C, lab) != 0) { printf("FAIL: train\n"); return 1; }
    mmw_clear();

    /* ── inverted bucket -> members (replaces O(N) gather scan) ── */
    static int inv_cnt[MAXK], inv_start[MAXK + 1], inv_perm[MAXN];
    for (int k = 0; k < KANCH; k++) inv_cnt[k] = 0;
    for (int i = 0; i < g_nb; i++) inv_cnt[lab[i]]++;
    inv_start[0] = 0;
    for (int k = 0; k < KANCH; k++) inv_start[k + 1] = inv_start[k] + inv_cnt[k];
    static int inv_fill[MAXK];
    for (int k = 0; k < KANCH; k++) inv_fill[k] = inv_start[k];
    for (int i = 0; i < g_nb; i++) inv_perm[inv_fill[lab[i]]++] = i;

    /* ── fault: mmap KVCB once (replaces fopen/fseek per query) ── */
#ifdef _WIN32
    HANDLE hF = CreateFileA(PATH, GENERIC_READ, FILE_SHARE_READ, NULL,
                            OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, NULL);
    if (hF == INVALID_HANDLE_VALUE) { printf("FAIL: open\n"); return 1; }
    HANDLE hM = CreateFileMappingA(hF, NULL, PAGE_READONLY, 0, 0, NULL);
    if (!hM) { CloseHandle(hF); printf("FAIL: map\n"); return 1; }
    const uint8_t *kmap = (const uint8_t *)MapViewOfFile(hM, FILE_MAP_READ, 0, 0, 0);
    if (!kmap) { CloseHandle(hM); CloseHandle(hF); printf("FAIL: view\n"); return 1; }
#else
    FILE *kvcb = fopen(PATH, "rb");
    if (!kvcb) { printf("FAIL: open\n"); return 1; }
#endif

    static int buckets[MAXK];
    static uint8_t span[BLKSZ];
    double freq = pc_freq();
    double t_rank = 0, t_gate = 0, t_desc = 0, t_fault = 0;
    long r1hit = 0, r10hit = 0;
    long gh_routed = 0, gh_pass = 0;
    long ad_bypass = 0;
    long top10pass = 0;
    int bad = 0;

    int nq_use = NQ < g_nq ? NQ : g_nq;
    for (int qi = 0; qi < nq_use; qi++) {
        const float *q = g_q + (size_t)qi * g_dim;
        uint32_t slide = ((uint32_t)(q[1] * 143.0f)) % 144u;

        uint64_t t0 = pc_now();
        int qb = anch_assign(q, C, KANCH, g_dim);
        int got = anch_route(q, C, KANCH, g_dim, TOPB, buckets);
        static int cand[MAXN]; int nc = 0;
        static int segstart[65];
        for (int t = 0; t < got; t++) {
            int b = buckets[t];
            segstart[t] = nc;
            for (int u = inv_start[b]; u < inv_start[b + 1] && nc < MAXN; u++)
                cand[nc++] = inv_perm[u];
        }
        segstart[got] = nc;
        if (MODE == 5) {
            /* forage visit order (locked rule): bucket-route order across
             * houses, id ascending within a bucket. Top-10 set is unchanged
             * by visit order (insertion is order-independent, cap is
             * recall-neutral); only early-exit rate moves. */
            for (int t = 0; t < got; t++) {
                int lo = segstart[t], hi = segstart[t + 1];
                for (int i = lo + 1; i < hi; i++) {
                    int k = cand[i], j = i - 1;
                    while (j >= lo && cand[j] > k) { cand[j + 1] = cand[j]; j--; }
                    cand[j + 1] = k;
                }
            }
        }
        t_rank += (double)(pc_now() - t0);

        /* gate + rank top10 */
        t0 = pc_now();
        MVNode qa = bnode((uint32_t)qb, slide);
        /* C: valve decided at the pipe entrance — one color pass per query,
         * candidate loop below only reads two precomputed bytes. */
        uint8_t qcol = mmw_color(qa.node, qa.layer, slide, MMW_EDGE_E);
        static uint8_t bcol[256];
        for (int b = 0; b < KANCH && b < 256; b++) {
            MVNode nb = bnode((uint32_t)b, slide);
            bcol[b] = mmw_color(nb.node, nb.layer, slide, MMW_EDGE_W);
        }
        if (MODE == 3) {
            /* admit: query E-dimension vs fixed reference (bucket 0 W) */
            MVNode ref = bnode(0u, slide);
            if (!mmw_open(&qa, MMW_EDGE_E, &ref, MMW_EDGE_W)) { ad_bypass++; continue; }
        }
        if (MODE == 2) {
            /* single-value flip: open this query's E edge, then normal path */
            mmw_clear();
            mmw_override(qa.node, MMW_EDGE_E, 1);
        }
        static int topi[10]; static double topd[10]; static int topp[10];
        for (int t = 0; t < 10; t++) { topi[t] = -1; topd[t] = 1e300; topp[t] = 0; }
        int ntop = 0;
        int gt0early = g_gt[(size_t)qi * g_gtk];
        for (int c = 0; c < nc; c++) {
            int i = cand[c];
            int pass = 1;
            if (MODE != 1 && lab[i] != qb) {
                if (MODE == 0 || MODE == 5) {
                    /* C (spec: never-traversed = open): valve opened once at the
                     * pipe entrance — pass is signature from precomputed bytes,
                     * never a filter. Modes 2/3/4 keep the live mmw_open path. */
                    pass = (lab[i] < 256) && (qcol == bcol[lab[i]]);
                } else {
                    MVNode bc = bnode((uint32_t)lab[i], slide);
                    pass = (lab[i] < 256) && mmw_open(&qa, MMW_EDGE_E, &bc, MMW_EDGE_W);
                }
            }
            if (i == gt0early) { gh_routed++; if (pass) gh_pass++; }
            if (!pass && MODE != 0 && MODE != 4 && MODE != 5) continue;
            /* early-exit: cap = worst of full top-10 (monotone sum), else no cap */
            double d = dist2_cap(q, g_base + (size_t)i * g_dim, g_dim,
                                 ntop == 10 ? topd[9] : 1e300);
            /* A: O(1) reject when full — d >= worst implies all 10 compares fail;
             * mode 4 equal-worst + pass preference kept as the sole exception. */
            int rej = 0;
            if (ntop == 10)
                rej = (d >= topd[9]) && !(MODE == 4 && d == topd[9] && pass && !topp[9]);
            for (int t = 0; !rej && t < 10; t++) {
                if (d < topd[t] || (MODE == 4 && d == topd[t] && pass && !topp[t])) {
                    for (int u = 9; u > t; u--) { topd[u] = topd[u-1]; topi[u] = topi[u-1]; topp[u] = topp[u-1]; }
                    topd[t] = d; topi[t] = i; topp[t] = pass;
                    ntop++;
                    break;
                }
            }
        }
        t_gate += (double)(pc_now() - t0);
        if (topi[0] < 0) { bad++; continue; }
        if (MODE == 4) { for (int t = 0; t < 10 && topi[t] >= 0; t++) top10pass += topp[t]; }

        int gt0 = g_gt[(size_t)qi * g_gtk];
        if (topi[0] == gt0) r1hit++;
        for (int t = 0; t < 10; t++) {
            for (int g = 0; g < 10; g++) {
                if (topi[t] == g_gt[(size_t)qi * g_gtk + g]) { r10hit++; break; }
            }
        }

        int best = topi[0];
        t0 = pc_now();
        FrRouteCache cache; fr_cache_init(&cache);
        FrustumRouteEvent ev;
        uint32_t pos = entries[best].field_slot;
        int okroute = 1;
        for (int d = 3; d >= 0; d--) {
            FrustumSeeker s;
            s.position = pos % 20736u;
            s.view_id = (uint8_t)(entries[best].hj_cluster % 8u);
            s.voronoi_mask = 0xFFFFFFu;
            s.frustum_depth = (uint8_t)(d + 1);
            if (!fr_route_produce(&s, &cache, &ev)) { okroute = 0; break; }
            pos = (pos + ev.branch_path) % 20736u;
        }
        t_desc += (double)(pc_now() - t0);
        if (!okroute) { bad++; continue; }

        t0 = pc_now();
        int ok = 0;
#ifdef _WIN32
        memcpy(span, kmap + entries[best].store_off, BLKSZ);
        ok = (memcmp(span, g_base + (size_t)best * g_dim, BLKSZ) == 0);
#else
        if (fseek(kvcb, (long)entries[best].store_off, SEEK_SET) == 0 &&
            fread(span, 1, BLKSZ, kvcb) == BLKSZ &&
            memcmp(span, g_base + (size_t)best * g_dim, BLKSZ) == 0) ok = 1;
#endif
        t_fault += (double)(pc_now() - t0);
        g_sink += span[0];
        if (!ok) bad++;
    }
#ifdef _WIN32
    UnmapViewOfFile(kmap); CloseHandle(hM); CloseHandle(hF);
#else
    fclose(kvcb);
#endif

    double tot = t_rank + t_gate + t_desc + t_fault;
    double us = tot / freq * 1e6 / nq_use;
    printf("mode=%d recall@1=%.4f recall@10avg=%.4f bad=%d bypassed=%ld\n", MODE,
           (double)r1hit / nq_use, (double)r10hit / nq_use / 10.0, bad, ad_bypass);
    printf("gt0 routed=%ld/%d gatepass|routed=%.4f\n",
           gh_routed, nq_use, gh_routed ? (double)gh_pass / gh_routed : -1.0);
    if (MODE == 4) printf("top10 gate-pass retained: %ld\n", top10pass);
    printf("rank=%.1f gate=%.1f desc=%.1f fault=%.1f us/q | total=%.1f us/q (%.0f q/s)\n",
           t_rank / freq * 1e6 / nq_use, t_gate / freq * 1e6 / nq_use,
           t_desc / freq * 1e6 / nq_use, t_fault / freq * 1e6 / nq_use,
           us, 1e6 / (us > 0 ? us : 1));
    printf("sink=%llu\n", (unsigned long long)g_sink);
    return bad ? 1 : 0;
}
