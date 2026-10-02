/* P0/P1 semantic-to-geometry vertical slice.
 *
 * P0: deterministic anchors assign RouteTags, HyperJump resolves the selected
 * field position, and payload bytes stay in a separate array.
 * P1: compare brute-force exact search with anchor-routed exact search.
 * This is a small measurement harness, not a production ANN index.
 */
#include <stdio.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#include "anchor_route.h"
#include "geo_hyper_resolve.h"

#define N 1000
#define DIM 8
#define K 10
#define TOPB 2
#define PAYLOAD_BYTES 32
#define QUERIES 1000

typedef struct {
    uint32_t identity;
    uint32_t world;
    uint32_t route_tag;
    uint32_t field_pos;
    uint8_t bytes[PAYLOAD_BYTES];
} Entry;

static uint32_t rng_state = 0x6d2b79f5u;
static uint32_t next_u32(void) {
    rng_state ^= rng_state << 13;
    rng_state ^= rng_state >> 17;
    rng_state ^= rng_state << 5;
    return rng_state;
}

static float noise(void) {
    return (float)(next_u32() % 2001u) / 10000.0f - 0.1f;
}

static double distance_sq(const float *a, const float *b) {
    double d = 0.0;
    for (int j = 0; j < DIM; j++) {
        double e = (double)a[j] - b[j];
        d += e * e;
    }
    return d;
}

static int brute_best(const float *q, const float *x) {
    int best = -1;
    double bd = 1e300;
    for (int i = 0; i < N; i++) {
        double d = distance_sq(q, x + (size_t)i * DIM);
        if (d < bd) { bd = d; best = i; }
    }
    return best;
}

static int routed_best(const float *q, const float *x, const float *centroids,
                       const int *labels, int *candidate_count, int *jump_ops) {
    int tags[TOPB];
    int got = anch_route(q, centroids, K, DIM, TOPB, tags);
    int best = -1;
    double bd = 1e300;
    *candidate_count = 0;
    *jump_ops = 0;
    for (int i = 0; i < N; i++) {
        int accepted = 0;
        for (int t = 0; t < got; t++) if (labels[i] == tags[t]) accepted = 1;
        if (!accepted) continue;
        (*candidate_count)++;
        uint32_t p = (uint32_t)i % HJ_TOTAL;
        HjCell cell = hjr_resolve(p, HS_MODE48);
        if (hjr_point(cell, HS_MODE48) != p) return -2;
        (*jump_ops)++;
        double d = distance_sq(q, x + (size_t)i * DIM);
        if (d < bd) { bd = d; best = i; }
    }
    return best;
}

static int payload_ok(const Entry *e) {
    for (int j = 0; j < PAYLOAD_BYTES; j++)
        if (e->bytes[j] != (uint8_t)((e->identity * 17u + (uint32_t)j * 13u) & 255u)) return 0;
    return 1;
}

int main(void) {
    float *x = (float *)malloc((size_t)N * DIM * sizeof(float));
    float *c = (float *)malloc((size_t)K * DIM * sizeof(float));
    int *labels = (int *)malloc((size_t)N * sizeof(int));
    Entry *entries = (Entry *)calloc(N, sizeof(Entry));
    if (!x || !c || !labels || !entries) return 2;

    /* Ten separated deterministic semantic regions. */
    for (int i = 0; i < N; i++) {
        int region = i % K;
        for (int j = 0; j < DIM; j++)
            x[(size_t)i * DIM + j] = (float)region * 4.0f + (float)j * 0.03f + noise();
        entries[i].identity = (uint32_t)i;
        entries[i].world = (uint32_t)(i % 3);
        entries[i].field_pos = (uint32_t)i;
        for (int j = 0; j < PAYLOAD_BYTES; j++)
            entries[i].bytes[j] = (uint8_t)((i * 17 + j * 13) & 255);
    }

    int ok = anch_train(x, N, DIM, K, c, labels) == 0;
    if (ok) for (int i = 0; i < N; i++) {
        entries[i].route_tag = (uint32_t)labels[i];
        if (!payload_ok(&entries[i])) ok = 0;
        HjCell cell = hjr_resolve(entries[i].field_pos % HJ_TOTAL, HS_MODE48);
        if (hjr_point(cell, HS_MODE48) != entries[i].field_pos % HJ_TOTAL) ok = 0;
    }
    printf("P0 vertical slice: %s (N=%d, RouteTags=%d, worlds=3, payload=separate)\n",
           ok ? "PASS" : "FAIL", N, K);
    if (!ok) { free(x); free(c); free(labels); free(entries); return 1; }

    clock_t brute_ticks = 0, route_ticks = 0;
    long total_candidates = 0, total_jumps = 0;
    int hits = 0;
    for (int qi = 0; qi < QUERIES; qi++) {
        int source = (qi * 37) % N;
        const float *q = x + (size_t)source * DIM;
        clock_t start = clock();
        int exact = brute_best(q, x);
        brute_ticks += clock() - start;
        int candidates = 0, jumps = 0;
        start = clock();
        int routed = routed_best(q, x, c, labels, &candidates, &jumps);
        route_ticks += clock() - start;
        total_candidates += candidates;
        total_jumps += jumps;
        if (routed < 0) { ok = 0; break; }
        if (routed == exact) hits++;
    }
    double bt = (double)brute_ticks / CLOCKS_PER_SEC;
    double rt = (double)route_ticks / CLOCKS_PER_SEC;
    printf("P1 brute:   time=%.6fs candidates/query=%d payload-bytes/query=%d\n",
           bt, N, N * PAYLOAD_BYTES);
    printf("P1 routed:  time=%.6fs candidates/query=%.1f payload-bytes/query=%.1f jumps/query=%.1f\n",
           rt, (double)total_candidates / QUERIES,
           (double)total_candidates * PAYLOAD_BYTES / QUERIES,
           (double)total_jumps / QUERIES);
    printf("P1 recall@1: %.3f (%d/%d), candidate reduction: %.1f%%\n",
           (double)hits / QUERIES, hits, QUERIES,
           100.0 * (1.0 - (double)total_candidates / (QUERIES * N)));
    printf("P1 decision: %s\n",
           hits == QUERIES
               ? "PASS: exact + candidate/payload reduction; latency not yet proven"
               : "REVIEW: route miss exists");

    free(x); free(c); free(labels); free(entries);
    return (ok && hits == QUERIES) ? 0 : 1;
}
