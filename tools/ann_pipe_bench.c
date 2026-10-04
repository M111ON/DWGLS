/* tools/ann_pipe_bench.c — end-to-end speed of the ANN-storage pipe.
 *
 * Stages per query (the whole assembly, fixtures + real KVCB file):
 *   1. rank   : anch_route top-2 over light-index features
 *   2. gate   : Wang E->W facing (own bucket always live)
 *   3. descend: frustum 3->0 exit->enter chain from top entry's slot
 *   4. fault  : entry (off,size) fread from the KVCB file + verify
 *
 * Reports per-stage us/q, total us/q, QPS, bytes, page faults, peak WS.
 * Bench, not gate: exit 0 unless bytes mismatch (correctness is gated
 * elsewhere). Cold pass drops the working set first; warm repeats it.
 *
 * BUILD: gcc -O2 -I. -Icore -o build/ann_pipe_bench tools/ann_pipe_bench.c -lpsapi -lm
 * RUN:   ./build/ann_pipe_bench [--n N] [--cold-only|--warm-only]
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

#define NBLK    32
#define BLKSZ   64
#define DIM     4
#define KANCH   4
#define TOPB    2
#define PATH    "build/ann_pipe.kvcb"

static uint64_t g_sink = 0;

static uint64_t faults_now(void) {
#ifdef _WIN32
    PROCESS_MEMORY_COUNTERS pmc;
    if (!GetProcessMemoryInfo(GetCurrentProcess(), &pmc, sizeof(pmc))) return 0;
    return pmc.PageFaultCount;
#else
    return 0;
#endif
}
static size_t ws_now(void) {
#ifdef _WIN32
    PROCESS_MEMORY_COUNTERS pmc;
    if (!GetProcessMemoryInfo(GetCurrentProcess(), &pmc, sizeof(pmc))) return 0;
    return pmc.WorkingSetSize;
#else
    return 0;
#endif
}
static void drop_ws(void) {
#ifdef _WIN32
    EmptyWorkingSet(GetCurrentProcess());
#endif
}
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

static double dist2(const float *a, const float *b) {
    double d = 0;
    for (int j = 0; j < DIM; j++) { double e = (double)a[j] - b[j]; d += e * e; }
    return d;
}
static MVNode bnode(uint32_t bucket, uint32_t slide) {
    MVNode n;
    n.node = bucket; n.slide = slide; n.layer = 0; n.entry = 0; n.exit = 0;
    return n;
}

int main(int argc, char **argv) {
    int N = 200, cold = 1, warm = 1;
    for (int i = 1; i < argc; i++) {
        if (!strcmp(argv[i], "--n") && i + 1 < argc) N = atoi(argv[++i]);
        else if (!strcmp(argv[i], "--cold-only")) warm = 0;
        else if (!strcmp(argv[i], "--warm-only")) cold = 0;
    }

    /* ── fixture KVCB file ── */
    static uint8_t blob[NBLK * BLKSZ];
    for (uint32_t i = 0; i < NBLK; i++)
        for (uint32_t j = 0; j < BLKSZ; j++)
            blob[(size_t)i * BLKSZ + j] = (uint8_t)((i * 37u + j * 11u + 3u) & 0xFFu);
    KVColdBase base; kvcb_init(&base);
    base.blob = blob; base.size = sizeof(blob); base.n_tokens = 128; base.have = 1;
    if (kvcb_save(&base, PATH) != 0) { printf("FAIL: save\n"); return 1; }

    /* ── light index + anchors ── */
    static LIXEntry entries[NBLK];
    char nb[32];
    for (uint32_t i = 0; i < NBLK; i++) {
        snprintf(nb, sizeof(nb), "pipe-%05u", i);
        entries[i].name_hash  = lix_hash(nb, (uint32_t)strlen(nb));
        entries[i].field_slot = lix_slot_of(i * 101u + 7u);
        entries[i].hj_cluster = lix_cluster_of(entries[i].field_slot);
        entries[i].store_off  = (uint64_t)sizeof(KVCBFileHdr) + (uint64_t)i * BLKSZ;
        entries[i].store_size = BLKSZ;
        entries[i].access     = i % 5u;
        entries[i].flags      = 0;
    }
    static float X[NBLK * DIM];
    for (uint32_t i = 0; i < NBLK; i++) {
        X[(size_t)i * DIM + 0] = (float)entries[i].field_slot / 20736.0f;
        X[(size_t)i * DIM + 1] = (float)entries[i].hj_cluster / 144.0f;
        X[(size_t)i * DIM + 2] = (float)entries[i].access / 64.0f;
        X[(size_t)i * DIM + 3] = 0.5f;
    }
    static float C[KANCH * DIM];
    static int lab[NBLK];
    if (anch_train(X, NBLK, DIM, KANCH, C, lab) != 0) { printf("FAIL: train\n"); return 1; }
    mmw_clear();

    static float q[DIM];
    static int buckets[KANCH];
    static uint8_t span[BLKSZ];
    uint32_t qrng = 0xbeef01ul;
    double freq = pc_freq();

    for (int pass = 0; pass < (cold && warm ? 2 : 1); pass++) {
        int is_cold = cold ? (pass == 0) : 0;
        if (!cold && !warm) break;
        if (cold && warm) { if (is_cold) { drop_ws(); Sleep(50); } }
        else if (cold) { drop_ws(); Sleep(50); }
        uint64_t f0 = faults_now();
        double t_rank = 0, t_gate = 0, t_desc = 0, t_fault = 0;
        uint64_t bytes = 0;
        int bad = 0;

        for (int qi = 0; qi < N; qi++) {
            qrng ^= qrng << 13; qrng ^= qrng >> 17; qrng ^= qrng << 5;
            uint32_t src = qrng % NBLK;
            for (int j = 0; j < DIM; j++) q[j] = X[(size_t)src * DIM + j];
            uint32_t slide = ((uint32_t)(q[1] * 143.0f)) % 144u;

            uint64_t t0 = pc_now();
            int qb = anch_assign(q, C, KANCH, DIM);
            int got = anch_route(q, C, KANCH, DIM, TOPB, buckets);
            int cand[64]; int nc = 0;
            for (int t = 0; t < got; t++)
                for (uint32_t i = 0; i < NBLK && nc < 64; i++)
                    if (lab[i] == buckets[t]) cand[nc++] = (int)i;
            t_rank += (double)(pc_now() - t0);

            t0 = pc_now();
            MVNode qa = bnode((uint32_t)qb, slide);
            int best = -1; double bd = 1e300;
            for (int c = 0; c < nc; c++) {
                int i = cand[c];
                if (lab[i] != qb) {
                    MVNode bc = bnode((uint32_t)lab[i], slide);
                    if (!mmw_open(&qa, MMW_EDGE_E, &bc, MMW_EDGE_W)) continue;
                }
                double d = dist2(q, X + (size_t)i * DIM);
                if (d < bd) { bd = d; best = i; }
            }
            t_gate += (double)(pc_now() - t0);
            if (best < 0) { bad++; continue; }

            t0 = pc_now();
            FrRouteCache cache; fr_cache_init(&cache);
            FrustumRouteEvent ev;
            uint32_t pos = entries[best].field_slot;
            for (int d = 3; d >= 0; d--) {
                FrustumSeeker s;
                s.position = pos % 20736u;
                s.view_id = (uint8_t)(entries[best].hj_cluster % 8u);
                s.voronoi_mask = 0xFFFFFFu;
                s.frustum_depth = (uint8_t)(d + 1);
                if (!fr_route_produce(&s, &cache, &ev)) { best = -2; break; }
                pos = (pos + ev.branch_path) % 20736u;
            }
            t_desc += (double)(pc_now() - t0);
            if (best == -2) { bad++; continue; }

            t0 = pc_now();
            FILE *f = fopen(PATH, "rb");
            int ok = 0;
            if (f) {
                if (fseek(f, (long)entries[best].store_off, SEEK_SET) == 0 &&
                    fread(span, 1, BLKSZ, f) == BLKSZ) {
                    ok = 1;
                    for (uint32_t j = 0; j < BLKSZ; j++) {
                        uint8_t want = (uint8_t)(((uint32_t)best * 37u + j * 11u + 3u) & 0xFFu);
                        if (span[j] != want) { ok = 0; break; }
                    }
                }
                fclose(f);
            }
            t_fault += (double)(pc_now() - t0);
            bytes += BLKSZ;
            g_sink += span[0];
            if (!ok) bad++;
        }
        uint64_t df = faults_now() - f0;
        double us = (t_rank + t_gate + t_desc + t_fault) / freq * 1e6 / N;
        printf("  %s: n=%d bad=%d | rank=%.1f gate=%.1f desc=%.1f fault=%.1f us/q | total=%.1f us/q (%.0f q/s) | bytes=%llu faults=+%llu peak_ws=%.1f MB\n",
               is_cold ? "cold" : "warm", N, bad,
               t_rank / freq * 1e6 / N, t_gate / freq * 1e6 / N,
               t_desc / freq * 1e6 / N, t_fault / freq * 1e6 / N,
               us, 1e6 / (us > 0 ? us : 1),
               (unsigned long long)bytes, (unsigned long long)df,
               (double)ws_now() / 1048576.0);
        if (bad) { printf("FAIL: %d bad queries\n", bad); return 1; }
    }
    printf("sink=%llu\n", (unsigned long long)g_sink);
    return 0;
}